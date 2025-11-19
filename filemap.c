#include "filemap.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Hash function (djb2 algorithm)
static unsigned long hash_string(const char* str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    }
    return hash % HASHMAP_SIZE;
}

// Initialize hashmap
void filemap_init(FileHashMap* map) {
    for (int i = 0; i < HASHMAP_SIZE; i++) {
        map->buckets[i] = NULL;
        pthread_mutex_init(&map->locks[i], NULL);
    }
    map->count = 0;
    pthread_mutex_init(&map->count_mutex, NULL);
}

// Initialize LRU cache
void cache_init(LRUCache* cache) {
    cache->head = NULL;
    cache->tail = NULL;
    cache->count = 0;
    for (int i = 0; i < CACHE_SIZE; i++) {
        cache->nodes[i] = NULL;
    }
    pthread_mutex_init(&cache->cache_mutex, NULL);
}

// Insert or update file in hashmap - O(1) average
bool filemap_put(FileHashMap* map, const char* filename, FileMetadata* file) {
    unsigned long hash = hash_string(filename);
    
    pthread_mutex_lock(&map->locks[hash]);
    
    // Check if file already exists (update case)
    FileNode* current = map->buckets[hash];
    while (current != NULL) {
        if (strcmp(current->key, filename) == 0) {
            // Update existing entry
            current->file = file;
            pthread_mutex_unlock(&map->locks[hash]);
            return true;
        }
        current = current->next;
    }
    
    // Insert new entry
    FileNode* new_node = (FileNode*)malloc(sizeof(FileNode));
    if (new_node == NULL) {
        pthread_mutex_unlock(&map->locks[hash]);
        return false;
    }
    
    strcpy(new_node->key, filename);
    new_node->file = file;
    new_node->next = map->buckets[hash];
    map->buckets[hash] = new_node;
    
    pthread_mutex_unlock(&map->locks[hash]);
    
    // Update count
    pthread_mutex_lock(&map->count_mutex);
    map->count++;
    pthread_mutex_unlock(&map->count_mutex);
    
    return true;
}

// Get file from hashmap - O(1) average
FileMetadata* filemap_get(FileHashMap* map, const char* filename) {
    unsigned long hash = hash_string(filename);
    
    pthread_mutex_lock(&map->locks[hash]);
    
    FileNode* current = map->buckets[hash];
    while (current != NULL) {
        if (strcmp(current->key, filename) == 0) {
            FileMetadata* result = current->file;
            pthread_mutex_unlock(&map->locks[hash]);
            return result;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&map->locks[hash]);
    return NULL; // Not found
}

// Move node to front of LRU cache
static void cache_move_to_front(LRUCache* cache, CacheNode* node) {
    if (node == cache->head) return; // Already at front
    
    // Remove from current position
    if (node->prev) node->prev->next = node->next;
    if (node->next) node->next->prev = node->prev;
    if (node == cache->tail) cache->tail = node->prev;
    
    // Move to front
    node->prev = NULL;
    node->next = cache->head;
    if (cache->head) cache->head->prev = node;
    cache->head = node;
    if (cache->tail == NULL) cache->tail = node;
}

// Add to cache or update existing
static void cache_put(LRUCache* cache, const char* filename, FileMetadata* file) {
    pthread_mutex_lock(&cache->cache_mutex);
    
    // Check if already in cache
    for (int i = 0; i < cache->count; i++) {
        if (cache->nodes[i] && strcmp(cache->nodes[i]->key, filename) == 0) {
            cache->nodes[i]->file = file;
            cache->nodes[i]->access_time = time(NULL);
            cache_move_to_front(cache, cache->nodes[i]);
            pthread_mutex_unlock(&cache->cache_mutex);
            return;
        }
    }
    
    // Not in cache - need to add
    CacheNode* new_node = (CacheNode*)malloc(sizeof(CacheNode));
    if (new_node == NULL) {
        pthread_mutex_unlock(&cache->cache_mutex);
        return;
    }
    
    strcpy(new_node->key, filename);
    new_node->file = file;
    new_node->access_time = time(NULL);
    new_node->prev = NULL;
    new_node->next = cache->head;
    
    if (cache->head) cache->head->prev = new_node;
    cache->head = new_node;
    if (cache->tail == NULL) cache->tail = new_node;
    
    // If cache is full, remove LRU item
    if (cache->count >= CACHE_SIZE) {
        // Remove tail (least recently used)
        CacheNode* lru = cache->tail;
        if (lru->prev) lru->prev->next = NULL;
        cache->tail = lru->prev;
        
        // Remove from nodes array
        for (int i = 0; i < CACHE_SIZE; i++) {
            if (cache->nodes[i] == lru) {
                cache->nodes[i] = new_node;
                break;
            }
        }
        free(lru);
    } else {
        // Add to first empty slot
        cache->nodes[cache->count] = new_node;
        cache->count++;
    }
    
    pthread_mutex_unlock(&cache->cache_mutex);
}

// Get from cache
static FileMetadata* cache_get(LRUCache* cache, const char* filename) {
    pthread_mutex_lock(&cache->cache_mutex);
    
    for (int i = 0; i < cache->count; i++) {
        if (cache->nodes[i] && strcmp(cache->nodes[i]->key, filename) == 0) {
            cache->nodes[i]->access_time = time(NULL);
            FileMetadata* result = cache->nodes[i]->file;
            cache_move_to_front(cache, cache->nodes[i]);
            pthread_mutex_unlock(&cache->cache_mutex);
            return result;
        }
    }
    
    pthread_mutex_unlock(&cache->cache_mutex);
    return NULL;
}

// Get file with caching - O(1) best case
FileMetadata* filemap_get_cached(FileHashMap* map, LRUCache* cache, const char* filename) {
    // First check cache
    FileMetadata* cached = cache_get(cache, filename);
    if (cached != NULL) {
        return cached; // Cache hit!
    }
    
    // Cache miss - get from hashmap
    FileMetadata* file = filemap_get(map, filename);
    if (file != NULL) {
        // Add to cache for future lookups
        cache_put(cache, filename, file);
    }
    
    return file;
}

// Remove file from hashmap - O(1) average
bool filemap_remove(FileHashMap* map, const char* filename) {
    unsigned long hash = hash_string(filename);
    
    pthread_mutex_lock(&map->locks[hash]);
    
    FileNode* current = map->buckets[hash];
    FileNode* prev = NULL;
    
    while (current != NULL) {
        if (strcmp(current->key, filename) == 0) {
            // Found it - remove
            if (prev == NULL) {
                map->buckets[hash] = current->next;
            } else {
                prev->next = current->next;
            }
            free(current);
            pthread_mutex_unlock(&map->locks[hash]);
            
            // Update count
            pthread_mutex_lock(&map->count_mutex);
            map->count--;
            pthread_mutex_unlock(&map->count_mutex);
            
            return true;
        }
        prev = current;
        current = current->next;
    }
    
    pthread_mutex_unlock(&map->locks[hash]);
    return false; // Not found
}

// Get all files (for VIEW command) - O(N) but necessary
int filemap_get_all(FileHashMap* map, FileMetadata** files, int max_files) {
    int count = 0;
    
    for (int i = 0; i < HASHMAP_SIZE && count < max_files; i++) {
        pthread_mutex_lock(&map->locks[i]);
        
        FileNode* current = map->buckets[i];
        while (current != NULL && count < max_files) {
            files[count++] = current->file;
            current = current->next;
        }
        
        pthread_mutex_unlock(&map->locks[i]);
    }
    
    return count;
}

// Cleanup
void filemap_destroy(FileHashMap* map) {
    for (int i = 0; i < HASHMAP_SIZE; i++) {
        pthread_mutex_lock(&map->locks[i]);
        
        FileNode* current = map->buckets[i];
        while (current != NULL) {
            FileNode* next = current->next;
            free(current);
            current = next;
        }
        
        pthread_mutex_unlock(&map->locks[i]);
        pthread_mutex_destroy(&map->locks[i]);
    }
    pthread_mutex_destroy(&map->count_mutex);
}

void cache_destroy(LRUCache* cache) {
    pthread_mutex_lock(&cache->cache_mutex);
    
    CacheNode* current = cache->head;
    while (current != NULL) {
        CacheNode* next = current->next;
        free(current);
        current = next;
    }
    
    pthread_mutex_unlock(&cache->cache_mutex);
    pthread_mutex_destroy(&cache->cache_mutex);
}

// Statistics
void filemap_stats(FileHashMap* map) {
    printf("\n===== Hashmap Statistics =====\n");
    printf("Total files: %d\n", map->count);
    
    int used_buckets = 0;
    int max_chain = 0;
    int total_chain = 0;
    
    for (int i = 0; i < HASHMAP_SIZE; i++) {
        pthread_mutex_lock(&map->locks[i]);
        
        if (map->buckets[i] != NULL) {
            used_buckets++;
            int chain_len = 0;
            FileNode* current = map->buckets[i];
            while (current != NULL) {
                chain_len++;
                current = current->next;
            }
            if (chain_len > max_chain) max_chain = chain_len;
            total_chain += chain_len;
        }
        
        pthread_mutex_unlock(&map->locks[i]);
    }
    
    float load_factor = (float)map->count / HASHMAP_SIZE;
    float avg_chain = used_buckets > 0 ? (float)total_chain / used_buckets : 0;
    
    printf("Used buckets: %d / %d\n", used_buckets, HASHMAP_SIZE);
    printf("Load factor: %.4f\n", load_factor);
    printf("Average chain length: %.2f\n", avg_chain);
    printf("Max chain length: %d\n", max_chain);
    printf("==============================\n\n");
}
