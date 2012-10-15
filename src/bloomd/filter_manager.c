#include <stdlib.h>
#include <stdio.h>
#include <syslog.h>
#include <pthread.h>
#include <dirent.h>
#include <string.h>
#include "spinlock.h"
#include "filter_manager.h"
#include "hashmap.h"
#include "filter.h"

/**
 * Wraps a bloom_filter to ensure only a single
 * writer access it at a time. Tracks the outstanding
 * references, to allow a sane close to take place.
 */
typedef struct {
    volatile int is_active;         // Set to 0 when we are trying to delete it
    volatile int is_hot;            // Used to mark a filter as hot
    volatile int should_delete;     // Used to control deletion

    bloom_filter *filter;    // The actual filter object
    pthread_rwlock_t rwlock; // Protects the filter
    bloom_config *custom;   // Custom config to cleanup
} bloom_filter_wrapper;

/**
 * We use a linked list of filtmgr_vsn structs
 * as a simple form of Multi-Version Concurrency Controll (MVCC).
 * The latest version is always the head of the list, and older
 * versions are maintained as a linked list. A separate vacuum thread
 * is used to clean out the old version. This allows reads against the
 * head to be non-blocking.
 */
typedef struct filtmgr_vsn {
    volatile int is_hot;  // Used to mark a version as hot
    unsigned long long vsn;

    // Maps key names -> bloom_filter_wrapper
    bloom_hashmap *filter_map;

    // Holds a reference to the deleted filter, since
    // it is no longer in the hash map
    bloom_filter_wrapper *deleted;
    struct filtmgr_vsn *prev;
} filtmgr_vsn;

struct bloom_filtmgr {
    bloom_config *config;
    filtmgr_vsn *latest;
    pthread_mutex_t write_lock; // Serializes destructive operations
};

struct filtmgr_thread_args {
    bloom_filtmgr *mgr;
    int *should_run;
};

/**
 * This is the time in seconds we wait
 * for a version to 'cool' before cleaning it up.
 */
#define VERSION_COOLDOWN 15

/*
 * Static declarations
 */
static const char FOLDER_PREFIX[] = "bloomd.";
static const int FOLDER_PREFIX_LEN = sizeof(FOLDER_PREFIX) - 1;

static bloom_filter_wrapper* take_filter(filtmgr_vsn *vsn, char *filter_name);
static void delete_filter(bloom_filter_wrapper *filt);
static int add_filter(bloom_filtmgr *mgr, filtmgr_vsn *vsn, char *filter_name, bloom_config *config, int is_hot);
static int filter_map_list_cb(void *data, const char *key, void *value);
static int filter_map_list_cold_cb(void *data, const char *key, void *value);
static int filter_map_delete_cb(void *data, const char *key, void *value);
static int load_existing_filters(bloom_filtmgr *mgr);
static filtmgr_vsn* create_new_version(bloom_filtmgr *mgr);
static void destroy_version(filtmgr_vsn *vsn);
static int copy_hash_entries(void *data, const char *key, void *value);
static void* filtmgr_thread_main(void *in);

/**
 * Initializer
 * @arg config The configuration
 * @arg mgr Output, resulting manager.
 * @return 0 on success.
 */
int init_filter_manager(bloom_config *config, bloom_filtmgr **mgr) {
    // Allocate a new object
    bloom_filtmgr *m = *mgr = calloc(1, sizeof(bloom_filtmgr));

    // Copy the config
    m->config = config;

    // Initialize the write lock
    pthread_mutex_init(&m->write_lock, NULL);

    // Allocate the initial version and hash table
    filtmgr_vsn *vsn = calloc(1, sizeof(filtmgr_vsn));
    m->latest = vsn;
    int res = hashmap_init(0, &vsn->filter_map);
    if (res) {
        syslog(LOG_ERR, "Failed to allocate filter hash map!");
        free(m);
        return -1;
    }

    // Discover existing filters
    load_existing_filters(m);

    // Done
    return 0;
}

/**
 * Cleanup
 * @arg mgr The manager to destroy
 * @return 0 on success.
 */
int destroy_filter_manager(bloom_filtmgr *mgr) {
    // Nuke all the keys in the current version
    filtmgr_vsn *current = mgr->latest;
    hashmap_iter(current->filter_map, filter_map_delete_cb, mgr);

    // Destroy the versions
    filtmgr_vsn *next, *vsn = mgr->latest;
    while (vsn) {
        // Handle any lingering deletes
        if (vsn->deleted) delete_filter(vsn->deleted);
        next = vsn->prev;
        destroy_version(vsn);
        vsn = next;
    }

    // Free the manager
    free(mgr);
    return 0;
}

/**
 * Starts the filter managers passive thread. This must
 * be started after initializing the filter manager to cleanup
 * internal state.
 * @arg mgr The manager to monitor
 * @arg should_run An integer set to 0 when we should terminate
 * @return The pthread_t handle of the thread. Used for joining.
 */
pthread_t filtmgr_start_worker(bloom_filtmgr *mgr, int *should_run) {
    struct filtmgr_thread_args* args = malloc(sizeof(struct filtmgr_thread_args));
    args->mgr = mgr;
    args->should_run = should_run;
    pthread_t t;
    if (pthread_create(&t, NULL, filtmgr_thread_main, args)) {
        free(args);
        return 0;
    }
    return t;
}

/**
 * Flushes the filter with the given name
 * @arg filter_name The name of the filter to flush
 * @return 0 on success. -1 if the filter does not exist.
 */
int filtmgr_flush_filter(bloom_filtmgr *mgr, char *filter_name) {
    // Get the filter
    filtmgr_vsn *current = mgr->latest;
    bloom_filter_wrapper *filt = take_filter(current, filter_name);
    if (!filt) return -1;

    // Flush
    bloomf_flush(filt->filter);
    return 0;
}

/**
 * Checks for the presence of keys in a given filter
 * @arg filter_name The name of the filter containing the keys
 * @arg keys A list of points to character arrays to check
 * @arg num_keys The number of keys to check
 * @arg result Ouput array, stores a 0 if the key does not exist
 * or 1 if the key does exist.
 * @return 0 on success, -1 if the filter does not exist.
 * -2 on internal error.
 */
int filtmgr_check_keys(bloom_filtmgr *mgr, char *filter_name, char **keys, int num_keys, char *result) {
    // Get the filter
    filtmgr_vsn *current = mgr->latest;
    bloom_filter_wrapper *filt = take_filter(current, filter_name);
    if (!filt) return -1;

    // Acquire the write lock
    pthread_rwlock_rdlock(&filt->rwlock);

    // Check the keys, store the results
    int res = 0;
    for (int i=0; i<num_keys; i++) {
        res = bloomf_contains(filt->filter, keys[i]);
        if (res == -1) break;
        *(result+i) = res;
    }

    // Mark as hot
    filt->is_hot = 1;

    // Release the lock
    pthread_rwlock_unlock(&filt->rwlock);
    return (res == -1) ? -2 : 0;
}

/**
 * Sets keys in a given filter
 * @arg filter_name The name of the filter
 * @arg keys A list of points to character arrays to add
 * @arg num_keys The number of keys to add
 * @arg result Ouput array, stores a 0 if the key already is set
 * or 1 if the key is set.
 * * @return 0 on success, -1 if the filter does not exist.
 * -2 on internal error.
 */
int filtmgr_set_keys(bloom_filtmgr *mgr, char *filter_name, char **keys, int num_keys, char *result) {
    // Get the filter
    filtmgr_vsn *current = mgr->latest;
    bloom_filter_wrapper *filt = take_filter(current, filter_name);
    if (!filt) return -1;

    // Acquire the write lock
    pthread_rwlock_wrlock(&filt->rwlock);

    // Set the keys, store the results
    int res = 0;
    for (int i=0; i<num_keys; i++) {
        res = bloomf_add(filt->filter, keys[i]);
        if (res == -1) break;
        *(result+i) = res;
    }

    // Mark as hot
    filt->is_hot = 1;

    // Release the lock
    pthread_rwlock_unlock(&filt->rwlock);
    return (res == -1) ? -2 : 0;
}

/**
 * Creates a new filter of the given name and parameters.
 * @arg filter_name The name of the filter
 * @arg custom_config Optional, can be null. Configs that override the defaults.
 * @return 0 on success, -1 if the filter already exists.
 * -2 for internal error.
 */
int filtmgr_create_filter(bloom_filtmgr *mgr, char *filter_name, bloom_config *custom_config) {
    // Lock the creation
    pthread_mutex_lock(&mgr->write_lock);

    // Bail if the filter already exists
    bloom_filter_wrapper *filt = NULL;
    filtmgr_vsn *latest = mgr->latest;
    latest->is_hot = 1;
    hashmap_get(latest->filter_map, filter_name, (void**)&filt);
    if (filt) {
        pthread_mutex_unlock(&mgr->write_lock);
        return -1;
    }

    // Create a new version
    filtmgr_vsn *new_vsn = create_new_version(mgr);

    // Use a custom config if provided, else the default
    bloom_config *config = (custom_config) ? custom_config : mgr->config;

    // Add the filter to the new version
    int res = add_filter(mgr, new_vsn, filter_name, config, 1);
    if (res != 0) {
        destroy_version(new_vsn);
        res = -2; // Internal error
    } else {
        // Install the new version
        mgr->latest = new_vsn;
    }

    // Release the lock
    pthread_mutex_unlock(&mgr->write_lock);
    return res;
}

/**
 * Deletes the filter entirely. This removes it from the filter
 * manager and deletes it from disk. This is a permanent operation.
 * @arg filter_name The name of the filter to delete
 * @return 0 on success, -1 if the filter does not exist.
 */
int filtmgr_drop_filter(bloom_filtmgr *mgr, char *filter_name) {
    // Lock the deletion
    pthread_mutex_lock(&mgr->write_lock);

    // Get the filter
    filtmgr_vsn *current = mgr->latest;
    bloom_filter_wrapper *filt = take_filter(current, filter_name);
    if (!filt) {
        pthread_mutex_unlock(&mgr->write_lock);
        return -1;
    }

    // Set the filter to be non-active and mark for deletion
    filt->is_active = 0;
    filt->should_delete = 1;

    // Create a new version without this filter
    filtmgr_vsn *new_vsn = create_new_version(mgr);
    hashmap_delete(new_vsn->filter_map, filter_name);
    current->deleted = filt;

    // Install the new version
    mgr->latest = new_vsn;

    // Unlock
    pthread_mutex_unlock(&mgr->write_lock);
    return 0;
}

/**
 * Unmaps the filter from memory, but leaves it
 * registered in the filter manager. This is rarely invoked
 * by a client, as it can be handled automatically by bloomd,
 * but particular clients with specific needs may use it as an
 * optimization.
 * @arg filter_name The name of the filter to delete
 * @return 0 on success, -1 if the filter does not exist.
 */
int filtmgr_unmap_filter(bloom_filtmgr *mgr, char *filter_name) {
    // Get the filter
    filtmgr_vsn *current = mgr->latest;
    bloom_filter_wrapper *filt = take_filter(current, filter_name);
    if (!filt) return -1;

    // Only do it if we are not in memory
    if (!filt->filter->filter_config.in_memory) {
        // Acquire the write lock
        pthread_rwlock_wrlock(&filt->rwlock);

        // Close the filter
        bloomf_close(filt->filter);

        // Release the lock
        pthread_rwlock_unlock(&filt->rwlock);
    }

    return 0;
}


/**
 * Clears the filter from the internal data stores. This can only
 * be performed if the filter is proxied.
 * @arg filter_name The name of the filter to delete
 * @return 0 on success, -1 if the filter does not exist, -2
 * if the filter is not proxied.
 */
int filtmgr_clear_filter(bloom_filtmgr *mgr, char *filter_name) {
    // Lock the deletion
    pthread_mutex_lock(&mgr->write_lock);

    // Get the filter
    filtmgr_vsn *current = mgr->latest;
    bloom_filter_wrapper *filt = take_filter(current, filter_name);
    if (!filt) {
        pthread_mutex_unlock(&mgr->write_lock);
        return -1;
    }

    // Check if the filter is proxied
    if (!bloomf_is_proxied(filt->filter)) {
        pthread_mutex_unlock(&mgr->write_lock);
        return -2;
    }

    // This is critical, as it prevents it from
    // being deleted. Instead, it is merely closed.
    filt->is_active = 0;
    filt->should_delete = 0;

    // Create a new version without this filter
    filtmgr_vsn *new_vsn = create_new_version(mgr);
    hashmap_delete(new_vsn->filter_map, filter_name);
    current->deleted = filt;

    // Install the new version
    mgr->latest = new_vsn;

    // Unlock
    pthread_mutex_unlock(&mgr->write_lock);
    return 0;
}


/**
 * Allocates space for and returns a linked
 * list of all the filters.
 * @arg mgr The manager to list from
 * @arg head Output, sets to the address of the list header
 * @return 0 on success.
 */
int filtmgr_list_filters(bloom_filtmgr *mgr, bloom_filter_list_head **head) {
    // Allocate the head
    bloom_filter_list_head *h = *head = calloc(1, sizeof(bloom_filter_list_head));

    // Iterate through a callback to append
    filtmgr_vsn *current = mgr->latest;
    current->is_hot = 1;
    hashmap_iter(current->filter_map, filter_map_list_cb, h);
    return 0;
}


/**
 * Allocates space for and returns a linked
 * list of all the cold filters. This has the side effect
 * of clearing the list of cold filters!
 * @arg mgr The manager to list from
 * @arg head Output, sets to the address of the list header
 * @return 0 on success.
 */
int filtmgr_list_cold_filters(bloom_filtmgr *mgr, bloom_filter_list_head **head) {
    // Allocate the head of a new hashmap
    bloom_filter_list_head *h = *head = calloc(1, sizeof(bloom_filter_list_head));

    // Scan for the cold filters
    filtmgr_vsn *current = mgr->latest;
    current->is_hot = 1;
    hashmap_iter(current->filter_map, filter_map_list_cold_cb, h);
    return 0;
}


/**
 * This method allows a callback function to be invoked with bloom filter.
 * The purpose of this is to ensure that a bloom filter is not deleted or
 * otherwise destroyed while being referenced. The filter is not locked
 * so clients should under no circumstance use this to read/write to the filter.
 * It should be used to read metrics, size information, etc.
 * @return 0 on success, -1 if the filter does not exist.
 */
int filtmgr_filter_cb(bloom_filtmgr *mgr, char *filter_name, filter_cb cb, void* data) {
    // Get the filter
    filtmgr_vsn *current = mgr->latest;
    current->is_hot = 1;
    bloom_filter_wrapper *filt = take_filter(current, filter_name);
    if (!filt) return -1;

    // Callback
    cb(data, filter_name, filt->filter);
    return 0;
}


/**
 * Convenience method to cleanup a filter list.
 */
void filtmgr_cleanup_list(bloom_filter_list_head *head) {
    bloom_filter_list *next, *current = head->head;
    while (current) {
        next = current->next;
        free(current->filter_name);
        free(current);
        current = next;
    }
    free(head);
}


/**
 * Gets the bloom filter in a thread safe way.
 */
static bloom_filter_wrapper* take_filter(filtmgr_vsn *vsn, char *filter_name) {
    vsn->is_hot = 1;  // Mark as hot
    bloom_filter_wrapper *filt = NULL;
    hashmap_get(vsn->filter_map, filter_name, (void**)&filt);
    return (filt && filt->is_active) ? filt : NULL;
}


/**
 * Invoked to cleanup a filter once we
 * have hit 0 remaining references.
 */
static void delete_filter(bloom_filter_wrapper *filt) {
    // Delete or Close the filter
    if (filt->should_delete)
        bloomf_delete(filt->filter);
    else
        bloomf_close(filt->filter);

    // Cleanup the filter
    destroy_bloom_filter(filt->filter);

    // Release any custom configs
    if (filt->custom) {
        free(filt->custom);
    }

    // Release the struct
    free(filt);
    return;
}

/**
 * Creates a new filter and adds it to the filter set.
 * @arg mgr The manager to add to
 * @arg vsn The version to add to
 * @arg filter_name The name of the filter
 * @arg config The configuration for the filter
 * @arg is_hot Is the filter hot. False for existing.
 * @return 0 on success, -1 on error
 */
static int add_filter(bloom_filtmgr *mgr, filtmgr_vsn *vsn, char *filter_name, bloom_config *config, int is_hot) {
    // Create the filter
    bloom_filter_wrapper *filt = calloc(1, sizeof(bloom_filter_wrapper));
    filt->is_active = 1;
    filt->is_hot = is_hot;
    filt->should_delete = 0;
    pthread_rwlock_init(&filt->rwlock, NULL);

    // Set the custom filter if its not the same
    if (mgr->config != config) {
        filt->custom = config;
    }

    // Try to create the underlying filter. Only discover if it is hot.
    int res = init_bloom_filter(config, filter_name, is_hot, &filt->filter);
    if (res != 0) {
        free(filt);
        return -1;
    }

    // Add to the hash map
    if (!hashmap_put(vsn->filter_map, filter_name, filt)) {
        destroy_bloom_filter(filt->filter);
        free(filt);
        return -1;
    }
    return 0;
}

/**
 * Called as part of the hashmap callback
 * to list all the filters. Only works if value is
 * not NULL.
 */
static int filter_map_list_cb(void *data, const char *key, void *value) {
    // Filter out the non-active nodes
    bloom_filter_wrapper *filt = value;
    if (!filt->is_active) return 0;

    // Cast the inputs
    bloom_filter_list_head *head = data;

    // Allocate a new entry
    bloom_filter_list *node = malloc(sizeof(bloom_filter_list));

    // Setup
    node->filter_name = strdup(key);
    node->next = head->head;

    // Inject
    head->head = node;
    head->size++;
    return 0;
}

/**
 * Called as part of the hashmap callback
 * to list cold filters. Only works if value is
 * not NULL.
 */
static int filter_map_list_cold_cb(void *data, const char *key, void *value) {
    // Cast the inputs
    bloom_filter_list_head *head = data;
    bloom_filter_wrapper *filt = value;

    // Check if hot, turn off and skip
    if (filt->is_hot) {
        filt->is_hot = 0;
        return 0;
    }

    // Check if proxied
    if (bloomf_is_proxied(filt->filter)) {
        return 0;
    }

    // Allocate a new entry
    bloom_filter_list *node = malloc(sizeof(bloom_filter_list));

    // Setup
    node->filter_name = strdup(key);
    node->next = head->head;

    // Inject
    head->head = node;
    head->size++;
    return 0;
}

/**
 * Called as part of the hashmap callback
 * to cleanup the filters.
 */
static int filter_map_delete_cb(void *data, const char *key, void *value) {
    // Cast the inputs
    bloom_filter_wrapper *filt = value;

    // Delete, but not the underlying files
    filt->should_delete = 0;
    delete_filter(filt);
    return 0;
}

/**
 * Works with scandir to filter out non-bloomd folders.
 */
#ifndef __linux__
static int filter_bloomd_folders(struct dirent *d) {
#else
static int filter_bloomd_folders(const struct dirent *d) {
#endif
    // Get the file name
    char *name = (char*)d->d_name;

    // Look if it ends in ".data"
    int name_len = strlen(name);

    // Too short
    if (name_len < 8) return 0;

    // Compare the prefix
    if (strncmp(name, FOLDER_PREFIX, FOLDER_PREFIX_LEN) == 0) {
        return 1;
    }

    // Do not store
    return 0;
}

/**
 * Loads the existing filters. This is not thread
 * safe and assumes that we are being initialized.
 */
static int load_existing_filters(bloom_filtmgr *mgr) {
    struct dirent **namelist;
    int num;

    num = scandir(mgr->config->data_dir, &namelist, filter_bloomd_folders, NULL);
    if (num == -1) {
        syslog(LOG_ERR, "Failed to scan files for existing filters!");
        return -1;
    }
    syslog(LOG_INFO, "Found %d existing filters", num);

    // Add all the filters
    for (int i=0; i< num; i++) {
        char *folder_name = namelist[i]->d_name;
        char *filter_name = folder_name + FOLDER_PREFIX_LEN;
        if (add_filter(mgr, mgr->latest, filter_name, mgr->config, 0)) {
            syslog(LOG_ERR, "Failed to load filter '%s'!", filter_name);
        }
    }

    for (int i=0; i < num; i++) free(namelist[i]);
    free(namelist);
    return 0;
}


/**
 * Creates a new version struct from the current version.
 * Does not install the new version in place. This should
 * be guarded by the write lock to prevent conflicting versions.
 */
static filtmgr_vsn* create_new_version(bloom_filtmgr *mgr) {
    // Create a new blank version
    filtmgr_vsn *vsn = calloc(1, sizeof(filtmgr_vsn));
    vsn->is_hot = 1;

    // Increment the version number
    filtmgr_vsn *current = mgr->latest;
    vsn->vsn = mgr->latest->vsn + 1;

    // Set the previous pointer
    vsn->prev = current;

    // Initialize the hashmap
    int res = hashmap_init(hashmap_size(current->filter_map), &vsn->filter_map);
    if (res) {
        syslog(LOG_ERR, "Failed to allocate new filter hash map!");
        free(vsn);
        return NULL;
    }

    // Copy old keys, this is likely a bottle neck...
    // We need to make this more efficient in the future.
    res = hashmap_iter(current->filter_map, copy_hash_entries, vsn->filter_map);
    if (res != 0) {
        syslog(LOG_ERR, "Failed to copy filter hash map!");
        hashmap_destroy(vsn->filter_map);
        free(vsn);
        return NULL;
    }

    // Return the new version
    syslog(LOG_DEBUG, "(FiltMgr) Created new version %llu", vsn->vsn);
    return vsn;
}

// Destroys a version. Does basic cleanup.
static void destroy_version(filtmgr_vsn *vsn) {
    hashmap_destroy(vsn->filter_map);
    free(vsn);
}

// Copies entries from an existing map into a new one
static int copy_hash_entries(void *data, const char *key, void *value) {
    bloom_hashmap *new = data;
    return (hashmap_put(new, (char*)key, value) ? 0 : 1);
}


// Recursively waits and cleans up old versions
static int clean_old_versions(filtmgr_vsn *old, int *should_run) {
    // Recurse if possible
    if (old->prev) clean_old_versions(old->prev, should_run);
    if (!*should_run) return 0;
    syslog(LOG_DEBUG, "(FiltMgr) Waiting to clean version %llu", old->vsn);

    // Wait for this version to become 'cold'
    do {
        old->is_hot = 0;
        sleep(VERSION_COOLDOWN);
    } while (*should_run && old->is_hot);
    if (!*should_run) return 0;

    // Handle the cleanup
    if (old->deleted) {
        delete_filter(old->deleted);
    }

    // Destroy this version
    syslog(LOG_DEBUG, "(FiltMgr) Destroying version %llu", old->vsn);
    destroy_version(old);
    return 0;
}


/**
 * This thread is started after initialization to maintain
 * the state of the filter manager. It's current use is to
 * cleanup the garbage created by our MVCC model. It does this
 * by setting the `is_hot` field to 0 and remembering the latest
 * version. It then deletes any cold versions. This is definitely
 * a somewhat heuristic approach, and is NOT 100% guarenteed. In
 * particular, it IS possible that an operation exceeds 2 check cycles
 * and gets deleted while in use. There is no good way for us to
 * currently detect and avoid this. There is a certain burden to
 * be thus conservative with our timing, and to always set is_hot.
 */
static void* filtmgr_thread_main(void *in) {
    // Extract our arguments
    struct filtmgr_thread_args* args = in;
    bloom_filtmgr *mgr = args->mgr;
    int *should_run = args->should_run;
    free(args);

    // Store the last version
    unsigned long long last_vsn = 0;
    filtmgr_vsn *current;
    while (*should_run) {
        sleep(1);

        // Do nothing if the version has not changed
        current = mgr->latest;
        if (current->vsn == last_vsn)
            continue;
        else if (*should_run) {
            last_vsn = current->vsn;

            // Cleanup the old versions
            clean_old_versions(current->prev, should_run);
            current->prev = NULL;
        }
    }

    return NULL;
}

