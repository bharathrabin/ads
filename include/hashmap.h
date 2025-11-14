#ifndef HASHMAP_H
#define HASHMAP_H

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

typedef struct hashmap hashmap;

typedef uint64_t (*hash_fn)(const void *key, size_t key_size);

typedef bool (*equals_fn)(const void *a, const void *b, size_t key_size);

/**
 * Create a new hashmap
 *
 * @param key_size Size of keys in bytes
 * @param value_size Size of values in bytes
 * @param hash Hash function for keys
 * @param equals Equality function for keys
 * @param return New hashmap or NULL on failure
 */
hashmap hashmap_create(size_t key_size, size_t value_size, hash_fn hash, equals_fn equals);

/**
 * Insert or update a key-value pair
 *
 * @param map the hashmap
 * @param key Pointer to key data
 * @param value Pointer to value data
 * @return true on success, false on failure
 */
bool hashmap_put(hashmap *map, const void *key, const void *value);

/**
 * Retrieve a value by key
 *
 * @param map The hashmap
 * @param key Pointer to key data
 * @param value_out Pointer to store retrieved value (if found)
 * @return true if key found, false otherwise
 */

bool hashmap_get(hashmap *map, const void *key, void *value_out);

/**
 * Destroy the hasmap and free all memory
 * @param map hashmap to destroy
 */

void hashmap_destroy(hashmap *map);

#endif
