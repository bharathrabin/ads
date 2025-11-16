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
 */
static inline size_t calc_bucket_size(const hashmap *map)
{
    return 8 +                             // tophash array
           BUCKET_SIZE * map->key_size +   // keys
           BUCKET_SIZE * map->value_size + // values
           sizeof(char *);                 // overflow pointer
}

/**
 * Get pointer to the i-th bucket in the bucket array
 */
static inline char *get_bucket(const hashmap *map, char *buckets, size_t index)
{
    size_t bsize = calc_bucket_size(map);
    return buckets + (index * bsize);
}

/**
 * Get pointer to tophash array within a bucket
 */
static inline uint8_t *get_tophash(char *bucket)
{
    return (uint8_t *)bucket;
}

/**
 * Get pointer to keys array within a bucket
 */
static inline char *get_keys(char *bucket)
{
    return bucket + 8;
}

/**
 * Get pointer to the i-th key within a bucket
 */
static inline char *get_key(char *bucket, size_t key_size, size_t index)
{
    assert(index < BUCKET_SIZE);
    return get_keys(bucket) + (index * key_size);
}

/**
 * Get pointer to values array within a bucket
 */
static inline char *get_values(const hashmap *map, char *bucket)
{
    return bucket + 8 + (BUCKET_SIZE * map->key_size);
}

/**
 * Get pointer to the i-th value within a bucket
 */
static inline char *get_value(const hashmap *map, char *bucket, size_t index)
{
    assert(index < BUCKET_SIZE);
    return get_values(map, bucket) + (index * map->value_size);
}

/**
 * Get pointer to the overflow pointer within a bucket
 */
static inline char **get_overflow_ptr(const hashmap *map, char *bucket)
{
    return (char **)(bucket + 8 + (BUCKET_SIZE * map->key_size) + BUCKET_SIZE * map->value_size);
}

/**
 * Get the overflow bucket (returns NULL if no overflow)
 */
static inline char *get_overflow(const hashmap *map, char *bucket)
{
    char **overflow_ptr = get_overflow_ptr(map, bucket);
    return *overflow_ptr;
}

/**
 * Set the overflow bucket pointer
 */
static inline void set_overflow(const hashmap *map, char *bucket, char *overflow)
{
    char **overflow_ptr = get_overflow_ptr(map, bucket);
    *overflow_ptr = overflow;
}

/**
 * Extract the top 8 bits of a hash (for tophash array)
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
 * Calculate bucket index from hash
 */
static inline size_t bucket_index(uint64_t hash, size_t bucket_count)
{
    // Use lower bits to select bucket (bucket_count is power of 2)
    return hash & (bucket_count - 1);
}

static char *alloc_bucket(const hashmap *map)
{
    size_t bsize = calc_bucket_size(map);
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

static char *alloc_buckets(const hashmap *map, size_t count)
{
    size_t bsize = calc_bucket_size(map);
    char *buckets = (char *)calloc(count, bsize);
    if (!buckets)
    {
        return NULL;
    }
    for (size_t i = 0; i < count; i++)
    {
        char *bucket = get_bucket(map, buckets, i);
        uint8_t *tophash = get_tophash(bucket);
        for (int j = 0; j < BUCKET_SIZE; j++)
        {
            tophash[j] = EMPTY;
        }
    }
    return buckets;
}

/**
 * Create a map and allocate necessary memory
 */
hashmap *hashmap_create(size_t key_size, size_t value_size, hash_fn hash, equals_fn equals)
{
    if (key_size == 0 || value_size == 0 || !!hash || !equals)
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
 * Free all overflow buckets in a bucket chain
 */
static void free_overflow_chain(const hashmap *map, char *bucket)
{
    char *overflow = get_overflow(map, bucket);
    while (overflow)
    {
        char *next = get_overflow(map, overflow);
        free(overflow);
        overflow = next;
    }
}

/**
 * Destroy a map and free memory
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
            char *bucket = get_bucket(map, map->buckets, i);
            free_overflow_chain(map, bucket);
        }
        free(map->buckets);
    }
    if (map->old_buckets)
    {
        for (size_t i = 0; i < map->old_bucket_count; i++)
        {
            char *bucket = get_bucket(map, map->old_buckets, i);
            free_overflow_chain(map, bucket);
        }
        free(map->old_buckets);
    }
    free(map);
}

/**
 * Put a key and value pair into the map
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

    char *bucket = get_bucket(map, map->buckets, idx);
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
                char *existing_key = get_key(map, current_bucket, i);
                if (map->equals(existing_key, key, map->key_size))
                {
                    // Found existing key - update value
                    char *existing_value = get_value(map, current_bucket, i);
                    memcpy(existing_value, value, map->value_size);
                    return true;
                }
            }
        }
        last_bucket = current_bucket;
        current_bucket = get_overflow(map, current_bucket);
    }

    if (insert_slot == -1)
    {
        char *overflow = alloc_bucket(map);
        if (!overflow)
        {
            return false;
        }
        set_overflow(map, last_bucket, overflow);
        insert_bucket = overflow;
        insert_slot = 0;
    }
    uint8_t *tophash = get_tophash(insert_bucket);
    tophash[insert_slot] = top;

    char *key_dest = get_key(insert_bucket, map->key_size, insert_slot);
    memcpy(key_dest, key, map->key_size);

    char *value_dest = get_value(map, insert_bucket, insert_slot);
    memcpy(value_dest, value, map->value_size);

    map->count++;

    uint8_t *tophash = get_tophash(insert_bucket);
    tophash[insert_slot] = top;

    // Copy the key
    char *key_dest = get_key(insert_bucket, map->key_size, insert_slot);
    memcpy(key_dest, key, map->key_size);

    // Copy the value
    char *value_dest = get_value(map, insert_bucket, insert_slot);
    memcpy(value_dest, value, map->value_size);

    // Increment count
    map->count++;

    // Check if we need to grow (load factor check)
    if (map->count * LOAD_FACTOR_DENOMINATOR >
        map->bucket_count * BUCKET_SIZE * LOAD_FACTOR_NUMERATOR)
    {
        // TODO: Implement growth/rehashing
    }
    return true;
}
