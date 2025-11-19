#ifndef FILEMAP_H
#define FILEMAP_H

#include <stdbool.h>
#include <time.h>
#include <pthread.h>

// Forward declarations
#define MAX_ACL_USERS 20

typedef struct {
    char username[100];
    bool can_write;
} AclEntry;

typedef struct {
    char name[256];
    char owner[100];
    int ss_index;
    char path_on_ss[512];
    int char_count;
    int word_count;
    time_t last_access;
    AclEntry acl[MAX_ACL_USERS];
    int acl_count;
} FileMetadata;

// Hashmap node
typedef struct FileNode {
    char key[256];              // Filename (key)
    FileMetadata* file;         // Pointer to actual file metadata
    struct FileNode* next;      // Chaining for collisions
} FileNode;

// Hashmap structure
#define HASHMAP_SIZE 10007  // Prime number for better distribution
typedef struct {
    FileNode* buckets[HASHMAP_SIZE];
    pthread_mutex_t locks[HASHMAP_SIZE];  // Fine-grained locking
    int count;                            // Total number of files
    pthread_mutex_t count_mutex;
} FileHashMap;

// LRU Cache for recent lookups
#define CACHE_SIZE 100
typedef struct CacheNode {
    char key[256];
    FileMetadata* file;
    struct CacheNode* prev;
    struct CacheNode* next;
    time_t access_time;
} CacheNode;

typedef struct {
    CacheNode* head;
    CacheNode* tail;
    CacheNode* nodes[CACHE_SIZE];
    int count;
    pthread_mutex_t cache_mutex;
} LRUCache;

// Initialize hashmap
void filemap_init(FileHashMap* map);

// Initialize cache
void cache_init(LRUCache* cache);

// Insert or update file in hashmap - O(1) average
bool filemap_put(FileHashMap* map, const char* filename, FileMetadata* file);

// Get file from hashmap - O(1) average
FileMetadata* filemap_get(FileHashMap* map, const char* filename);

// Get file with caching - O(1) best case
FileMetadata* filemap_get_cached(FileHashMap* map, LRUCache* cache, const char* filename);

// Remove file from hashmap - O(1) average
bool filemap_remove(FileHashMap* map, const char* filename);

// Get all files (for VIEW command) - O(N) but necessary
int filemap_get_all(FileHashMap* map, FileMetadata** files, int max_files);

// Cleanup
void filemap_destroy(FileHashMap* map);
void cache_destroy(LRUCache* cache);

// Statistics
void filemap_stats(FileHashMap* map);

#endif // FILEMAP_H
