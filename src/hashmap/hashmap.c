#include "hashmap.h"
#include <stdint.h>
#include <stdlib.h>

// Initial number of buckets (matches Go's implementation)
#define INITIAL_BUCKET_COUNT 8

// Number of key-value pairs per bucket (matches Go's implementation)
#define BUCKET_SIZE 8

// Load factor threshold for growth (matches Go's 6.5 load factor)
#define LOAD_FACTOR_NUMERATOR 13
#define LOAD_FACTOR_DENOMINATOR 2

// Empty tophash value
#define EMPTY 0

struct hashmap
{
    size_t key_size;
    size_t value_size;
    hash_fn hash;
    equals_fn equals;

    char *buckets;
    size_t bucket_count;
    size_t count;
    size_t hash_seed;

    // incremental rehashing
    char *old_buckets;
    size_t old_bucket_count;
    size_t evacuated;
};

/**
 * Calculate the total size of a single bucket in bytes
 * Layout: [tophash:8][keys:8*key_size][values:8*value_size][overflow:8]
 *
 * @param key_size Size of keys in bytes
 * @param value_size Size of values in bytes
 * @return Total size of one bucket in bytes
 */
static inline size_t calc_bucket_size(size_t key_size, size_t value_size)
{
    return BUCKET_SIZE * sizeof(uint8_t) + // tophash array: 8 × 1 byte
           BUCKET_SIZE * key_size +        // keys: 8 × key_size
           BUCKET_SIZE * value_size +      // values: 8 × value_size
           sizeof(char *);                 // overflow pointer
}

/**
 * Get pointer to the i-th bucket in the bucket array
 *
 * @param buckets Pointer to the bucket array
 * @param key_size Size of keys in bytes
 * @param value_size Size of values in bytes
 * @param index Index of the bucket to retrieve
 * @return Pointer to the specified bucket
 */
static inline char *get_bucket(char *buckets, size_t key_size, size_t value_size, size_t index)
{
    size_t bsize = calc_bucket_size(key_size, value_size);
    return buckets + (index * bsize);
}

/**
 * Get pointer to tophash array within a bucket
 *
 * @param bucket Pointer to the bucket
 * @return Pointer to the tophash array (8 bytes)
 */
static inline uint8_t *get_tophash(char *bucket)
{
    return (uint8_t *)bucket;
}

/**
 * Get pointer to keys array within a bucket
 *
 * @param bucket Pointer to the bucket
 * @return Pointer to the start of the keys array
 */
static inline char *get_keys(char *bucket)
{
    return bucket + 8;
}

/**
 * Get pointer to the i-th key within a bucket
 *
 * @param bucket Pointer to the bucket
 * @param key_size Size of keys in bytes
 * @param index Index of the key (0-7)
 * @return Pointer to the specified key
 */
static inline char *get_key(char *bucket, size_t key_size, size_t index)
{
    assert(index < BUCKET_SIZE);
    return get_keys(bucket) + (index * key_size);
}

/**
 * Get pointer to values array within a bucket
 *
 * @param bucket Pointer to the bucket
 * @param key_size Size of keys in bytes
 * @return Pointer to the start of the values array
 */
static inline char *get_values(char *bucket, size_t key_size)
{
    return bucket + 8 + (BUCKET_SIZE * key_size);
}

/**
 * Get pointer to the i-th value within a bucket
 *
 * @param bucket Pointer to the bucket
 * @param key_size Size of keys in bytes
 * @param value_size Size of values in bytes
 * @param index Index of the value (0-7)
 * @return Pointer to the specified value
 */
static inline char *get_value(char *bucket, size_t key_size, size_t value_size, size_t index)
{
    assert(index < BUCKET_SIZE);
    return get_values(bucket, key_size) + (index * value_size);
}

/**
 * Get pointer to the overflow pointer within a bucket
 *
 * @param bucket Pointer to the bucket
 * @param key_size Size of keys in bytes
 * @param value_size Size of values in bytes
 * @return Pointer to the overflow pointer field
 */
static inline char **get_overflow_ptr(char *bucket, size_t key_size, size_t value_size)
{
    return (char **)(bucket + 8 + (BUCKET_SIZE * key_size) + BUCKET_SIZE * value_size);
}

/**
 * Get the overflow bucket (returns NULL if no overflow)
 *
 * @param bucket Pointer to the bucket
 * @param key_size Size of keys in bytes
 * @param value_size Size of values in bytes
 * @return Pointer to overflow bucket, or NULL if none exists
 */
static inline char *get_overflow(char *bucket, size_t key_size, size_t value_size)
{
    char **overflow_ptr = get_overflow_ptr(bucket, key_size, value_size);
    return *overflow_ptr;
}

/**
 * Set the overflow bucket pointer
 *
 * @param bucket Pointer to the bucket
 * @param overflow Pointer to the overflow bucket to link
 * @param key_size Size of keys in bytes
 * @param value_size Size of values in bytes
 */
static inline void set_overflow(char *bucket, char *overflow, size_t key_size, size_t value_size)
{
    char **overflow_ptr = get_overflow_ptr(bucket, key_size, value_size);
    *overflow_ptr = overflow;
}

/**
 * Extract the top 8 bits of a hash value for use in the tophash array.
 * Returns a value >= 1 (since 0 is reserved for EMPTY).
 *
 * @param hash The full 64-bit hash value
 * @return Top 8 bits, adjusted to be at least 1
 */
static inline uint8_t top_hash(uint64_t hash)
{
    uint8_t top = (uint8_t)(hash >> 56);
    if (top < 1)
    {
        top = 1;
    }
    return top;
}

/**
 * Calculate which bucket a hash value maps to.
 * Uses bitwise AND with (bucket_count - 1) since bucket_count is a power of 2.
 *
 * @param hash The full 64-bit hash value
 * @param bucket_count Number of buckets (must be power of 2)
 * @return Bucket index in range [0, bucket_count)
 */
static inline size_t bucket_index(uint64_t hash, size_t bucket_count)
{
    // Use lower bits to select bucket (bucket_count is power of 2)
    return hash & (bucket_count - 1);
}

/**
 * Allocate and initialize a single bucket.
 * All tophash entries are set to EMPTY and overflow pointer is NULL (via calloc).
 *
 * @param key_size Size of keys in bytes
 * @param value_size Size of values in bytes
 * @return Pointer to newly allocated bucket, or NULL on allocation failure
 */
static char *alloc_bucket(size_t key_size, size_t value_size)
{
    size_t bsize = calc_bucket_size(key_size, value_size);
    char *bucket = (char *)calloc(1, bsize);
    if (!bucket)
    {
        return NULL;
    }
    uint8_t *tophash = get_tophash(bucket);
    for (int i = 0; i < BUCKET_SIZE; i++)
    {
        tophash[i] = EMPTY;
    }
    return bucket;
}

/**
 * Allocate and initialize an array of buckets.
 * All tophash entries in all buckets are set to EMPTY.
 *
 * @param map The hashmap (used for key_size and value_size)
 * @param count Number of buckets to allocate
 * @return Pointer to newly allocated bucket array, or NULL on allocation failure
 */
static char *alloc_buckets(const hashmap *map, size_t count)
{
    size_t bsize = calc_bucket_size(map->key_size, map->value_size);
    char *buckets = (char *)calloc(count, bsize);
    if (!buckets)
    {
        return NULL;
    }
    for (size_t i = 0; i < count; i++)
    {
        char *bucket = get_bucket(buckets, map->key_size, map->value_size, i);
        uint8_t *tophash = get_tophash(bucket);
        for (int j = 0; j < BUCKET_SIZE; j++)
        {
            tophash[j] = EMPTY;
        }
    }
    return buckets;
}

/**
 * Create a hashmap and allocate necessary memory.
 * Initializes with INITIAL_BUCKET_COUNT buckets.
 *
 * @param key_size Size of keys in bytes (must be > 0)
 * @param value_size Size of values in bytes (must be > 0)
 * @param hash Hash function pointer (must not be NULL)
 * @param equals Equality comparison function pointer (must not be NULL)
 * @return Pointer to newly created hashmap, or NULL on failure
 */
hashmap *hashmap_create(size_t key_size, size_t value_size, hash_fn hash, equals_fn equals)
{
    if (key_size == 0 || value_size == 0 || !hash || !equals)
    {
        return NULL;
    }
    // Initial allocation on the heap
    hashmap *map = (hashmap *)malloc(sizeof(hashmap));
    if (!map)
    {
        return NULL;
    }

    map->key_size = key_size;
    map->value_size = value_size;
    map->hash = hash;
    map->equals = equals;
    map->bucket_count = INITIAL_BUCKET_COUNT;
    map->count = 0;
    map->hash_seed = (uint32_t)time(NULL);

    map->buckets = alloc_buckets(map, INITIAL_BUCKET_COUNT);
    if (!map->buckets)
    {
        free(map);
        return NULL;
    }
    map->old_buckets = NULL;
    map->old_bucket_count = 0;
    map->evacuated = 0;

    return map;
}

/**
 * Free all overflow buckets in a chain starting from the given bucket.
 * Does not free the bucket itself, only its overflow chain.
 *
 * @param bucket Pointer to the bucket whose overflow chain should be freed
 * @param key_size Size of keys in bytes
 * @param value_size Size of values in bytes
 */
static void free_overflow_chain(char *bucket, size_t key_size, size_t value_size)
{
    char *overflow = get_overflow(bucket, key_size, value_size);
    while (overflow)
    {
        char *next = get_overflow(overflow, key_size, value_size);
        free(overflow);
        overflow = next;
    }
}

/**
 * Destroy a hashmap and free all associated memory.
 * Frees all buckets, overflow chains, and the hashmap structure itself.
 * Safe to call with NULL pointer (no-op).
 *
 * @param map Pointer to the hashmap to destroy
 */
void hashmap_destroy(hashmap *map)
{
    if (!map)
    {
        return;
    }
    if (map->buckets)
    {
        for (size_t i = 0; i < map->bucket_count; i++)
        {
            char *bucket = get_bucket(map->buckets, map->key_size, map->value_size, i);
            free_overflow_chain(bucket, map->key_size, map->value_size);
        }
        free(map->buckets);
    }
    if (map->old_buckets)
    {
        for (size_t i = 0; i < map->old_bucket_count; i++)
        {
            char *bucket = get_bucket(map->old_buckets, map->key_size, map->value_size, i);
            free_overflow_chain(bucket, map->key_size, map->value_size);
        }
        free(map->old_buckets);
    }
    free(map);
}

/**
 * Insert or update a key-value pair in the hashmap.
 * If key exists, its value is updated. Otherwise, a new entry is created.
 * May allocate overflow buckets if the target bucket is full.
 * Checks load factor after insertion (growth not yet implemented).
 *
 * @param map Pointer to the hashmap
 * @param key Pointer to the key to insert
 * @param value Pointer to the value to associate with the key
 * @return true on success, false on failure (NULL parameters or allocation failure)
 */
bool hashmap_put(hashmap *map, const void *key, const void *value)
{
    if (!map || !key || !value)
    {
        return false;
    }

    uint64_t hash = map->hash(key, map->key_size);
    uint8_t top = top_hash(hash);
    size_t idx = bucket_index(hash, map->bucket_count);

    char *bucket = get_bucket(map->buckets, map->key_size, map->value_size, idx);
    char *insert_bucket = bucket;
    int insert_slot = -1;

    char *current_bucket = bucket;
    char *last_bucket = bucket;
    while (current_bucket)
    {
        uint8_t *tophash = get_tophash(current_bucket);
        for (int i = 0; i < BUCKET_SIZE; i++)
        {
            // Remember first empty slot we find
            if (tophash[i] == EMPTY)
            {
                if (insert_slot == -1)
                {
                    insert_bucket = current_bucket;
                    insert_slot = i;
                }
                // Continue searching for existing key
                continue;
            }

            // Check if this slot matches our key
            if (tophash[i] == top)
            {
                char *existing_key = get_key(current_bucket, map->key_size, i);
                if (map->equals(existing_key, key, map->key_size))
                {
                    // Found existing key - update value
                    char *existing_value = get_value(current_bucket, map->key_size, map->value_size, i);
                    memcpy(existing_value, value, map->value_size);
                    return true;
                }
            }
        }
        last_bucket = current_bucket;
        current_bucket = get_overflow(current_bucket, map->key_size, map->value_size);
    }

    if (insert_slot == -1)
    {
        char *overflow = alloc_bucket(map->key_size, map->value_size);
        if (!overflow)
        {
            return false;
        }
        set_overflow(last_bucket, overflow, map->key_size, map->value_size);
        insert_bucket = overflow;
        insert_slot = 0;
    }
    uint8_t *tophash = get_tophash(insert_bucket);
    tophash[insert_slot] = top;

    char *key_dest = get_key(insert_bucket, map->key_size, insert_slot);
    memcpy(key_dest, key, map->key_size);

    char *value_dest = get_value(insert_bucket, map->key_size, map->value_size, insert_slot);
    memcpy(value_dest, value, map->value_size);

    map->count++;

    // Check if we need to grow (load factor check)
    if (map->count * LOAD_FACTOR_DENOMINATOR >
        map->bucket_count * BUCKET_SIZE * LOAD_FACTOR_NUMERATOR)
    {
        // TODO: Implement growth/rehashing
    }
    return true;
}