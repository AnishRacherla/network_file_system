#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>   // For WEXITSTATUS
#include <netinet/in.h>
#include <arpa/inet.h>  // For inet_pton
#include <pthread.h>
#include <time.h>       // For time()
#include <stdbool.h>    // For bool type
#include <limits.h>     // For INT_MAX
#include "logger.h"     // For logging system

#define PORT 8080
#define BUFFER_SIZE 1024
#define REPLY_BUFFER_SIZE 32768 // Increased for -l with many files (32KB)
#define MAX_LINE_LENGTH 512     // For formatting -l replies

// --- HashMap for O(1) file lookup ---
#define HASHMAP_SIZE 10007  // Prime number for better distribution

typedef struct HashNode {
    char key[256];
    int file_index;  // Index in file_table
    struct HashNode* next;
} HashNode;

typedef struct {
    HashNode* buckets[HASHMAP_SIZE];
    pthread_mutex_t locks[HASHMAP_SIZE];
} FileHashMap;

// LRU Cache for recent lookups
#define CACHE_SIZE 100
typedef struct CacheNode {
    char key[256];
    int file_index;
    struct CacheNode* prev;
    struct CacheNode* next;
    time_t access_time;
} CacheNode;

typedef struct {
    CacheNode* head;
    CacheNode* tail;
    CacheNode nodes[CACHE_SIZE];
    int count;
    pthread_mutex_t cache_mutex;
} LRUCache;

// Global hashmap and cache
FileHashMap file_hashmap;
LRUCache file_cache;

// --- Globals for Storage Servers ---
typedef enum {
    SS_ONLINE,
    SS_FAILED
} SSStatus;

typedef struct {
    char ip[100];
    int port;
    SSStatus status;
    time_t last_heartbeat;
} StorageServer;

#define MAX_SERVERS 10
StorageServer server_list[MAX_SERVERS];
int server_count = 0;
static int next_ss_index = 0; // For round-robin
pthread_mutex_t server_list_mutex = PTHREAD_MUTEX_INITIALIZER;

// --- Globals for File Metadata (UPDATED) ---
#define MAX_ACL_USERS 20
#define MAX_REPLICAS 1  // 1 replica = 2 total copies (primary + 1 backup)

typedef struct {
    char username[100];
    bool can_write; // true = R/W, false = R-only
} AclEntry;

typedef struct {
    char name[256];
    char owner[100];
    int ss_index; // Index in our server_list (PRIMARY)
    char path_on_ss[512]; // e.g., "data/uuid-123"
    
    // REPLICATION
    int replica_ss_indices[MAX_REPLICAS]; // Backup SS indices
    int replica_count;
    time_t last_modified;
    
    // fields for -l and INFO
    int char_count;
    int word_count;
    time_t creation_time;
    time_t last_access;
    
    // ACCESS CONTROL LIST
    AclEntry acl[MAX_ACL_USERS];
    int acl_count;

} FileMetadata;

#define MAX_FILES 1000
FileMetadata file_table[MAX_FILES];
int file_count = 0;
pthread_mutex_t file_table_mutex = PTHREAD_MUTEX_INITIALIZER;

// --- Globals for User List ---
#define MAX_USERS 1000
char global_user_list[MAX_USERS][100];
int global_user_count = 0;
pthread_mutex_t user_list_mutex = PTHREAD_MUTEX_INITIALIZER;

// --- Active Users Tracking (currently logged in) ---
char active_users[MAX_USERS][100];
int active_user_count = 0;
pthread_mutex_t active_users_mutex = PTHREAD_MUTEX_INITIALIZER;

// --- Active Client Sockets (for push notifications) ---
typedef struct {
    char username[100];
    int socket;
} ActiveClient;

ActiveClient active_clients[MAX_USERS];
int active_client_count = 0;
pthread_mutex_t active_clients_mutex = PTHREAD_MUTEX_INITIALIZER;

// --- Globals for Access Requests ---
typedef struct {
    char filename[256];
    char requester[100];
    bool wants_write; // true = requesting R/W, false = requesting R
    time_t request_time;
} AccessRequest;

#define MAX_ACCESS_REQUESTS 500
AccessRequest access_requests[MAX_ACCESS_REQUESTS];
int access_request_count = 0;
pthread_mutex_t access_request_mutex = PTHREAD_MUTEX_INITIALIZER;

// --- Notification System ---
typedef enum {
    NOTIF_ACCESS_REQUEST,   // Someone requested access to your file
    NOTIF_ACCESS_GRANTED,   // Your access request was granted
    NOTIF_FILE_MODIFIED     // A file you have access to was modified
} NotificationType;

typedef struct {
    NotificationType type;
    char recipient[100];     // Who should receive this notification
    char sender[100];        // Who triggered this notification
    char filename[256];      // Related filename
    char message[512];       // Full notification message
    time_t timestamp;
    bool is_read;           // Whether user has seen this notification
} Notification;

#define MAX_NOTIFICATIONS 1000
Notification notifications[MAX_NOTIFICATIONS];
int notification_count = 0;
pthread_mutex_t notification_mutex = PTHREAD_MUTEX_INITIALIZER;

// Function declarations for notification system
void add_notification(NotificationType type, const char* recipient, const char* sender, 
                     const char* filename, const char* message);
void send_pending_notifications(int client_socket, const char* username);
int get_unread_notification_count(const char* username);
void push_notification_to_user(const char* username, const char* message);

// --- FULL-TEXT SEARCH / INVERTED INDEX ---
typedef struct WordOccurrence {
    char filename[256];
    int sentence_number;
    struct WordOccurrence* next;
} WordOccurrence;

typedef struct IndexNode {
    char word[100];
    WordOccurrence* occurrences;
    struct IndexNode* left;
    struct IndexNode* right;
} IndexNode;

IndexNode* search_index_root = NULL;
pthread_mutex_t search_index_mutex = PTHREAD_MUTEX_INITIALIZER;

// Function declarations for search index
void index_file_content(const char* filename, const char* content);
void search_word_in_index(const char* word, char* result_buffer, int buffer_size);
void free_search_index(IndexNode* node);

#define METADATA_FILE "nameserver_metadata.dat"

// === HASHMAP IMPLEMENTATION FOR O(1) SEARCH ===

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
void hashmap_init(FileHashMap* map) {
    for (int i = 0; i < HASHMAP_SIZE; i++) {
        map->buckets[i] = NULL;
        pthread_mutex_init(&map->locks[i], NULL);
    }
}

// Initialize LRU cache
void cache_init(LRUCache* cache) {
    cache->head = NULL;
    cache->tail = NULL;
    cache->count = 0;
    pthread_mutex_init(&cache->cache_mutex, NULL);
}

// Add/Update in hashmap - O(1) average
void hashmap_put(FileHashMap* map, const char* filename, int file_index) {
    unsigned long hash = hash_string(filename);
    
    pthread_mutex_lock(&map->locks[hash]);
    
    // Check if already exists
    HashNode* current = map->buckets[hash];
    while (current != NULL) {
        if (strcmp(current->key, filename) == 0) {
            current->file_index = file_index; // Update
            pthread_mutex_unlock(&map->locks[hash]);
            return;
        }
        current = current->next;
    }
    
    // Insert new
    HashNode* new_node = (HashNode*)malloc(sizeof(HashNode));
    strcpy(new_node->key, filename);
    new_node->file_index = file_index;
    new_node->next = map->buckets[hash];
    map->buckets[hash] = new_node;
    
    pthread_mutex_unlock(&map->locks[hash]);
}

// Get from hashmap - O(1) average
int hashmap_get(FileHashMap* map, const char* filename) {
    unsigned long hash = hash_string(filename);
    
    pthread_mutex_lock(&map->locks[hash]);
    
    HashNode* current = map->buckets[hash];
    while (current != NULL) {
        if (strcmp(current->key, filename) == 0) {
            int index = current->file_index;
            pthread_mutex_unlock(&map->locks[hash]);
            return index;
        }
        current = current->next;
    }
    
    pthread_mutex_unlock(&map->locks[hash]);
    return -1; // Not found
}

// Remove from hashmap - O(1) average
void hashmap_remove(FileHashMap* map, const char* filename) {
    unsigned long hash = hash_string(filename);
    
    pthread_mutex_lock(&map->locks[hash]);
    
    HashNode* current = map->buckets[hash];
    HashNode* prev = NULL;
    
    while (current != NULL) {
        if (strcmp(current->key, filename) == 0) {
            if (prev == NULL) {
                map->buckets[hash] = current->next;
            } else {
                prev->next = current->next;
            }
            free(current);
            pthread_mutex_unlock(&map->locks[hash]);
            return;
        }
        prev = current;
        current = current->next;
    }
    
    pthread_mutex_unlock(&map->locks[hash]);
}

// Move node to front of LRU cache
static void cache_move_to_front(LRUCache* cache, CacheNode* node) {
    if (node == cache->head) return;
    
    if (node->prev) node->prev->next = node->next;
    if (node->next) node->next->prev = node->prev;
    if (node == cache->tail) cache->tail = node->prev;
    
    node->prev = NULL;
    node->next = cache->head;
    if (cache->head) cache->head->prev = node;
    cache->head = node;
    if (cache->tail == NULL) cache->tail = node;
}

// Add to cache
void cache_put(LRUCache* cache, const char* filename, int file_index) {
    pthread_mutex_lock(&cache->cache_mutex);
    
    // Check if already in cache
    for (int i = 0; i < cache->count; i++) {
        if (strcmp(cache->nodes[i].key, filename) == 0) {
            cache->nodes[i].file_index = file_index;
            cache->nodes[i].access_time = time(NULL);
            cache_move_to_front(cache, &cache->nodes[i]);
            pthread_mutex_unlock(&cache->cache_mutex);
            return;
        }
    }
    
    // Add new (evict LRU if full)
    int slot = cache->count < CACHE_SIZE ? cache->count++ : CACHE_SIZE - 1;
    
    if (slot == CACHE_SIZE - 1 && cache->tail) {
        // Remove tail from linked list
        CacheNode* lru = cache->tail;
        if (lru->prev) lru->prev->next = NULL;
        cache->tail = lru->prev;
    }
    
    strcpy(cache->nodes[slot].key, filename);
    cache->nodes[slot].file_index = file_index;
    cache->nodes[slot].access_time = time(NULL);
    cache->nodes[slot].prev = NULL;
    cache->nodes[slot].next = cache->head;
    
    if (cache->head) cache->head->prev = &cache->nodes[slot];
    cache->head = &cache->nodes[slot];
    if (cache->tail == NULL) cache->tail = &cache->nodes[slot];
    
    pthread_mutex_unlock(&cache->cache_mutex);
}

// Get from cache - O(1)
int cache_get(LRUCache* cache, const char* filename) {
    pthread_mutex_lock(&cache->cache_mutex);
    
    for (int i = 0; i < cache->count; i++) {
        if (strcmp(cache->nodes[i].key, filename) == 0) {
            cache->nodes[i].access_time = time(NULL);
            cache_move_to_front(cache, &cache->nodes[i]);
            int index = cache->nodes[i].file_index;
            pthread_mutex_unlock(&cache->cache_mutex);
            return index;
        }
    }
    
    pthread_mutex_unlock(&cache->cache_mutex);
    return -1;
}

// Remove from cache
void cache_remove(LRUCache* cache, const char* filename) {
    pthread_mutex_lock(&cache->cache_mutex);
    
    for (int i = 0; i < cache->count; i++) {
        if (strcmp(cache->nodes[i].key, filename) == 0) {
            // Remove this node from linked list
            if (cache->nodes[i].prev) cache->nodes[i].prev->next = cache->nodes[i].next;
            if (cache->nodes[i].next) cache->nodes[i].next->prev = cache->nodes[i].prev;
            if (&cache->nodes[i] == cache->head) cache->head = cache->nodes[i].next;
            if (&cache->nodes[i] == cache->tail) cache->tail = cache->nodes[i].prev;
            
            // Swap with last element and decrease count
            cache->nodes[i] = cache->nodes[cache->count - 1];
            cache->count--;
            break;
        }
    }
    
    pthread_mutex_unlock(&cache->cache_mutex);
}

// Get file index with caching - O(1) best case
int get_file_index(const char* filename) {
    // Try cache first
    int index = cache_get(&file_cache, filename);
    if (index != -1) {
        return index; // Cache hit!
    }
    
    // Cache miss - try hashmap
    index = hashmap_get(&file_hashmap, filename);
    if (index != -1) {
        cache_put(&file_cache, filename, index); // Add to cache
    }
    
    return index;
}

// === END OF HASHMAP IMPLEMENTATION ===

// === NOTIFICATION SYSTEM IMPLEMENTATION ===

// Add a notification to the system
void add_notification(NotificationType type, const char* recipient, const char* sender, 
                     const char* filename, const char* message) {
    pthread_mutex_lock(&notification_mutex);
    
    if (notification_count >= MAX_NOTIFICATIONS) {
        // Remove oldest notifications to make space
        for (int i = 0; i < MAX_NOTIFICATIONS - 1; i++) {
            notifications[i] = notifications[i + 1];
        }
        notification_count = MAX_NOTIFICATIONS - 1;
    }
    
    Notification* notif = &notifications[notification_count];
    notif->type = type;
    strncpy(notif->recipient, recipient, sizeof(notif->recipient) - 1);
    notif->recipient[sizeof(notif->recipient) - 1] = '\0';
    strncpy(notif->sender, sender, sizeof(notif->sender) - 1);
    notif->sender[sizeof(notif->sender) - 1] = '\0';
    strncpy(notif->filename, filename, sizeof(notif->filename) - 1);
    notif->filename[sizeof(notif->filename) - 1] = '\0';
    strncpy(notif->message, message, sizeof(notif->message) - 1);
    notif->message[sizeof(notif->message) - 1] = '\0';
    notif->timestamp = time(NULL);
    notif->is_read = false;
    
    notification_count++;
    
    printf("[NOTIFICATION] Added for %s: %s\n", recipient, message);
    
    pthread_mutex_unlock(&notification_mutex);
    
    // Push notification to user if they're online
    push_notification_to_user(recipient, message);
}

// Get count of unread notifications for a user
int get_unread_notification_count(const char* username) {
    int count = 0;
    pthread_mutex_lock(&notification_mutex);
    
    for (int i = 0; i < notification_count; i++) {
        if (strcmp(notifications[i].recipient, username) == 0 && !notifications[i].is_read) {
            count++;
        }
    }
    
    pthread_mutex_unlock(&notification_mutex);
    return count;
}

// Send pending notifications to user (called on login or when requested)
void send_pending_notifications(int client_socket, const char* username) {
    pthread_mutex_lock(&notification_mutex);
    
    int unread_count = 0;
    for (int i = 0; i < notification_count; i++) {
        if (strcmp(notifications[i].recipient, username) == 0 && !notifications[i].is_read) {
            unread_count++;
        }
    }
    
    if (unread_count == 0) {
        pthread_mutex_unlock(&notification_mutex);
        return;
    }
    
    // Send notification header
    char header[256];
    snprintf(header, sizeof(header), 
             "\n========== You have %d new notification%s ==========\n",
             unread_count, unread_count == 1 ? "" : "s");
    send(client_socket, header, strlen(header), 0);
    
    // Send each notification
    for (int i = 0; i < notification_count; i++) {
        if (strcmp(notifications[i].recipient, username) == 0 && !notifications[i].is_read) {
            char notif_msg[1024];
            char time_str[100];
            struct tm* tm_info = localtime(&notifications[i].timestamp);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
            
            const char* type_str = "";
            switch (notifications[i].type) {
                case NOTIF_ACCESS_REQUEST:
                    type_str = "[ACCESS REQUEST]";
                    break;
                case NOTIF_ACCESS_GRANTED:
                    type_str = "[ACCESS GRANTED]";
                    break;
                case NOTIF_FILE_MODIFIED:
                    type_str = "[FILE MODIFIED]";
                    break;
            }
            
            snprintf(notif_msg, sizeof(notif_msg),
                    "%s %s\n  From: %s | File: %s\n  %s\n\n",
                    type_str, time_str, notifications[i].sender, 
                    notifications[i].filename, notifications[i].message);
            
            send(client_socket, notif_msg, strlen(notif_msg), 0);
            
            // Mark as read
            notifications[i].is_read = true;
        }
    }
    
    char footer[] = "=========================================================\n";
    send(client_socket, footer, strlen(footer), 0);
    
    pthread_mutex_unlock(&notification_mutex);
}

// Push notification to a specific user if they're online
void push_notification_to_user(const char* username, const char* message) {
    pthread_mutex_lock(&active_clients_mutex);
    
    // Find the user's socket
    int target_socket = -1;
    for (int i = 0; i < active_client_count; i++) {
        if (strcmp(active_clients[i].username, username) == 0) {
            target_socket = active_clients[i].socket;
            break;
        }
    }
    
    pthread_mutex_unlock(&active_clients_mutex);
    
    // If user is online, send the notification
    if (target_socket != -1) {
        char notif_buffer[4096];
        snprintf(notif_buffer, sizeof(notif_buffer), "[NOTIFICATION] %s\n", message);
        send(target_socket, notif_buffer, strlen(notif_buffer), 0);
    }
}

// === END OF NOTIFICATION SYSTEM ===

// === FULL-TEXT SEARCH / INVERTED INDEX IMPLEMENTATION ===

// Helper: Convert string to lowercase
void to_lowercase(char* str) {
    for (int i = 0; str[i]; i++) {
        if (str[i] >= 'A' && str[i] <= 'Z') {
            str[i] = str[i] + 32;
        }
    }
}

// Helper: Check if character is alphanumeric
int is_alphanumeric(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// Insert a word occurrence into the BST
IndexNode* insert_word_occurrence(IndexNode* root, const char* word, const char* filename, int sentence_num) {
    if (root == NULL) {
        // Create new node for this word
        IndexNode* new_node = (IndexNode*)malloc(sizeof(IndexNode));
        strncpy(new_node->word, word, sizeof(new_node->word) - 1);
        new_node->word[sizeof(new_node->word) - 1] = '\0';
        new_node->left = NULL;
        new_node->right = NULL;
        
        // Create first occurrence
        WordOccurrence* occ = (WordOccurrence*)malloc(sizeof(WordOccurrence));
        strncpy(occ->filename, filename, sizeof(occ->filename) - 1);
        occ->filename[sizeof(occ->filename) - 1] = '\0';
        occ->sentence_number = sentence_num;
        occ->next = NULL;
        
        new_node->occurrences = occ;
        return new_node;
    }
    
    int cmp = strcmp(word, root->word);
    
    if (cmp < 0) {
        root->left = insert_word_occurrence(root->left, word, filename, sentence_num);
    } else if (cmp > 0) {
        root->right = insert_word_occurrence(root->right, word, filename, sentence_num);
    } else {
        // Word already exists, add new occurrence to the list
        WordOccurrence* occ = (WordOccurrence*)malloc(sizeof(WordOccurrence));
        strncpy(occ->filename, filename, sizeof(occ->filename) - 1);
        occ->filename[sizeof(occ->filename) - 1] = '\0';
        occ->sentence_number = sentence_num;
        occ->next = root->occurrences;
        root->occurrences = occ;
    }
    
    return root;
}

// Index file content: parse into sentences and words
void index_file_content(const char* filename, const char* content) {
    pthread_mutex_lock(&search_index_mutex);
    
    char* content_copy = strdup(content);
    if (!content_copy) {
        pthread_mutex_unlock(&search_index_mutex);
        return;
    }
    
    int sentence_num = 1;
    char word[100];
    int word_pos = 0;
    
    for (int i = 0; content_copy[i] != '\0'; i++) {
        char c = content_copy[i];
        
        // Check for sentence delimiters
        if (c == '.' || c == '!' || c == '?') {
            // Finish current word if any
            if (word_pos > 0) {
                word[word_pos] = '\0';
                to_lowercase(word);
                search_index_root = insert_word_occurrence(search_index_root, word, filename, sentence_num);
                word_pos = 0;
            }
            sentence_num++;
        } else if (is_alphanumeric(c)) {
            // Build word
            if (word_pos < sizeof(word) - 1) {
                word[word_pos++] = c;
            }
        } else {
            // Space or other delimiter - finish current word
            if (word_pos > 0) {
                word[word_pos] = '\0';
                to_lowercase(word);
                search_index_root = insert_word_occurrence(search_index_root, word, filename, sentence_num);
                word_pos = 0;
            }
        }
    }
    
    // Handle last word if file doesn't end with punctuation
    if (word_pos > 0) {
        word[word_pos] = '\0';
        to_lowercase(word);
        search_index_root = insert_word_occurrence(search_index_root, word, filename, sentence_num);
    }
    
    free(content_copy);
    pthread_mutex_unlock(&search_index_mutex);
}

// Search for a word in the index
IndexNode* find_word_in_tree(IndexNode* root, const char* word) {
    if (root == NULL) {
        return NULL;
    }
    
    int cmp = strcmp(word, root->word);
    
    if (cmp < 0) {
        return find_word_in_tree(root->left, word);
    } else if (cmp > 0) {
        return find_word_in_tree(root->right, word);
    } else {
        return root;
    }
}

// Search for a word and format results
void search_word_in_index(const char* word, char* result_buffer, int buffer_size) {
    pthread_mutex_lock(&search_index_mutex);
    
    char search_word[100];
    strncpy(search_word, word, sizeof(search_word) - 1);
    search_word[sizeof(search_word) - 1] = '\0';
    to_lowercase(search_word);
    
    IndexNode* node = find_word_in_tree(search_index_root, search_word);
    
    if (node == NULL) {
        snprintf(result_buffer, buffer_size, "404 Word '%s' not found in any indexed files\n", word);
    } else {
        int offset = snprintf(result_buffer, buffer_size, "200 SEARCH_RESULTS\n");
        
        WordOccurrence* occ = node->occurrences;
        while (occ != NULL && offset < buffer_size - 100) {
            offset += snprintf(result_buffer + offset, buffer_size - offset, 
                             "%s:%d\n", occ->filename, occ->sentence_number);
            occ = occ->next;
        }
    }
    
    pthread_mutex_unlock(&search_index_mutex);
}

// Free the search index tree
void free_search_index(IndexNode* node) {
    if (node == NULL) return;
    
    free_search_index(node->left);
    free_search_index(node->right);
    
    // Free all occurrences
    WordOccurrence* occ = node->occurrences;
    while (occ != NULL) {
        WordOccurrence* next = occ->next;
        free(occ);
        occ = next;
    }
    
    free(node);
}

// === END OF SEARCH INDEX IMPLEMENTATION ===



void* async_replicate_file(void* arg);
void pick_backup_servers(int primary_index, int* backup_indices, int* backup_count);
int test_ss_connection(const char* ip, int port);
void sync_files_to_recovered_ss(int recovered_ss_index);


// --- Helper: Save metadata to disk ---
void save_metadata() {
    pthread_mutex_lock(&file_table_mutex);
    pthread_mutex_lock(&user_list_mutex);
    
    FILE* f = fopen(METADATA_FILE, "wb");
    if (f) {
        // Save file data
        fwrite(&file_count, sizeof(int), 1, f);
        fwrite(file_table, sizeof(FileMetadata), file_count, f);
        
        // Save user data
        fwrite(&global_user_count, sizeof(int), 1, f);
        fwrite(global_user_list, sizeof(char[100]), global_user_count, f);
        
        fclose(f);
        printf("[METADATA] Saved %d files and %d users to disk\n", file_count, global_user_count);
    } else {
        perror("Failed to save metadata");
    }
    
    pthread_mutex_unlock(&user_list_mutex);
    pthread_mutex_unlock(&file_table_mutex);
}

// --- Helper: Load metadata from disk ---
void load_metadata() {
    FILE* f = fopen(METADATA_FILE, "rb");
    if (f) {
        int loaded_file_count = 0;
        
        // Read file data
        fread(&loaded_file_count, sizeof(int), 1, f);
        
        // Validate and load files one by one
        file_count = 0;
        for (int i = 0; i < loaded_file_count && i < MAX_FILES; i++) {
            FileMetadata temp;
            fread(&temp, sizeof(FileMetadata), 1, f);
            
            // Validate the file metadata
            if (temp.name[0] == '\0' || temp.owner[0] == '\0') {
                printf("[METADATA] Skipping corrupted file entry %d (empty name or owner)\n", i);
                continue;
            }
            
            // Check for duplicates before adding
            bool is_duplicate = false;
            for (int j = 0; j < file_count; j++) {
                if (strcmp(file_table[j].name, temp.name) == 0) {
                    printf("[METADATA] Skipping duplicate file: %s (keeping first owner: %s)\n", 
                           temp.name, file_table[j].owner);
                    is_duplicate = true;
                    break;
                }
            }
            
            if (!is_duplicate) {
                file_table[file_count] = temp;
                file_count++;
            }
        }
        
        // Read user data
        fread(&global_user_count, sizeof(int), 1, f);
        if (global_user_count > MAX_USERS) global_user_count = MAX_USERS;
        fread(global_user_list, sizeof(char[100]), global_user_count, f);
        
        fclose(f);
        printf("[METADATA] Loaded %d files (skipped %d corrupted/duplicates) and %d users from disk\n", 
               file_count, loaded_file_count - file_count, global_user_count);
    } else {
        printf("[METADATA] No previous metadata found (new server)\n");
        file_count = 0;
        global_user_count = 0;
    }
}

// --- Helper: Check Permissions ---
bool check_permission(FileMetadata* file, const char* user, bool needs_write) {
    // 1. Check if they are the owner
    if (strcmp(file->owner, user) == 0) {
        return true;
    }
    // 2. Check the ACL
    for (int i = 0; i < file->acl_count; i++) {
        if (strcmp(file->acl[i].username, user) == 0) {
            // User found in ACL
            if (needs_write) {
                return file->acl[i].can_write; // Must have write permission
            } else {
                return true; // Read permission is enough
            }
        }
    }
    return false; // No access
}

// --- Helper: Find least loaded storage server (minimum number of files) ---
int find_least_loaded_ss() {
    // Must be called with server_list_mutex already locked
    int min_files = INT_MAX;
    int candidates[MAX_SERVERS];  // Servers with minimum files
    int candidate_count = 0;
    
    printf("[LOAD BALANCING] Finding least loaded storage server:\n");
    
    // First pass: find the minimum file count
    for (int i = 0; i < server_count; i++) {
        if (server_list[i].status == SS_ONLINE) {
            // Count files on this storage server
            int file_count_on_ss = 0;
            pthread_mutex_lock(&file_table_mutex);
            for (int j = 0; j < file_count; j++) {
                if (file_table[j].ss_index == i) {
                    file_count_on_ss++;
                }
            }
            pthread_mutex_unlock(&file_table_mutex);
            
            printf("[LOAD BALANCING] SS %d (port %d): %d files\n", 
                   i, server_list[i].port, file_count_on_ss);
            
            if (file_count_on_ss < min_files) {
                min_files = file_count_on_ss;
            }
        } else {
            printf("[LOAD BALANCING] SS %d (port %d): OFFLINE/FAILED\n", 
                   i, server_list[i].port);
        }
    }
    
    // Second pass: collect all servers with minimum file count
    for (int i = 0; i < server_count; i++) {
        if (server_list[i].status == SS_ONLINE) {
            int file_count_on_ss = 0;
            pthread_mutex_lock(&file_table_mutex);
            for (int j = 0; j < file_count; j++) {
                if (file_table[j].ss_index == i) {
                    file_count_on_ss++;
                }
            }
            pthread_mutex_unlock(&file_table_mutex);
            
            if (file_count_on_ss == min_files) {
                candidates[candidate_count++] = i;
            }
        }
    }
    
    // If multiple servers have same minimum, use round-robin among them
    int best_ss_index = -1;
    if (candidate_count > 0) {
        static int rr_counter = 0;  // Separate counter for round-robin among candidates
        best_ss_index = candidates[rr_counter % candidate_count];
        rr_counter++;
        printf("[LOAD BALANCING] %d servers with %d files, selected SS %d (port %d)\n", 
               candidate_count, min_files, best_ss_index, server_list[best_ss_index].port);
    }
    
    return best_ss_index;
}


// --- Thread Function: handle_client ---
void* handle_client(void* arg) {
    int client_socket = *(int*)arg;
    free(arg); 

    char buffer[BUFFER_SIZE];
    char username[100] = {0};  // Declare username at function scope
    
    // --- IDENTIFICATION STEP ---
    memset(buffer, 0, BUFFER_SIZE);
    int bytes_read = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
    
    if (bytes_read <= 0) {
        printf("[Socket %d] Client disconnected before identifying.\n", client_socket);
        close(client_socket);
        return NULL;
    }
    buffer[bytes_read] = '\0';

    // --- CHECK CONNECTION TYPE ---
    if (strncmp(buffer, "REGISTER", 8) == 0) {
        // --- IT'S A STORAGE SERVER ---
        printf("[Socket %d] Received REGISTER command: %s\n", client_socket, buffer);
        
        char ip[100];
        int port;
        char file_list_raw[8192] = {0};
        
        // Parse: REGISTER <ip> <port> [file1|file2|...]
        // First, extract IP and port
        char* space1 = strchr(buffer, ' ');
        if (space1 == NULL) {
            send(client_socket, "400 ERROR: BAD FORMAT", 21, 0);
            close(client_socket);
            return NULL;
        }
        space1++;
        
        char* space2 = strchr(space1, ' ');
        if (space2 == NULL) {
            send(client_socket, "400 ERROR: BAD FORMAT", 21, 0);
            close(client_socket);
            return NULL;
        }
        
        strncpy(ip, space1, space2 - space1);
        ip[space2 - space1] = '\0';
        
        // Parse port and optional file list
        char* space3 = strchr(space2 + 1, ' ');
        if (space3 == NULL) {
            // No file list, just IP and port
            port = atoi(space2 + 1);
        } else {
            // File list exists
            char port_str[10];
            strncpy(port_str, space2 + 1, space3 - space2 - 1);
            port_str[space3 - space2 - 1] = '\0';
            port = atoi(port_str);
            
            // Copy file list
            strcpy(file_list_raw, space3 + 1);
        }

        pthread_mutex_lock(&server_list_mutex);
        
        // Check if this SS is already registered (by IP:port)
        int existing_ss_index = -1;
        for (int i = 0; i < server_count; i++) {
            if (strcmp(server_list[i].ip, ip) == 0 && server_list[i].port == port) {
                existing_ss_index = i;
                break;
            }
        }
        
        int ss_index;
        bool is_recovery = false;
        if (existing_ss_index != -1) {
            // SS re-registering (after restart/failure)
            ss_index = existing_ss_index;
            // ALWAYS treat re-registration as recovery - server restarted and needs sync
            is_recovery = true;
            printf("Storage Server %s:%d is RE-REGISTERING (index %d) - triggering recovery sync\n", ip, port, ss_index);
            server_list[ss_index].status = SS_ONLINE;  // Mark as online again!
            server_list[ss_index].last_heartbeat = time(NULL);
        } else {
            // New SS
            if (server_count >= MAX_SERVERS) {
                pthread_mutex_unlock(&server_list_mutex);
                printf("Max servers reached. Rejecting.\n");
                send(client_socket, "500 ERROR: MAX SERVERS", 22, 0);
                close(client_socket);
                return NULL;
            }
            
            strcpy(server_list[server_count].ip, ip);
            server_list[server_count].port = port;
            server_list[server_count].status = SS_ONLINE;
            server_list[server_count].last_heartbeat = time(NULL);
            ss_index = server_count;
            server_count++;
            printf("Registered new Storage Server: %s:%d (index %d). Total servers: %d\n", 
                   ip, port, ss_index, server_count);
        }
        
        pthread_mutex_unlock(&server_list_mutex);
        
        // RECOVERY PHASE 1: Before processing files, determine what needs to be synced
        // We'll do the actual sync AFTER processing the file list
        bool needs_recovery_sync = is_recovery;
        
        // Process file list
        if (strlen(file_list_raw) > 0) {
            printf("[SS %d] Processing file list: %s\n", ss_index, file_list_raw);
            
            pthread_mutex_lock(&file_table_mutex);
            
            // Parse file list (format: path1|path2|path3)
            char* token = strtok(file_list_raw, "|");
            while (token != NULL) {
                // Check if file already exists in our table
                int existing_file_index = -1;
                for (int i = 0; i < file_count; i++) {
                    if (strcmp(file_table[i].path_on_ss, token) == 0) {
                        existing_file_index = i;
                        break;
                    }
                }
                
                if (existing_file_index != -1) {
                    // File already in table - update its SS index (in case it moved)
                    file_table[existing_file_index].ss_index = ss_index;
                    printf("[SS %d] File %s already registered, updated SS index\n", 
                           ss_index, file_table[existing_file_index].name);
                } else {
                    // New file - extract logical name from path
                    // Format: data/filename_timestamp or data/filename
                    char* last_slash = strrchr(token, '/');
                    char* filename = (last_slash != NULL) ? last_slash + 1 : token;
                    
                    // Remove timestamp if present (format: name_timestamp)
                    char logical_name[256];
                    strcpy(logical_name, filename);
                    char* underscore = strrchr(logical_name, '_');
                    if (underscore != NULL) {
                        // Check if what follows is a timestamp (all digits)
                        bool is_timestamp = true;
                        for (char* p = underscore + 1; *p != '\0'; p++) {
                            if (*p < '0' || *p > '9') {
                                is_timestamp = false;
                                break;
                            }
                        }
                        if (is_timestamp) {
                            *underscore = '\0'; // Remove timestamp
                        }
                    }
                    
                    if (file_count < MAX_FILES) {
                        strcpy(file_table[file_count].name, logical_name);
                        strcpy(file_table[file_count].owner, "system"); // Default owner
                        file_table[file_count].ss_index = ss_index;
                        strcpy(file_table[file_count].path_on_ss, token);
                        file_table[file_count].char_count = 0;
                        file_table[file_count].word_count = 0;
                        file_table[file_count].last_access = time(NULL);
                        file_table[file_count].acl_count = 0;
                        file_count++;
                        
                        printf("[SS %d] Registered existing file: %s -> %s\n", 
                               ss_index, logical_name, token);
                    }
                }
                
                token = strtok(NULL, "|");
            }
            
            pthread_mutex_unlock(&file_table_mutex);
            
            // Save metadata after processing files
            save_metadata();
        }
        
        // IMPORTANT: Send success response BEFORE starting sync
        // This allows the SS to be ready to receive INTERNAL_REPLICATE commands
        send(client_socket, "200 OK", 6, 0);
        printf("[Socket %d] Registration complete. Closing SS init connection.\n", client_socket);
        close(client_socket);
        
        // RECOVERY PHASE 2: If this server is recovering, FORCE sync ALL files from replicas
        // This happens AFTER registration is complete and file list is processed
        if (needs_recovery_sync) {
            printf("\n[RECOVERY] ╔════════════════════════════════════════════╗\n");
            printf("[RECOVERY] ║ FORCE SYNCING ALL FILES FOR SS %d          ║\n", ss_index);
            printf("[RECOVERY] ║ Server: %s:%d                              ║\n", ip, port);
            printf("[RECOVERY] ║ All files will be synced from replicas    ║\n");
            printf("[RECOVERY] ╚════════════════════════════════════════════╝\n\n");
            
            sleep(1); // Give SS time to start listening
            
            pthread_mutex_lock(&file_table_mutex);
            
            printf("[RECOVERY] Total files in system: %d\n", file_count);
            int synced_count = 0;
            int failed_count = 0;
            
            // Sync ALL files that this SS should have
            for (int i = 0; i < file_count; i++) {
                int source_ss_index = -1;
                bool should_have_file = false;
                
                printf("\n[RECOVERY] --- File #%d: '%s' ---\n", i, file_table[i].name);
                printf("\n[RECOVERY] --- File #%d: '%s' ---\n", i, file_table[i].name);
                
                // Case 1: This SS is the PRIMARY - sync FROM replica
                if (file_table[i].ss_index == ss_index) {
                    should_have_file = true;
                    printf("[RECOVERY] SS %d is PRIMARY for this file\n", ss_index);
                    printf("[RECOVERY] Replicas: %d total\n", file_table[i].replica_count);
                    
                    // Find an online replica to sync from
                    for (int r = 0; r < file_table[i].replica_count; r++) {
                        int replica_idx = file_table[i].replica_ss_indices[r];
                        printf("[RECOVERY]   Replica #%d: SS %d", r, replica_idx);
                        
                        pthread_mutex_lock(&server_list_mutex);
                        bool is_online = (server_list[replica_idx].status == SS_ONLINE);
                        pthread_mutex_unlock(&server_list_mutex);
                        
                        printf(" - %s\n", is_online ? "ONLINE" : "OFFLINE");
                        
                        if (is_online) {
                            source_ss_index = replica_idx;
                            printf("[RECOVERY] ✓ Will sync FROM replica SS %d TO primary SS %d\n", 
                                   replica_idx, ss_index);
                            break;
                        }
                    }
                }
                // Case 2: This SS is a REPLICA - sync FROM primary
                else {
                    for (int r = 0; r < file_table[i].replica_count; r++) {
                        if (file_table[i].replica_ss_indices[r] == ss_index) {
                            should_have_file = true;
                            printf("[RECOVERY] SS %d is REPLICA for this file\n", ss_index);
                            printf("[RECOVERY] Primary is SS %d\n", file_table[i].ss_index);
                            
                            pthread_mutex_lock(&server_list_mutex);
                            bool primary_online = (server_list[file_table[i].ss_index].status == SS_ONLINE);
                            pthread_mutex_unlock(&server_list_mutex);
                            
                            printf("[RECOVERY] Primary status: %s\n", primary_online ? "ONLINE" : "OFFLINE");
                            
                            if (primary_online) {
                                source_ss_index = file_table[i].ss_index;
                                printf("[RECOVERY] ✓ Will sync FROM primary SS %d TO replica SS %d\n", 
                                       source_ss_index, ss_index);
                            }
                            break;
                        }
                    }
                }
                
                if (!should_have_file) {
                    printf("[RECOVERY] ✗ SS %d should NOT have this file - skipping\n", ss_index);
                    continue;
                }
                
                if (source_ss_index == -1) {
                    printf("[RECOVERY] ✗ No online source found - cannot sync\n");
                    failed_count++;
                    continue;
                }
                if (source_ss_index == -1) {
                    printf("[RECOVERY] ✗ No online source found - cannot sync\n");
                    failed_count++;
                    continue;
                }
                
                // SYNC THE FILE
                printf("\n[RECOVERY SYNC] ═══════════════════════════════════════\n");
                printf("[RECOVERY SYNC] File: '%s'\n", file_table[i].name);
                printf("[RECOVERY SYNC] Direction: SS %d → SS %d\n", source_ss_index, ss_index);
                
                // Get source and target server info
                pthread_mutex_lock(&server_list_mutex);
                StorageServer source_ss = server_list[source_ss_index];
                StorageServer target_ss = server_list[ss_index];
                pthread_mutex_unlock(&server_list_mutex);
                
                printf("[RECOVERY SYNC] Source: %s:%d\n", source_ss.ip, source_ss.port);
                printf("[RECOVERY SYNC] Target: %s:%d\n", target_ss.ip, target_ss.port);
                
                // Connect to target (recovering) server
                int target_sock = socket(AF_INET, SOCK_STREAM, 0);
                struct sockaddr_in target_addr;
                memset(&target_addr, 0, sizeof(target_addr));
                target_addr.sin_family = AF_INET;
                target_addr.sin_port = htons(target_ss.port);
                inet_pton(AF_INET, target_ss.ip, &target_addr.sin_addr);
                
                if (connect(target_sock, (struct sockaddr *)&target_addr, sizeof(target_addr)) < 0) {
                    printf("[RECOVERY SYNC] ✗ FAILED - Cannot connect to target SS %d\n", ss_index);
                    printf("[RECOVERY SYNC] ═══════════════════════════════════════\n\n");
                    failed_count++;
                    continue;
                }
                
                // Construct the correct path on the SOURCE server
                // Extract just the filename from the path
                char* filename_only = strrchr(file_table[i].path_on_ss, '/');
                if (filename_only) {
                    filename_only++; // Skip the '/'
                } else {
                    filename_only = (char*)file_table[i].path_on_ss;
                }
                
                // Build the path on the source server
                char source_path[512];
                snprintf(source_path, sizeof(source_path), "data_ss_%d/%s", 
                         9000 + source_ss_index + 1, filename_only);
                
                printf("[RECOVERY SYNC] Source path: %s (on SS %d)\n", source_path, source_ss_index);
                printf("[RECOVERY SYNC] Target path: %s (on SS %d)\n", file_table[i].path_on_ss, ss_index);
                
                // Send INTERNAL_REPLICATE command to pull from source
                char command[1024];
                sprintf(command, "INTERNAL_REPLICATE %s %s:%d", 
                        source_path, source_ss.ip, source_ss.port);
                printf("[RECOVERY SYNC] Command: %s\n", command);
                send(target_sock, command, strlen(command), 0);
                
                // Wait for response (synchronous)
                char reply[256] = {0};
                int bytes_received = recv(target_sock, reply, sizeof(reply) - 1, 0);
                close(target_sock);
                
                if (bytes_received > 0 && strncmp(reply, "200 OK", 6) == 0) {
                    printf("[RECOVERY SYNC] ✓ SUCCESS\n");
                    printf("[RECOVERY SYNC] Response: %s\n", reply);
                    synced_count++;
                } else {
                    printf("[RECOVERY SYNC] ✗ FAILED\n");
                    printf("[RECOVERY SYNC] Response: %s\n", bytes_received > 0 ? reply : "(no response)");
                    failed_count++;
                }
                printf("[RECOVERY SYNC] ═══════════════════════════════════════\n\n");
            }
            
            pthread_mutex_unlock(&file_table_mutex);
            
            printf("\n[RECOVERY] ╔════════════════════════════════════════════╗\n");
            printf("[RECOVERY] ║  SYNC COMPLETE FOR SS %d                   ║\n", ss_index);
            printf("[RECOVERY] ║  Synced: %d  Failed: %d                    ║\n", synced_count, failed_count);
            printf("[RECOVERY] ║  Server is now ready for operations       ║\n");
            printf("[RECOVERY] ╚════════════════════════════════════════════╝\n\n");
        }
        
        return NULL;
    
    } else if (strncmp(buffer, "UPDATE_META", 11) == 0) {
        // --- STORAGE SERVER IS REPORTING AN UPDATE ---
        printf("[Socket %d] Received UPDATE_META: %s\n", client_socket, buffer);
        char command[100], path_on_ss[512];
        int wc, cc;
        sscanf(buffer, "%s %s %d %d", command, path_on_ss, &wc, &cc);

        pthread_mutex_lock(&file_table_mutex);
        int file_idx = -1;
        
        for (int i = 0; i < file_count; i++) {
            if (strcmp(file_table[i].path_on_ss, path_on_ss) == 0) {
                file_table[i].word_count = wc;
                file_table[i].char_count = cc;
                file_table[i].last_access = time(NULL);
                file_table[i].last_modified = time(NULL); // Update modification time
                printf("Updated metadata for %s\n", file_table[i].name);
                file_idx = i;
                break;
            }
        }
        
        // TRIGGER SYNCHRONOUS REPLICATION: File was modified (WRITE/APPEND operation completed)
        if (file_idx != -1) {
            FileMetadata meta = file_table[file_idx];
            pthread_mutex_unlock(&file_table_mutex);
            
            // Determine which server has the latest version (source)
            int source_ss_index = meta.ss_index; // Default to primary
            pthread_mutex_lock(&server_list_mutex);
            
            StorageServer source_ss = server_list[source_ss_index];
            pthread_mutex_unlock(&server_list_mutex);
            
            // NOTIFY ALL USERS WHO HAVE ACCESS TO THIS FILE (except the owner)
            pthread_mutex_lock(&file_table_mutex);
            FileMetadata file_meta = file_table[file_idx];
            pthread_mutex_unlock(&file_table_mutex);
            
            // Notify users in ACL
            for (int i = 0; i < file_meta.acl_count; i++) {
                char notif_msg[512];
                snprintf(notif_msg, sizeof(notif_msg),
                        "File '%s' (owned by %s) has been modified. New stats: %d words, %d chars",
                        file_meta.name, file_meta.owner, wc, cc);
                add_notification(NOTIF_FILE_MODIFIED, file_meta.acl[i].username, 
                               file_meta.owner, file_meta.name, notif_msg);
            }
            
            // Replicate to ALL other servers (including primary if write was on replica)
            pthread_mutex_lock(&server_list_mutex);
            
            // Build list of servers to sync: primary + all replicas, excluding source
            int servers_to_sync[MAX_REPLICAS + 1];
            int sync_count = 0;
            
            // Add primary if it's not the source
            if (meta.ss_index != source_ss_index) {
                servers_to_sync[sync_count++] = meta.ss_index;
            }
            
            // Add all replicas that aren't the source
            for (int i = 0; i < meta.replica_count; i++) {
                int replica_index = meta.replica_ss_indices[i];
                if (replica_index != source_ss_index) {
                    servers_to_sync[sync_count++] = replica_index;
                }
            }
            
            printf("[REPLICATION] Syncing file %s from SS %d to %d other servers\n",
                   meta.path_on_ss, source_ss_index, sync_count);
            
            for (int i = 0; i < sync_count; i++) {
                int target_index = servers_to_sync[i];
                
                // Skip offline servers
                if (server_list[target_index].status != SS_ONLINE) {
                    printf("[REPLICATION] Skipping offline target SS %d\n", target_index);
                    continue;
                }
                
                StorageServer target_ss = server_list[target_index];
                pthread_mutex_unlock(&server_list_mutex);
                
                // Connect to target SS and replicate synchronously
                int backup_sock = socket(AF_INET, SOCK_STREAM, 0);
                struct sockaddr_in backup_addr;
                memset(&backup_addr, 0, sizeof(backup_addr));
                backup_addr.sin_family = AF_INET;
                backup_addr.sin_port = htons(target_ss.port);
                inet_pton(AF_INET, target_ss.ip, &backup_addr.sin_addr);
                
                if (connect(backup_sock, (struct sockaddr *)&backup_addr, sizeof(backup_addr)) < 0) {
                    printf("[REPLICATION] Failed to connect to target SS %d\n", target_index);
                    pthread_mutex_lock(&server_list_mutex);
                    server_list[target_index].status = SS_FAILED;
                    continue;
                }
                
                // Send INTERNAL_REPLICATE command - replicate FROM source server
                char command[1024];
                sprintf(command, "INTERNAL_REPLICATE %s %s:%d", meta.path_on_ss, source_ss.ip, source_ss.port);
                send(backup_sock, command, strlen(command), 0);
                
                // WAIT for response (synchronous)
                char reply[256] = {0};
                recv(backup_sock, reply, sizeof(reply) - 1, 0);
                close(backup_sock);
                
                if (strncmp(reply, "200 OK", 6) == 0) {
                    printf("[REPLICATION] Successfully replicated %s to SS %d (synchronous)\n", meta.path_on_ss, target_index);
                } else {
                    printf("[REPLICATION] Failed to replicate %s to SS %d: %s\n", meta.path_on_ss, target_index, reply);
                }
                
                pthread_mutex_lock(&server_list_mutex);
            }
            pthread_mutex_unlock(&server_list_mutex);
        } else {
            pthread_mutex_unlock(&file_table_mutex);
        }
        
        // Save metadata to disk after update
        save_metadata();
        
        send(client_socket, "200 OK", 6, 0);

    } else if (strncmp(buffer, "CONNECT", 7) == 0) {
        // --- IT'S A USER CLIENT ---
        sscanf(buffer, "CONNECT %s", username);  // Use the username declared at function scope
        
        // Get client IP for logging
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        getpeername(client_socket, (struct sockaddr*)&client_addr, &addr_len);
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        int client_port = ntohs(client_addr.sin_port);
        
        // --- CHECK: Is this username already logged in? ---
        pthread_mutex_lock(&active_users_mutex);
        bool already_logged_in = false;
        for (int i = 0; i < active_user_count; i++) {
            if (strcmp(active_users[i], username) == 0) {
                already_logged_in = true;
                break;
            }
        }
        
        if (already_logged_in) {
            pthread_mutex_unlock(&active_users_mutex);
            printf("[Socket %d] User '%s' REJECTED: Already logged in\n", client_socket, username);
            log_event(LOG_WARNING, client_ip, client_port, username, "Login rejected: User already logged in");
            send(client_socket, "403 ERROR: User already logged in", 34, 0);
            close(client_socket);
            return NULL;
        }
        
        // Add to active users list
        if (active_user_count < MAX_USERS) {
            strcpy(active_users[active_user_count], username);
            active_user_count++;
        }
        pthread_mutex_unlock(&active_users_mutex);
        
        printf("[Socket %d] User '%s' connected.\n", client_socket, username);
        log_event(LOG_INFO, client_ip, client_port, username, "User connected");

        // --- Add user to global list ---
        pthread_mutex_lock(&user_list_mutex);
        bool found = false;
        for (int i = 0; i < global_user_count; i++) {
            if (strcmp(global_user_list[i], username) == 0) {
                found = true;
                break;
            }
        }
        if (!found && global_user_count < MAX_USERS) {
            strcpy(global_user_list[global_user_count], username);
            global_user_count++;
            pthread_mutex_unlock(&user_list_mutex); // Unlock before saving
            save_metadata(); // Save updated user list
        } else {
            pthread_mutex_unlock(&user_list_mutex);
        }

        send(client_socket, "200 OK", 6, 0);
        
        // Register client socket for push notifications
        pthread_mutex_lock(&active_clients_mutex);
        if (active_client_count < MAX_USERS) {
            strcpy(active_clients[active_client_count].username, username);
            active_clients[active_client_count].socket = client_socket;
            active_client_count++;
        }
        pthread_mutex_unlock(&active_clients_mutex);
        
        // SEND PENDING NOTIFICATIONS ON LOGIN
        send_pending_notifications(client_socket, username);

        // Main command loop for this user
        while (1) {
            memset(buffer, 0, BUFFER_SIZE);
            bytes_read = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);

            if (bytes_read <= 0) break; // Client disconnected
            
            buffer[bytes_read] = '\0';
            printf("[User %s] Received: %s\n", username, buffer);

            if (strcmp(buffer, "exit") == 0) break;

            // --- COMMAND PARSING ---
            if (strncmp(buffer, "CREATE", 6) == 0 && strncmp(buffer, "CREATEFOLDER", 12) != 0 ) {
                char command[100], filename[256];
                sscanf(buffer, "%s %s", command, filename);

                printf("[User %s] Requesting CREATE %s\n", username, filename);
                log_event(LOG_INFO, client_ip, client_port, username, "CREATE request: %s", filename);
                
                // 1. Check if file already exists using HashMap O(1)
                int existing_index = get_file_index(filename);
                
                if (existing_index != -1) {
                    log_event(LOG_WARNING, client_ip, client_port, username, 
                             "CREATE failed: %s already exists", filename);
                    send(client_socket, "409 ERROR: FILE ALREADY EXISTS", 30, 0);
                    continue;
                }
                
                // 2. Pick a Storage Server (Least Loaded - minimum files)
                pthread_mutex_lock(&server_list_mutex);
                if (server_count == 0) {
                    pthread_mutex_unlock(&server_list_mutex);
                    log_event(LOG_ERROR, client_ip, client_port, username, 
                             "CREATE failed: No storage servers available");
                    send(client_socket, "503 ERROR: NO STORAGE SERVERS", 27, 0);
                    continue;
                }
                
                // Find the least loaded storage server
                int ss_index = find_least_loaded_ss();
                
                if (ss_index == -1) {
                    pthread_mutex_unlock(&server_list_mutex);
                    log_event(LOG_ERROR, client_ip, client_port, username, 
                             "CREATE failed: No online storage servers");
                    send(client_socket, "503 ERROR: NO STORAGE SERVERS ONLINE", 36, 0);
                    continue;
                }
                
                StorageServer ss = server_list[ss_index];
                pthread_mutex_unlock(&server_list_mutex);

                printf("[CREATE] Using SS index=%d, IP=%s, port=%d\n", ss_index, ss.ip, ss.port);

                // 3. Generate a unique path with SS-specific directory
                char internal_path[512];
                sprintf(internal_path, "data_ss_%d/%s_%ld", ss.port, filename, time(NULL)); 
                
                printf("[CREATE] Internal path: %s\n", internal_path); 

                // 4. Try to connect to this Storage Server
                int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
                if (ss_sock < 0) {
                    perror("NS failed to create SS socket");
                    log_event(LOG_ERROR, client_ip, client_port, username, 
                             "CREATE failed: Socket creation error");
                    send(client_socket, "500 ERROR: Internal server error", 33, 0);
                    continue;
                }
                
                struct sockaddr_in ss_addr;
                memset(&ss_addr, 0, sizeof(ss_addr));
                ss_addr.sin_family = AF_INET;
                ss_addr.sin_port = htons(ss.port);
                inet_pton(AF_INET, ss.ip, &ss_addr.sin_addr);

                if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) {
                    printf("NS failed to connect to SS %s:%d\n", ss.ip, ss.port);
                    close(ss_sock);
                    pthread_mutex_lock(&server_list_mutex);
                    server_list[ss_index].status = SS_FAILED;
                    pthread_mutex_unlock(&server_list_mutex);
                    log_event(LOG_ERROR, client_ip, client_port, username, 
                             "CREATE failed: Cannot connect to storage server");
                    send(client_socket, "503 ERROR: STORAGE SERVER UNAVAILABLE", 38, 0);
                    continue;
                }
                
                // Successfully connected!
                
                // 5. Send the internal command
                char ss_command[BUFFER_SIZE];
                sprintf(ss_command, "INTERNAL_CREATE %s", internal_path);
                send(ss_sock, ss_command, strlen(ss_command), 0);
                    
                    // 6. Wait for SS reply
                    char ss_reply[BUFFER_SIZE] = {0};
                    recv(ss_sock, ss_reply, BUFFER_SIZE - 1, 0);
                    close(ss_sock);

                    // 7. If SS was OK, update our file table
                    if (strncmp(ss_reply, "200 OK", 6) == 0) {
                    
                    // REPLICATION: Pick backup servers BEFORE locking file_table_mutex
                    // to avoid deadlock (pick_backup_servers locks server_list_mutex then file_table_mutex)
                    int backup_indices[MAX_REPLICAS];
                    int backup_count;
                    pick_backup_servers(ss_index, backup_indices, &backup_count);
                    
                    pthread_mutex_lock(&file_table_mutex);
                    int new_file_index = file_count;  // Save index before incrementing
                    strcpy(file_table[file_count].name, filename);
                    strcpy(file_table[file_count].owner, username);
                    file_table[file_count].ss_index = ss_index;
                    strcpy(file_table[file_count].path_on_ss, internal_path);
                    file_table[file_count].char_count = 0;
                    file_table[file_count].word_count = 0;
                    file_table[file_count].creation_time = time(NULL);
                    file_table[file_count].last_access = time(NULL);
                    file_table[file_count].last_modified = time(NULL);
                    file_table[file_count].acl_count = 0; // Init ACL
                    
                    file_table[file_count].replica_count = backup_count;
                    for (int i = 0; i < backup_count; i++) {
                        file_table[file_count].replica_ss_indices[i] = backup_indices[i];
                    }
                    
                    file_count++;
                    
                    // Add to hashmap for O(1) future lookups
                    hashmap_put(&file_hashmap, filename, new_file_index);
                    
                    pthread_mutex_unlock(&file_table_mutex);
                    
                    // ASYNC REPLICATION: Spawn threads to replicate to backups
                    pthread_mutex_lock(&server_list_mutex);
                    StorageServer primary_ss = server_list[ss_index];
                    pthread_mutex_unlock(&server_list_mutex);
                    
                    for (int i = 0; i < backup_count; i++) {
                        typedef struct {
                            int backup_ss_index;
                            char path_on_ss[512];
                            char primary_ss_ip[100];
                            int primary_ss_port;
                        } ReplicateArgs;
                        
                        ReplicateArgs* args = malloc(sizeof(ReplicateArgs));
                        args->backup_ss_index = backup_indices[i];
                        strcpy(args->path_on_ss, internal_path);
                        strcpy(args->primary_ss_ip, primary_ss.ip);
                        args->primary_ss_port = primary_ss.port;
                        
                        pthread_t repl_thread;
                        pthread_create(&repl_thread, NULL, async_replicate_file, args);
                        pthread_detach(repl_thread);
                    }
                    
                    // Save metadata to disk
                    save_metadata();
                    
                    log_event(LOG_INFO, client_ip, client_port, username, 
                             "CREATE success: %s created on SS %s:%d", filename, ss.ip, ss.port);
                    send(client_socket, "200 OK: FILE CREATED", 20, 0);
                } else {
                    log_event(LOG_ERROR, client_ip, client_port, username, 
                             "CREATE failed: Storage server error for %s", filename);
                    send(client_socket, "500 ERROR: SS FAILED", 20, 0);
                }

            } else if (strncmp(buffer, "CREATEFOLDER", 12) == 0) {
                // --- CREATEFOLDER COMMAND ---
                char command[100], foldername[256];
                sscanf(buffer, "%s %s", command, foldername);

                printf("[User %s] Requesting CREATEFOLDER %s\n", username, foldername);
                
                // 1. Check if folder already exists
                pthread_mutex_lock(&file_table_mutex);
                bool exists = false;
                for (int i = 0; i < file_count; i++) {
                    if (strcmp(file_table[i].name, foldername) == 0) {
                        exists = true;
                        break;
                    }
                }
                pthread_mutex_unlock(&file_table_mutex);
                
                if (exists) {
                    send(client_socket, "409 ERROR: FOLDER ALREADY EXISTS", 32, 0);
                    continue;
                }
                
                // 2. Pick a Storage Server (Least Loaded - minimum files)
                pthread_mutex_lock(&server_list_mutex);
                if (server_count == 0) {
                    pthread_mutex_unlock(&server_list_mutex);
                    send(client_socket, "503 ERROR: NO STORAGE SERVERS", 29, 0);
                    continue;
                }
                
                // Find the least loaded storage server
                int ss_index = find_least_loaded_ss();
                
                if (ss_index == -1) {
                    pthread_mutex_unlock(&server_list_mutex);
                    send(client_socket, "503 ERROR: NO STORAGE SERVERS ONLINE", 36, 0);
                    continue;
                }
                
                StorageServer ss = server_list[ss_index];
                pthread_mutex_unlock(&server_list_mutex);

                // 3. Generate a unique physical path for the folder with SS-specific directory
                char internal_path[512];
                sprintf(internal_path, "data_ss_%d/%s_%ld", ss.port, foldername, time(NULL)); 

                // 4. Try to connect to Storage Server
                int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
                if (ss_sock < 0) {
                    perror("NS failed to create SS socket for folder");
                    send(client_socket, "500 ERROR: Internal server error", 33, 0);
                    continue;
                }
                
                struct sockaddr_in ss_addr;
                memset(&ss_addr, 0, sizeof(ss_addr));
                ss_addr.sin_family = AF_INET;
                ss_addr.sin_port = htons(ss.port);
                inet_pton(AF_INET, ss.ip, &ss_addr.sin_addr);

                if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) {
                    printf("NS failed to connect to SS %s:%d for folder\n", ss.ip, ss.port);
                    close(ss_sock);
                    pthread_mutex_lock(&server_list_mutex);
                    server_list[ss_index].status = SS_FAILED;
                    pthread_mutex_unlock(&server_list_mutex);
                    send(client_socket, "503 ERROR: STORAGE SERVER UNAVAILABLE", 38, 0);
                    continue;
                }
                
                // Successfully connected!
                
                // 5. Send INTERNAL_CREATEFOLDER command
                char ss_command[BUFFER_SIZE];
                sprintf(ss_command, "INTERNAL_CREATEFOLDER %s", internal_path);
                send(ss_sock, ss_command, strlen(ss_command), 0);
                    
                // 6. Wait for SS reply
                char ss_reply[BUFFER_SIZE] = {0};
                recv(ss_sock, ss_reply, BUFFER_SIZE - 1, 0);
                close(ss_sock);

                // 7. If SS was OK, update our file table
                if (strncmp(ss_reply, "200 OK", 6) == 0) {
                    pthread_mutex_lock(&file_table_mutex);
                    strcpy(file_table[file_count].name, foldername);
                    strcpy(file_table[file_count].owner, username);
                    file_table[file_count].ss_index = ss_index;
                    strcpy(file_table[file_count].path_on_ss, internal_path);
                    file_table[file_count].char_count = 0;
                    file_table[file_count].word_count = 0;
                    file_table[file_count].last_access = time(NULL);
                    file_table[file_count].acl_count = 0;
                    file_count++;
                    pthread_mutex_unlock(&file_table_mutex);
                    
                    // Save metadata to disk
                    save_metadata();
                    
                    printf("Folder created: %s -> %s on SS %d\n", foldername, internal_path, ss_index);
                    send(client_socket, "200 OK: FOLDER CREATED", 22, 0);
                } else {
                    printf("SS failed to create folder: %s\n", ss_reply);
                    send(client_socket, "500 ERROR: SS FAILED", 20, 0);
                }

            } else if ((strncmp(buffer, "VIEW", 4) == 0) && (strncmp(buffer, "VIEWFOLDER", 10) != 0) && (strncmp(buffer, "VIEWREQUESTS", 12) != 0)) {
                // --- FULL VIEW COMMAND LOGIC ---
                bool flag_all = false;
                bool flag_long = false;
                log_event(LOG_INFO, client_ip, client_port, username, 
                         "VIEW request: %s", buffer);
                printf("[User %s] Requesting VIEW: %s\n", username, buffer);
                if (strncmp(buffer, "VIEW -", 6) == 0) {
                    flag_all = (strstr(buffer, "a") != NULL);
                    flag_long = (strstr(buffer, "l") != NULL);
                }

                char reply_buffer[REPLY_BUFFER_SIZE];
                memset(reply_buffer, 0, sizeof(reply_buffer));
                int reply_len = 0;

                pthread_mutex_lock(&file_table_mutex);
                pthread_mutex_lock(&server_list_mutex);
                
                for (int i = 0; i < file_count; i++) {
                    // --- PERMISSION CHECK ---
                    bool has_access = check_permission(&file_table[i], username, false);
                    
                    if (flag_all || has_access) {
                        char line_buffer[MAX_LINE_LENGTH];
                        memset(line_buffer, 0, sizeof(line_buffer));
                        
                        if (flag_long) {
                            // Format for -l with server info
                            if (file_table[i].ss_index >= 0 && file_table[i].ss_index < MAX_SERVERS) {
                                StorageServer ss = server_list[file_table[i].ss_index];
                                snprintf(line_buffer, sizeof(line_buffer),
                                    "%-20s (Owner: %-10s, Chars: %-7d, Server: %s:%d)\n",
                                    file_table[i].name,
                                    file_table[i].owner[0] ? file_table[i].owner : "unknown",
                                    file_table[i].char_count,
                                    ss.ip, ss.port);
                            } else {
                                snprintf(line_buffer, sizeof(line_buffer),
                                    "%-20s (Owner: %-10s, Chars: %-7d, Server: N/A)\n",
                                    file_table[i].name,
                                    file_table[i].owner[0] ? file_table[i].owner : "unknown",
                                    file_table[i].char_count);
                            }
                        } else {
                            // Format for simple view
                            snprintf(line_buffer, sizeof(line_buffer), "%s\n", file_table[i].name);
                        }
                        
                        // Check if adding this line would overflow the buffer
                        int line_len = strlen(line_buffer);
                        if (reply_len + line_len < REPLY_BUFFER_SIZE - 1) {
                            memcpy(reply_buffer + reply_len, line_buffer, line_len);
                            reply_len += line_len;
                        } else {
                            // Buffer full - send what we have and continue
                            send(client_socket, reply_buffer, reply_len, 0);
                            memset(reply_buffer, 0, sizeof(reply_buffer));
                            reply_len = 0;
                            // Add current line to fresh buffer
                            memcpy(reply_buffer, line_buffer, line_len);
                            reply_len = line_len;
                        }
                    }
                }
                
                pthread_mutex_unlock(&server_list_mutex);
                pthread_mutex_unlock(&file_table_mutex);
                
                // Send remaining data or "(No files found)" message
                if (reply_len == 0) {
                    const char* msg = "(No files found)";
                    log_event(LOG_INFO, client_ip, client_port, username, 
                             "VIEW success: 0 files found");
                    send(client_socket, msg, strlen(msg), 0);
                } else {
                    log_event(LOG_INFO, client_ip, client_port, username, 
                             "VIEW success: %d bytes sent", reply_len);
                    send(client_socket, reply_buffer, reply_len, 0);
                }

            } else if (strncmp(buffer, "READ", 4) == 0) {
                // --- READ COMMAND ---
                char command[100], filename[256];
                sscanf(buffer, "%s %s", command, filename);
                printf("[User %s] Requesting READ %s\n", username, filename);
                log_event(LOG_INFO, client_ip, client_port, username, "READ request: %s", filename);

                // Use HashMap for O(1) lookup
                int file_index = get_file_index(filename);

                if (file_index == -1) {
                    log_event(LOG_WARNING, client_ip, client_port, username, 
                             "READ failed: %s not found", filename);
                    send(client_socket, "404 ERROR: FILE NOT FOUND", 25, 0);
                } else {
                    pthread_mutex_lock(&file_table_mutex);
                    if (!check_permission(&file_table[file_index], username, false)) {
                        pthread_mutex_unlock(&file_table_mutex);
                        log_event(LOG_WARNING, client_ip, client_port, username, 
                                 "READ failed: Permission denied for %s", filename);
                        send(client_socket, "403 ERROR: PERMISSION DENIED", 27, 0);
                    } else {
                        file_table[file_index].last_access = time(NULL);
                        FileMetadata meta = file_table[file_index];
                        pthread_mutex_unlock(&file_table_mutex);
                        
                        pthread_mutex_lock(&server_list_mutex);
                        
                        int chosen_ss_index = meta.ss_index;
                        StorageServer chosen_ss = server_list[chosen_ss_index];
                        
                        // Test if primary SS is actually reachable
                        int ss_reachable = test_ss_connection(chosen_ss.ip, chosen_ss.port);
                        
                        if (!ss_reachable) {
                            // Mark as failed if it was online
                            if (chosen_ss.status == SS_ONLINE) {
                                printf("[READ] Primary SS %d unreachable (marking as failed)\n", chosen_ss_index);
                                server_list[chosen_ss_index].status = SS_FAILED;
                            }
                            
                            // FAILOVER: Try replicas instead
                            bool found_replica = false;
                            char replica_path[512];
                            for (int i = 0; i < meta.replica_count; i++) {
                                int replica_index = meta.replica_ss_indices[i];
                                
                                // Check if replica is online and reachable
                                if (server_list[replica_index].status == SS_ONLINE &&
                                    test_ss_connection(server_list[replica_index].ip, 
                                                      server_list[replica_index].port)) {
                                    
                                    // Reconstruct path for replica SS's storage directory
                                    // Extract filename from primary path (e.g., "data_ss_9001/file.txt_123" -> "file.txt_123")
                                    char* filename = strrchr(meta.path_on_ss, '/');
                                    if (filename == NULL) {
                                        filename = meta.path_on_ss;
                                    } else {
                                        filename++; // Skip '/'
                                    }
                                    
                                    // Build path for replica SS (e.g., "data_ss_9002/file.txt_123")
                                    snprintf(replica_path, sizeof(replica_path), "data_ss_%d/%s", 
                                            server_list[replica_index].port, filename);
                                    
                                    chosen_ss_index = replica_index;
                                    chosen_ss = server_list[replica_index];
                                    found_replica = true;
                                    printf("[FAILOVER] Primary SS %d failed, using replica SS %d for %s (path: %s)\n", 
                                           meta.ss_index, replica_index, filename, replica_path);
                                    log_event(LOG_INFO, client_ip, client_port, username,
                                             "READ failover: Using replica SS %d for %s", replica_index, filename);
                                    break;
                                }
                            }
                            
                            if (!found_replica) {
                                pthread_mutex_unlock(&server_list_mutex);
                                log_event(LOG_ERROR, client_ip, client_port, username,
                                         "READ failed: All storage servers unavailable for %s", filename);
                                send(client_socket, "503 ERROR: ALL STORAGE SERVERS UNAVAILABLE", 42, 0);
                                continue;
                            }
                        }
                        
                        pthread_mutex_unlock(&server_list_mutex);
                        
                        // Construct correct path based on which SS we're using
                        char actual_path[512];
                        if (chosen_ss_index != meta.ss_index) {
                            // Failover to replica - construct replica path
                            char* filename_only = strrchr(meta.path_on_ss, '/');
                            if (filename_only) {
                                filename_only++; // Skip the '/'
                            } else {
                                filename_only = (char*)meta.path_on_ss;
                            }
                            snprintf(actual_path, sizeof(actual_path), "data_ss_%d/%s", 
                                    9000 + chosen_ss_index + 1, filename_only);
                        } else {
                            // Using primary - use original path
                            strcpy(actual_path, meta.path_on_ss);
                        }
                        
                        log_event(LOG_INFO, client_ip, client_port, username, 
                                 "READ success: %s from SS %s:%d", filename, chosen_ss.ip, chosen_ss.port);
                        char reply_buffer[BUFFER_SIZE];
                        sprintf(reply_buffer, "200 OK %s %d %s", chosen_ss.ip, chosen_ss.port, actual_path);
                        send(client_socket, reply_buffer, strlen(reply_buffer), 0);
                    }
                }

            } else if (strncmp(buffer, "WRITE", 5) == 0) {
                // --- WRITE COMMAND ---
                char command[100], filename[256];
                int sentence_num; 
                sscanf(buffer, "%s %s %d", command, filename, &sentence_num);
                log_event(LOG_INFO, client_ip, client_port, username, 
                         "WRITE request: %s sentence %d", filename, sentence_num);

                // Use HashMap for O(1) lookup
                int file_index = get_file_index(filename);

                if (file_index == -1) {
                    log_event(LOG_WARNING, client_ip, client_port, username, 
                             "WRITE failed: %s not found", filename);
                    send(client_socket, "404 ERROR: FILE NOT FOUND", 25, 0);
                } else {
                    pthread_mutex_lock(&file_table_mutex);
                    if (!check_permission(&file_table[file_index], username, true)) { // true = needs WRITE
                        pthread_mutex_unlock(&file_table_mutex);
                        log_event(LOG_WARNING, client_ip, client_port, username, 
                                 "WRITE failed: Permission denied for %s", filename);
                        send(client_socket, "403 ERROR: PERMISSION DENIED (WRITE)", 35, 0);
                    } else {
                        FileMetadata meta = file_table[file_index];
                        pthread_mutex_unlock(&file_table_mutex);
                        
                        pthread_mutex_lock(&server_list_mutex);
                        StorageServer ss = server_list[meta.ss_index];
                        int chosen_ss_index = meta.ss_index;
                        
                        // Check if primary SS is marked as failed
                        if (ss.status != SS_ONLINE) {
                            // FAILOVER: Try replicas for WRITE
                            bool found_replica = false;
                            pthread_mutex_lock(&file_table_mutex);
                            FileMetadata file_meta = file_table[file_index];
                            pthread_mutex_unlock(&file_table_mutex);
                            
                            for (int i = 0; i < file_meta.replica_count; i++) {
                                int replica_index = file_meta.replica_ss_indices[i];
                                
                                if (server_list[replica_index].status == SS_ONLINE) {
                                    chosen_ss_index = replica_index;
                                    ss = server_list[replica_index];
                                    found_replica = true;
                                    printf("[FAILOVER] Primary SS %d offline, using replica SS %d for WRITE to %s\n", 
                                           meta.ss_index, replica_index, filename);
                                    log_event(LOG_INFO, client_ip, client_port, username,
                                             "WRITE failover: Using replica SS %d for %s", replica_index, filename);
                                    break;
                                }
                            }
                            
                            if (!found_replica) {
                                pthread_mutex_unlock(&server_list_mutex);
                                log_event(LOG_ERROR, client_ip, client_port, username,
                                         "WRITE failed: All storage servers unavailable for %s", filename);
                                send(client_socket, "503 ERROR: ALL STORAGE SERVERS UNAVAILABLE", 42, 0);
                                continue;
            }
                        }
                        
                        pthread_mutex_unlock(&server_list_mutex);
                        
                        // Construct correct path based on which SS we're using
                        char actual_path[512];
                        if (chosen_ss_index != meta.ss_index) {
                            // Failover to replica - construct replica path
                            char* filename_only = strrchr(meta.path_on_ss, '/');
                            if (filename_only) {
                                filename_only++; // Skip the '/'
                            } else {
                                filename_only = (char*)meta.path_on_ss;
                            }
                            snprintf(actual_path, sizeof(actual_path), "data_ss_%d/%s", 
                                    9000 + chosen_ss_index + 1, filename_only);
                        } else {
                            // Using primary - use original path
                            strcpy(actual_path, meta.path_on_ss);
                        }
                        
                        log_event(LOG_INFO, client_ip, client_port, username, 
                                 "WRITE approved: %s to SS %s:%d (path: %s)", filename, ss.ip, ss.port, actual_path);
                        
                        char reply_buffer[BUFFER_SIZE];
                        sprintf(reply_buffer, "300 WRITE_OK %s %d %s %s", ss.ip, ss.port, actual_path, username);
                        send(client_socket, reply_buffer, strlen(reply_buffer), 0);
                    }
                }
            
            } else if (strncmp(buffer, "APPEND", 6) == 0) {
                // --- APPEND COMMAND ---
                char command[100], filename[256], content[BUFFER_SIZE];
                
                if (sscanf(buffer, "%s %s", command, filename) != 2) {
                    log_event(LOG_WARNING, client_ip, client_port, username, 
                             "APPEND failed: Invalid syntax");
                    send(client_socket, "400 ERROR: APPEND requires filename", 36, 0);
                    continue;
                }
                
                char* content_start = strstr(buffer, filename);
                if (content_start) {
                    content_start += strlen(filename);
                    while (*content_start == ' ') content_start++;
                    strncpy(content, content_start, BUFFER_SIZE - 1);
                    content[BUFFER_SIZE - 1] = '\0';
                } else {
                    log_event(LOG_WARNING, client_ip, client_port, username, 
                             "APPEND failed: No content provided for %s", filename);
                    send(client_socket, "400 ERROR: No content provided", 31, 0);
                    continue;
                }
                
                log_event(LOG_INFO, client_ip, client_port, username, 
                         "APPEND request: %s", filename);
                printf("[User %s] APPEND to %s: '%s'\n", username, filename, content);
                
                int file_index = -1;
                pthread_mutex_lock(&file_table_mutex);
                for (int i = 0; i < file_count; i++) {
                    if (strcmp(file_table[i].name, filename) == 0) {
                        file_index = i;
                        break;
                    }
                }
                
                if (file_index == -1) {
                    pthread_mutex_unlock(&file_table_mutex);
                    log_event(LOG_WARNING, client_ip, client_port, username, 
                             "APPEND failed: %s not found", filename);
                    send(client_socket, "404 ERROR: FILE NOT FOUND", 25, 0);
                } else if (!check_permission(&file_table[file_index], username, true)) { // true = needs WRITE
                     pthread_mutex_unlock(&file_table_mutex);
                     log_event(LOG_WARNING, client_ip, client_port, username, 
                              "APPEND failed: Permission denied for %s", filename);
                    send(client_socket, "403 ERROR: PERMISSION DENIED (WRITE)", 35, 0);
                } else {
                    FileMetadata meta = file_table[file_index];
                    pthread_mutex_unlock(&file_table_mutex);
                    
                    pthread_mutex_lock(&server_list_mutex);
                    StorageServer ss = server_list[meta.ss_index];
                    pthread_mutex_unlock(&server_list_mutex);
                    
                    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
                    struct sockaddr_in ss_addr;
                    memset(&ss_addr, 0, sizeof(ss_addr));
                    ss_addr.sin_family = AF_INET;
                    ss_addr.sin_port = htons(ss.port);
                    inet_pton(AF_INET, ss.ip, &ss_addr.sin_addr);
                    
                    if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) {
                        perror("NS failed to connect to SS for APPEND");
                        log_event(LOG_ERROR, client_ip, client_port, username, 
                                 "APPEND failed: Cannot connect to SS %s:%d", ss.ip, ss.port);
                        send(client_socket, "500 ERROR: INTERNAL", 19, 0);
                        close(ss_sock);
                        continue;
                    }
                    
                    char ss_command[BUFFER_SIZE];
                    snprintf(ss_command, BUFFER_SIZE, "INTERNAL_APPEND %s %s", meta.path_on_ss, content);
                    send(ss_sock, ss_command, strlen(ss_command), 0);
                    
                    char ss_reply[BUFFER_SIZE] = {0};
                    recv(ss_sock, ss_reply, BUFFER_SIZE - 1, 0);
                    close(ss_sock);
                    
                    if (strncmp(ss_reply, "200 OK", 6) == 0) {
                        log_event(LOG_INFO, client_ip, client_port, username, 
                                 "APPEND success: %s", filename);
                        send(client_socket, "200 OK: CONTENT APPENDED", 24, 0);
                    } else {
                        log_event(LOG_WARNING, client_ip, client_port, username, 
                                 "APPEND failed: SS returned %s", ss_reply);
                        send(client_socket, ss_reply, strlen(ss_reply), 0);
                    }
                }

            } else if (strncmp(buffer, "LIST", 4) == 0 && strncmp(buffer, "LISTCHECKPOINTS", 15) != 0 ) {
                // --- LIST COMMAND ---
                char reply_buffer[REPLY_BUFFER_SIZE] = "Registered Users:\n";
                
                pthread_mutex_lock(&user_list_mutex);
                for (int i = 0; i < global_user_count; i++) {
                    strncat(reply_buffer, global_user_list[i], MAX_LINE_LENGTH);
                    if(i!= global_user_count -1)
                    strncat(reply_buffer, "\n", 2);
                }
                pthread_mutex_unlock(&user_list_mutex);
                
                send(client_socket, reply_buffer, strlen(reply_buffer), 0);

            } else if (strncmp(buffer, "INFO", 4) == 0) {
                // --- INFO COMMAND ---
                char command[100], filename[256];
                sscanf(buffer, "%s %s", command, filename);
                log_event(LOG_INFO, client_ip, client_port, username, 
                         "INFO request: %s", filename);

                // Use HashMap for O(1) lookup
                int file_index = get_file_index(filename);

                if (file_index == -1) {
                    log_event(LOG_WARNING, client_ip, client_port, username, 
                             "INFO failed: %s not found", filename);
                    send(client_socket, "404 ERROR: FILE NOT FOUND", 25, 0);
                } else {
                    pthread_mutex_lock(&file_table_mutex);
                    if (!check_permission(&file_table[file_index], username, false)) { // Read check
                        pthread_mutex_unlock(&file_table_mutex);
                        log_event(LOG_WARNING, client_ip, client_port, username, 
                                 "INFO failed: Permission denied for %s", filename);
                        send(client_socket, "403 ERROR: PERMISSION DENIED", 27, 0);
                    } else {
                        FileMetadata* file = &file_table[file_index];
                        char reply_buffer[REPLY_BUFFER_SIZE];
                        char creation_time_str[100];
                        char last_access_str[100];
                        strftime(creation_time_str, sizeof(creation_time_str), "%Y-%m-%d %H:%M:%S", localtime(&file->creation_time));
                        strftime(last_access_str, sizeof(last_access_str), "%Y-%m-%d %H:%M:%S", localtime(&file->last_access));

                        // Get storage server info
                        pthread_mutex_lock(&server_list_mutex);
                        StorageServer ss = server_list[file->ss_index];
                        pthread_mutex_unlock(&server_list_mutex);

                        // Fetch sentence count from storage server
                        int sentence_count = 0;
                        int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
                        struct sockaddr_in ss_addr;
                        memset(&ss_addr, 0, sizeof(ss_addr));
                        ss_addr.sin_family = AF_INET;
                        ss_addr.sin_port = htons(ss.port);
                        inet_pton(AF_INET, ss.ip, &ss_addr.sin_addr);
                        
                        if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) >= 0) {
                            char ss_command[BUFFER_SIZE];
                            sprintf(ss_command, "GET_SENTENCE_COUNT %s", file->path_on_ss);
                            send(ss_sock, ss_command, strlen(ss_command), 0);
                            
                            char ss_reply[BUFFER_SIZE] = {0};
                            recv(ss_sock, ss_reply, BUFFER_SIZE - 1, 0);
                            close(ss_sock);
                            
                            sscanf(ss_reply, "%d", &sentence_count);
                        } else {
                            close(ss_sock);
                        }

                        snprintf(reply_buffer, REPLY_BUFFER_SIZE,
                            "File: %s\nOwner: %s\nSize: %d Chars, %d Words, %d Sentences\nCreated: %s\nLast Access: %s\nStorage Server: %s:%d\nInternal Path: %s\n",
                            file->name, file->owner, file->char_count, file->word_count, sentence_count, creation_time_str, last_access_str, ss.ip, ss.port, file->path_on_ss);

                        log_event(LOG_INFO, client_ip, client_port, username, 
                                 "INFO success: %s (%d chars, %d words, %d sentences)", 
                                 filename, file->char_count, file->word_count, sentence_count);

                        strcat(reply_buffer, "Access List:\n  - ");
                        strcat(reply_buffer, file->owner);
                        strcat(reply_buffer, " (Owner)\n");
                        for (int i = 0; i < file->acl_count; i++) {
                            strcat(reply_buffer, "  - ");
                            strcat(reply_buffer, file->acl[i].username);
                            if (file->acl[i].can_write) strcat(reply_buffer, " (R/W)\n");
                            else strcat(reply_buffer, " (R)\n");
                        }
                        send(client_socket, reply_buffer, strlen(reply_buffer), 0);
                        pthread_mutex_unlock(&file_table_mutex);
                    }
                }

            } else if (strncmp(buffer, "ADDACCESS", 9) == 0) {
                // --- ADDACCESS COMMAND ---
                char flag[10], filename[256], user_to_add[100];
               // --- FIX: Check sscanf return value ---
                int items_scanned = sscanf(buffer, "ADDACCESS %s %s %s", flag, filename, user_to_add);
                if (items_scanned != 3) {
                    send(client_socket, "400 ERROR: Bad format. Usage: ADDACCESS -R/-W <file> <user>", 60, 0);
                    continue; // Skip the rest of the logic
                }
                bool write_access = (strcmp(flag, "-W") == 0);

                // Use HashMap for O(1) lookup
                int file_index = get_file_index(filename);
                
                if (file_index == -1) {
                    send(client_socket, "404 ERROR: FILE NOT FOUND", 25, 0);
                } else {
                    pthread_mutex_lock(&file_table_mutex);
                    if (strcmp(file_table[file_index].owner, username) != 0) {
                        pthread_mutex_unlock(&file_table_mutex);
                        send(client_socket, "403 ERROR: Only the owner can change access", 44, 0);
                    } else {
                        // Check if user already exists in ACL
                        int existing_idx = -1;
                        for (int i = 0; i < file_table[file_index].acl_count; i++) {
                            if (strcmp(file_table[file_index].acl[i].username, user_to_add) == 0) {
                                existing_idx = i;
                                break;
                            }
                        }
                        
                        if (existing_idx != -1) {
                            // User already exists - UPDATE their permission
                            file_table[file_index].acl[existing_idx].can_write = write_access;
                            printf("[ACL] Updated %s's permission on %s to %s\n", 
                                   user_to_add, filename, write_access ? "R/W" : "R");
                            send(client_socket, "200 OK: Access updated", 22, 0);
                            pthread_mutex_unlock(&file_table_mutex);
                            save_metadata();
                        } else {
                            // User doesn't exist - ADD new entry
                            int acl_idx = file_table[file_index].acl_count;
                            if (acl_idx < MAX_ACL_USERS) {
                                strcpy(file_table[file_index].acl[acl_idx].username, user_to_add);
                                file_table[file_index].acl[acl_idx].can_write = write_access;
                                file_table[file_index].acl_count++;
                                printf("[ACL] Added %s to %s with %s permission\n", 
                                       user_to_add, filename, write_access ? "R/W" : "R");
                                send(client_socket, "200 OK: Access granted", 22, 0);
                                pthread_mutex_unlock(&file_table_mutex);
                                save_metadata();
                            } else {
                                pthread_mutex_unlock(&file_table_mutex);
                                send(client_socket, "500 ERROR: ACL is full", 22, 0);
                            }
                        }
                    }
                }

            } else if (strncmp(buffer, "REMACCESS", 9) == 0) {
                // --- REMACCESS COMMAND ---
                char filename[256], user_to_remove[100];
                // --- FIX: Check sscanf return value ---
                char flag[10];
                int items_scanned = sscanf(buffer, "REMACCESS %s %s %s", flag, filename, user_to_remove);
                if (items_scanned != 3) {
                    send(client_socket, "400 ERROR: Bad format. Usage: REMACCESS <file> <user>", 60, 0);
                    continue; // Skip the rest of the logic
                }

                // Use HashMap for O(1) lookup
                int file_index = get_file_index(filename);
                
                if (file_index == -1) {
                    send(client_socket, "404 ERROR: FILE NOT FOUND", 25, 0);
                } else {
                    pthread_mutex_lock(&file_table_mutex);
                    if (strcmp(file_table[file_index].owner, username) != 0) {
                        pthread_mutex_unlock(&file_table_mutex);
                        send(client_socket, "403 ERROR: Only the owner can change access", 44, 0);
                    } else {
                        int user_acl_index = -1;
                        for(int i=0; i < file_table[file_index].acl_count; i++) {
                            if(strcmp(file_table[file_index].acl[i].username, user_to_remove) == 0) {
                                user_acl_index = i;
                                break;
                            }
                        }
                        
                        if (user_acl_index != -1) {
                            // Fast remove: swap with last element
                            file_table[file_index].acl[user_acl_index] = file_table[file_index].acl[file_table[file_index].acl_count - 1];
                            file_table[file_index].acl_count--;
                            send(client_socket, "200 OK: Access removed", 22, 0);
                            pthread_mutex_unlock(&file_table_mutex); // Unlock before saving
                            save_metadata();
                        } else {
                            pthread_mutex_unlock(&file_table_mutex);
                            send(client_socket, "404 ERROR: User not in ACL", 27, 0);
                        }
                    }
                }

            } else if (strncmp(buffer, "DELETE", 6) == 0) {
                // --- DELETE COMMAND ---
                char command[100], filename[256];
                sscanf(buffer, "%s %s", command, filename);
                log_event(LOG_INFO, client_ip, client_port, username, 
                         "DELETE request: %s", filename);

                // Use HashMap for O(1) lookup
                int file_index = get_file_index(filename);
                
                if (file_index == -1) {
                    log_event(LOG_WARNING, client_ip, client_port, username, 
                             "DELETE failed: %s not found", filename);
                    send(client_socket, "404 ERROR: FILE NOT FOUND", 25, 0);
                } else {
                    pthread_mutex_lock(&file_table_mutex);
                    if (strcmp(file_table[file_index].owner, username) != 0) {
                        pthread_mutex_unlock(&file_table_mutex);
                        log_event(LOG_WARNING, client_ip, client_port, username, 
                                 "DELETE failed: User %s is not owner of %s", username, filename);
                        send(client_socket, "403 ERROR: Only the owner can delete", 36, 0);
                    } else {
                        FileMetadata meta = file_table[file_index];
                        
                        // Validate storage server index
                        pthread_mutex_lock(&server_list_mutex);
                        if (meta.ss_index < 0 || meta.ss_index >= server_count) {
                            pthread_mutex_unlock(&server_list_mutex);
                            pthread_mutex_unlock(&file_table_mutex);
                            log_event(LOG_ERROR, client_ip, client_port, username, 
                                     "DELETE failed: Invalid SS index %d for %s", meta.ss_index, filename);
                            send(client_socket, "500 ERROR: Invalid storage server reference", 44, 0);
                            continue;
                        }
                        
                        StorageServer ss = server_list[meta.ss_index];
                        pthread_mutex_unlock(&server_list_mutex);
                        
                        printf("[DELETE] Attempting to delete %s from SS %s:%d (index %d)\n", 
                               filename, ss.ip, ss.port, meta.ss_index);

                        int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
                        struct sockaddr_in ss_addr;
                        memset(&ss_addr, 0, sizeof(ss_addr));
                        ss_addr.sin_family = AF_INET;
                        ss_addr.sin_port = htons(ss.port);
                        inet_pton(AF_INET, ss.ip, &ss_addr.sin_addr);

                        if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) {
                            perror("NS failed to connect to SS for DELETE");
                            log_event(LOG_ERROR, client_ip, client_port, username, 
                                     "DELETE failed: Cannot connect to SS %s:%d", ss.ip, ss.port);
                            send(client_socket, "500 ERROR: INTERNAL", 19, 0);
                            close(ss_sock);
                            pthread_mutex_unlock(&file_table_mutex);
                            continue;
                        }
                        
                        char ss_command[BUFFER_SIZE];
                        sprintf(ss_command, "INTERNAL_DELETE %s", meta.path_on_ss);
                        send(ss_sock, ss_command, strlen(ss_command), 0);
                        
                        char ss_reply[BUFFER_SIZE] = {0};
                        recv(ss_sock, ss_reply, BUFFER_SIZE - 1, 0);
                        close(ss_sock);

                        if (strncmp(ss_reply, "200 OK", 6) == 0) {
                            // Remove deleted file from HashMap and Cache
                            hashmap_remove(&file_hashmap, filename);
                            cache_remove(&file_cache, filename);
                            
                            // Fast remove from table by swapping with last element
                            if (file_index != file_count - 1) {
                                // A file is being moved - update its HashMap entry
                                char* moved_filename = file_table[file_count - 1].name;
                                file_table[file_index] = file_table[file_count - 1];
                                // Update HashMap to point to new index for the moved file
                                hashmap_put(&file_hashmap, moved_filename, file_index);
                                // Update cache if the moved file is cached
                                cache_put(&file_cache, moved_filename, file_index);
                            }
                            file_count--;
                            
                            log_event(LOG_INFO, client_ip, client_port, username, 
                                     "DELETE success: %s", filename);
                            send(client_socket, "200 OK: FILE DELETED", 20, 0);
                            pthread_mutex_unlock(&file_table_mutex); // Unlock before saving
                            save_metadata();
                        } else if (strncmp(ss_reply, "423 LOCKED", 10) == 0) {
                            pthread_mutex_unlock(&file_table_mutex);
                            log_event(LOG_WARNING, client_ip, client_port, username, 
                                     "DELETE failed: %s is locked (being written)", filename);
                            send(client_socket, "423 LOCKED: File is being written by another user", 50, 0);
                        } else {
                            pthread_mutex_unlock(&file_table_mutex);
                            log_event(LOG_WARNING, client_ip, client_port, username, 
                                     "DELETE failed: SS returned %s", ss_reply);
                            send(client_socket, "500 ERROR: SS failed to delete", 30, 0);
                        }
                    }
                }
            
            } else if (strncmp(buffer, "DELETEFOLDER", 12) == 0) {
                // --- DELETEFOLDER COMMAND ---
                char command[100], foldername[256];
                sscanf(buffer, "%s %s", command, foldername);

                printf("[User %s] Requesting DELETEFOLDER %s\n", username, foldername);

                pthread_mutex_lock(&file_table_mutex);
                int folder_index = -1;
                for (int i = 0; i < file_count; i++) {
                    if (strcmp(file_table[i].name, foldername) == 0) {
                        folder_index = i;
                        break;
                    }
                }
                
                if (folder_index == -1) {
                    pthread_mutex_unlock(&file_table_mutex);
                    send(client_socket, "404 ERROR: FOLDER NOT FOUND", 27, 0);
                } else if (strcmp(file_table[folder_index].owner, username) != 0) {
                    pthread_mutex_unlock(&file_table_mutex);
                    send(client_socket, "403 ERROR: Only the owner can delete", 36, 0);
                } else {
                    FileMetadata meta = file_table[folder_index];
                    
                    pthread_mutex_lock(&server_list_mutex);
                    StorageServer ss = server_list[meta.ss_index];
                    pthread_mutex_unlock(&server_list_mutex);

                    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
                    struct sockaddr_in ss_addr;
                    memset(&ss_addr, 0, sizeof(ss_addr));
                    ss_addr.sin_family = AF_INET;
                    ss_addr.sin_port = htons(ss.port);
                    inet_pton(AF_INET, ss.ip, &ss_addr.sin_addr);

                    if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) {
                        perror("NS failed to connect to SS for DELETEFOLDER");
                        send(client_socket, "500 ERROR: INTERNAL", 19, 0);
                        close(ss_sock);
                        pthread_mutex_unlock(&file_table_mutex);
                        continue;
                    }
                    
                    char ss_command[BUFFER_SIZE];
                    sprintf(ss_command, "INTERNAL_DELETEFOLDER %s", meta.path_on_ss);
                    send(ss_sock, ss_command, strlen(ss_command), 0);
                    
                    char ss_reply[BUFFER_SIZE] = {0};
                    recv(ss_sock, ss_reply, BUFFER_SIZE - 1, 0);
                    close(ss_sock);

                    if (strncmp(ss_reply, "200 OK", 6) == 0) {
                        // Fast remove from table
                        file_table[folder_index] = file_table[file_count - 1];
                        file_count--;
                        
                        printf("Folder deleted: %s\n", foldername);
                        send(client_socket, "200 OK: FOLDER DELETED", 22, 0);
                        pthread_mutex_unlock(&file_table_mutex);
                        save_metadata();
                    } else {
                        pthread_mutex_unlock(&file_table_mutex);
                        printf("SS failed to delete folder: %s\n", ss_reply);
                        send(client_socket, ss_reply, strlen(ss_reply), 0);
                    }
                }
            
            } else if (strncmp(buffer, "MOVE", 4) == 0 && buffer[4] == ' ') {
                // --- MOVE COMMAND: Move file/folder into another folder ---
                char command[100], filename[256], foldername[256];
                sscanf(buffer, "%s %s %s", command, filename, foldername);
                printf("[User %s] Requesting MOVE %s -> folder %s\n", username, filename, foldername);

                pthread_mutex_lock(&file_table_mutex);
                
                // 1. Find the file/folder to move
                int file_index = -1;
                for (int i = 0; i < file_count; i++) {
                    if (strcmp(file_table[i].name, filename) == 0) {
                        file_index = i;
                        break;
                    }
                }
                
                if (file_index == -1) {
                    pthread_mutex_unlock(&file_table_mutex);
                    send(client_socket, "404 ERROR: SOURCE NOT FOUND", 27, 0);
                } else if (strcmp(file_table[file_index].owner, username) != 0) {
                    pthread_mutex_unlock(&file_table_mutex);
                    send(client_socket, "403 ERROR: Only the owner can move", 34, 0);
                } else {
                    // 2. Check if destination folder exists
                    int folder_index = -1;
                    for (int i = 0; i < file_count; i++) {
                        if (strcmp(file_table[i].name, foldername) == 0) {
                            folder_index = i;
                            break;
                        }
                    }
                    
                    if (folder_index == -1) {
                        pthread_mutex_unlock(&file_table_mutex);
                        send(client_socket, "404 ERROR: DESTINATION FOLDER NOT FOUND", 39, 0);
                    } else {
                        // 3. Get file metadata and folder path
                        FileMetadata file_meta = file_table[file_index];
                        FileMetadata folder_meta = file_table[folder_index];
                        
                        // 4. Build new hierarchical path: foldername/filename
                        char new_logical_path[512];
                        snprintf(new_logical_path, sizeof(new_logical_path), "%s/%s", foldername, filename);
                        
                        // 5. Check if new path already exists
                        int conflict = 0;
                        for (int i = 0; i < file_count; i++) {
                            if (strcmp(file_table[i].name, new_logical_path) == 0) {
                                conflict = 1;
                                break;
                            }
                        }
                        
                        if (conflict) {
                            pthread_mutex_unlock(&file_table_mutex);
                            send(client_socket, "409 ERROR: FILE ALREADY EXISTS IN FOLDER", 40, 0);
                        } else {
                            // 6. Build new physical path inside folder's directory
                            char new_physical_path[512];
                            // Extract base filename from old physical path
                            char *base = strrchr(file_meta.path_on_ss, '/');
                            if (base == NULL) base = file_meta.path_on_ss;
                            else base++; // Skip the '/'
                            
                            snprintf(new_physical_path, sizeof(new_physical_path), "%s/%s", 
                                    folder_meta.path_on_ss, base);
                            
                            pthread_mutex_unlock(&file_table_mutex);
                            
                            // 7. Connect to Storage Server and physically move the file
                            pthread_mutex_lock(&server_list_mutex);
                            StorageServer ss = server_list[file_meta.ss_index];
                            pthread_mutex_unlock(&server_list_mutex);
                            
                            int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
                            struct sockaddr_in ss_addr;
                            memset(&ss_addr, 0, sizeof(ss_addr));
                            ss_addr.sin_family = AF_INET;
                            ss_addr.sin_port = htons(ss.port);
                            inet_pton(AF_INET, ss.ip, &ss_addr.sin_addr);
                            
                            if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) {
                                perror("NS failed to connect to SS for MOVE");
                                send(client_socket, "500 ERROR: INTERNAL", 19, 0);
                                close(ss_sock);
                                continue;
                            }
                            
                            // Send INTERNAL_MOVE command
                            char ss_command[BUFFER_SIZE];
                            sprintf(ss_command, "INTERNAL_MOVE %s %s", file_meta.path_on_ss, new_physical_path);
                            send(ss_sock, ss_command, strlen(ss_command), 0);
                            
                            char ss_reply[BUFFER_SIZE] = {0};
                            recv(ss_sock, ss_reply, BUFFER_SIZE - 1, 0);
                            close(ss_sock);
                            
                            // 8. If SS succeeded, update metadata
                            if (strncmp(ss_reply, "200 OK", 6) == 0) {
                                pthread_mutex_lock(&file_table_mutex);
                                
                                // Update hashmap: remove old key, add new key
                                hashmap_remove(&file_hashmap, filename);
                                hashmap_put(&file_hashmap, new_logical_path, file_index);
                                
                                strncpy(file_table[file_index].name, new_logical_path, 
                                       sizeof(file_table[file_index].name) - 1);
                                strncpy(file_table[file_index].path_on_ss, new_physical_path,
                                       sizeof(file_table[file_index].path_on_ss) - 1);
                                pthread_mutex_unlock(&file_table_mutex);
                                
                                printf("Moved: %s -> %s (physical: %s -> %s)\n", 
                                      filename, new_logical_path, 
                                      file_meta.path_on_ss, new_physical_path);
                                send(client_socket, "200 OK: MOVED", 13, 0);
                                save_metadata();
                            } else {
                                printf("SS failed to move: %s\n", ss_reply);
                                send(client_socket, ss_reply, strlen(ss_reply), 0);
                            }
                        }
                    }
                }
            
            } else if (strncmp(buffer, "VIEWFOLDER", 10) == 0) {
                // --- VIEWFOLDER COMMAND: List contents of a folder ---
                char command[100], foldername[256];
                sscanf(buffer, "%s %s", command, foldername);
                printf("[User %s] Requesting VIEWFOLDER %s\n", username, foldername);

                pthread_mutex_lock(&file_table_mutex);
                int folder_index = -1;
                for (int i = 0; i < file_count; i++) {
                    if (strcmp(file_table[i].name, foldername) == 0) {
                        folder_index = i;
                        break;
                    }
                }
                
                if (folder_index == -1) {
                    pthread_mutex_unlock(&file_table_mutex);
                    send(client_socket, "404 ERROR: FOLDER NOT FOUND", 27, 0);
                } else if (!check_permission(&file_table[folder_index], username, false)) {
                    pthread_mutex_unlock(&file_table_mutex);
                    send(client_socket, "403 ERROR: PERMISSION DENIED", 28, 0);
                } else {
                    // Build list of all files/folders in this folder
                    char response[BUFFER_SIZE * 4] = "200 OK:\n";
                    int count = 0;
                    
                    for (int i = 0; i < file_count; i++) {
                        // Check if this file's name starts with "foldername/"
                        int len = strlen(foldername);
                        if (strncmp(file_table[i].name, foldername, len) == 0 && 
                            file_table[i].name[len] == '/') {
                            
                            // Extract the direct child name (not nested subdirectories)
                            char *child_name = file_table[i].name + len + 1;
                            char *slash = strchr(child_name, '/');
                            
                            if (slash == NULL) {
                                // Direct child - no further nesting
                                char line[512];
                                // Show relative child name (not full path)
                                sprintf(line, "%s (owner: %s)\n", child_name, file_table[i].owner);

                                if (strlen(response) + strlen(line) < sizeof(response) - 1) {
                                    strcat(response, line);
                                    count++;
                                }
                            }
                        }
                    }
                    
                    if (count == 0) {
                        strcat(response, "(empty folder)\n");
                    }
                    
                    pthread_mutex_unlock(&file_table_mutex);
                    send(client_socket, response, strlen(response), 0);
                }
            
            } else if (strncmp(buffer, "STREAM", 6) == 0) {
                // --- STREAM COMMAND: Word-by-word streaming ---
                char command[100], filename[256];
                sscanf(buffer, "%s %s", command, filename);
                printf("[User %s] Requesting STREAM %s\n", username, filename);

                // Use HashMap for O(1) lookup
                int file_index = get_file_index(filename);

                if (file_index == -1) {
                    send(client_socket, "404 ERROR: FILE NOT FOUND", 25, 0);
                } else {
                    pthread_mutex_lock(&file_table_mutex);
                    if (!check_permission(&file_table[file_index], username, false)) {
                        pthread_mutex_unlock(&file_table_mutex);
                        send(client_socket, "403 ERROR: PERMISSION DENIED", 27, 0);
                    } else {
                        file_table[file_index].last_access = time(NULL);
                        FileMetadata meta = file_table[file_index];
                        pthread_mutex_unlock(&file_table_mutex);
                        
                        pthread_mutex_lock(&server_list_mutex);
                        StorageServer ss = server_list[meta.ss_index];
                        pthread_mutex_unlock(&server_list_mutex);
                        
                        char reply_buffer[BUFFER_SIZE];
                        sprintf(reply_buffer, "400 STREAM_OK %s %d %s", ss.ip, ss.port, meta.path_on_ss);
                        send(client_socket, reply_buffer, strlen(reply_buffer), 0);
                    }
                }

            } else if (strncmp(buffer, "UNDO", 4) == 0) {
                // --- UNDO COMMAND: Revert last file change ---
                char command[100], filename[256];
                sscanf(buffer, "%s %s", command, filename);
                printf("[User %s] Requesting UNDO %s\n", username, filename);

                // Use HashMap for O(1) lookup
                int file_index = get_file_index(filename);

                if (file_index == -1) {
                    send(client_socket, "404 ERROR: FILE NOT FOUND", 25, 0);
                } else {
                    pthread_mutex_lock(&file_table_mutex);
                    if (!check_permission(&file_table[file_index], username, true)) {
                        // UNDO requires write permission
                        pthread_mutex_unlock(&file_table_mutex);
                        send(client_socket, "403 ERROR: PERMISSION DENIED (WRITE)", 35, 0);
                    } else {
                        FileMetadata meta = file_table[file_index];
                        pthread_mutex_unlock(&file_table_mutex);
                        
                        pthread_mutex_lock(&server_list_mutex);
                        StorageServer ss = server_list[meta.ss_index];
                        pthread_mutex_unlock(&server_list_mutex);
                        
                        // Connect to SS and send UNDO command
                        int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
                        struct sockaddr_in ss_addr;
                        memset(&ss_addr, 0, sizeof(ss_addr));
                        ss_addr.sin_family = AF_INET;
                        ss_addr.sin_port = htons(ss.port);
                        inet_pton(AF_INET, ss.ip, &ss_addr.sin_addr);

                        if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) {
                            perror("NS failed to connect to SS for UNDO");
                            send(client_socket, "500 ERROR: INTERNAL", 19, 0);
                            close(ss_sock);
                            continue;
                        }
                        
                        char ss_command[BUFFER_SIZE];
                        sprintf(ss_command, "INTERNAL_UNDO %s", meta.path_on_ss);
                        send(ss_sock, ss_command, strlen(ss_command), 0);
                        
                        char ss_reply[BUFFER_SIZE] = {0};
                        recv(ss_sock, ss_reply, BUFFER_SIZE - 1, 0);
                        close(ss_sock);
                        
                        send(client_socket, ss_reply, strlen(ss_reply), 0);
                    }
                }

            } else if (strncmp(buffer, "REDO", 4) == 0) {
                // --- REDO COMMAND: Restore previously undone changes ---
                char command[100], filename[256];
                sscanf(buffer, "%s %s", command, filename);
                printf("[User %s] Requesting REDO %s\n", username, filename);

                // Use HashMap for O(1) lookup
                int file_index = get_file_index(filename);

                if (file_index == -1) {
                    send(client_socket, "404 ERROR: FILE NOT FOUND", 25, 0);
                } else {
                    pthread_mutex_lock(&file_table_mutex);
                    if (!check_permission(&file_table[file_index], username, true)) {
                        // REDO requires write permission
                        pthread_mutex_unlock(&file_table_mutex);
                        send(client_socket, "403 ERROR: PERMISSION DENIED (WRITE)", 35, 0);
                    } else {
                        FileMetadata meta = file_table[file_index];
                        pthread_mutex_unlock(&file_table_mutex);
                        
                        pthread_mutex_lock(&server_list_mutex);
                        StorageServer ss = server_list[meta.ss_index];
                        pthread_mutex_unlock(&server_list_mutex);
                        
                        // Connect to SS and send REDO command
                        int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
                        struct sockaddr_in ss_addr;
                        memset(&ss_addr, 0, sizeof(ss_addr));
                        ss_addr.sin_family = AF_INET;
                        ss_addr.sin_port = htons(ss.port);
                        inet_pton(AF_INET, ss.ip, &ss_addr.sin_addr);

                        if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) {
                            perror("NS failed to connect to SS for REDO");
                            send(client_socket, "500 ERROR: INTERNAL", 19, 0);
                            close(ss_sock);
                            continue;
                        }
                        
                        char ss_command[BUFFER_SIZE];
                        sprintf(ss_command, "INTERNAL_REDO %s", meta.path_on_ss);
                        send(ss_sock, ss_command, strlen(ss_command), 0);
                        
                        char ss_reply[BUFFER_SIZE] = {0};
                        recv(ss_sock, ss_reply, BUFFER_SIZE - 1, 0);
                        close(ss_sock);
                        
                        send(client_socket, ss_reply, strlen(ss_reply), 0);
                    }
                }

            } else if (strncmp(buffer, "EXEC", 4) == 0) {
                // --- EXEC COMMAND: Execute file content as shell commands ---
                char command[100], filename[256];
                sscanf(buffer, "%s %s", command, filename);
                log_event(LOG_INFO, client_ip, client_port, username, 
                         "EXEC request: %s", filename);
                printf("[User %s] Requesting EXEC %s\n", username, filename);

                int file_index = -1;
                pthread_mutex_lock(&file_table_mutex);
                for (int i = 0; i < file_count; i++) {
                    if (strcmp(file_table[i].name, filename) == 0) {
                        file_index = i;
                        break;
                    }
                }

                if (file_index == -1) {
                    pthread_mutex_unlock(&file_table_mutex);
                    log_event(LOG_WARNING, client_ip, client_port, username, 
                             "EXEC failed: %s not found", filename);
                    send(client_socket, "404 ERROR: FILE NOT FOUND", 25, 0);
                } else if (!check_permission(&file_table[file_index], username, false)) {
                    pthread_mutex_unlock(&file_table_mutex);
                    log_event(LOG_WARNING, client_ip, client_port, username, 
                             "EXEC failed: Permission denied for %s", filename);
                    send(client_socket, "403 ERROR: PERMISSION DENIED", 27, 0);
                } else {
                    FileMetadata meta = file_table[file_index];
                    pthread_mutex_unlock(&file_table_mutex);
                    
                    pthread_mutex_lock(&server_list_mutex);
                    StorageServer ss = server_list[meta.ss_index];
                    pthread_mutex_unlock(&server_list_mutex);
                    
                    // Fetch file content from storage server
                    int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
                    struct sockaddr_in ss_addr;
                    memset(&ss_addr, 0, sizeof(ss_addr));
                    ss_addr.sin_family = AF_INET;
                    ss_addr.sin_port = htons(ss.port);
                    inet_pton(AF_INET, ss.ip, &ss_addr.sin_addr);

                    if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) {
                        perror("NS failed to connect to SS for EXEC");
                        log_event(LOG_ERROR, client_ip, client_port, username, 
                                 "EXEC failed: Cannot connect to SS %s:%d", ss.ip, ss.port);
                        send(client_socket, "500 ERROR: INTERNAL", 19, 0);
                        close(ss_sock);
                        continue;
                    }
                    
                    char ss_command[BUFFER_SIZE];
                    sprintf(ss_command, "CLIENT_READ %s", meta.path_on_ss);
                    send(ss_sock, ss_command, strlen(ss_command), 0);
                    
                    // Read file content
                    char file_content[8192] = {0};
                    int total_bytes = 0;
                    int bytes;
                    while ((bytes = recv(ss_sock, file_content + total_bytes, sizeof(file_content) - total_bytes - 1, 0)) > 0) {
                        total_bytes += bytes;
                        if (total_bytes >= sizeof(file_content) - 1) break;
                    }
                    close(ss_sock);
                    file_content[total_bytes] = '\0';
                    
                    // Execute the commands on NAMESERVER
                    printf("[EXEC] Executing commands from %s:\n%s\n", filename, file_content);
                    
                    // Check if file content is empty
                    if (strlen(file_content) == 0 || file_content[0] == '\0') {
                        send(client_socket, "400 ERROR: File is empty, nothing to execute", 44, 0);
                        continue;
                    }
                    
                    // Execute file content directly as bash script
                    printf("[EXEC] Executing file content as bash script\n");

                    // Normalize content: remove any CR characters (\r) that may come from Windows-style newlines
                    char normalized_content[8192];
                    int nidx = 0;
                    for (int i = 0; file_content[i] != '\0' && nidx < (int)sizeof(normalized_content) - 1; i++) {
                        char cc = file_content[i];
                        if (cc == '\r') continue; // skip CR
                        normalized_content[nidx++] = cc;
                    }
                    normalized_content[nidx] = '\0';

                    FILE* pipe = popen(normalized_content, "r");
                    if (pipe == NULL) {
                        send(client_socket, "500 ERROR: Failed to execute command", 37, 0);
                        log_event(LOG_ERROR, client_ip, client_port, username, 
                                 "EXEC failed: popen failed for %s", filename);
                        continue;
                    }
                    
                    // Read command output
                    char output_buffer[8192] = {0};
                    size_t output_len = fread(output_buffer, 1, sizeof(output_buffer) - 1, pipe);
                    output_buffer[output_len] = '\0';
                    
                    int exit_status = pclose(pipe);
                    
                    // Check if command executed successfully
                    if (exit_status != 0) {
                        // Command failed - send error with output
                        char error_response[8192 + 100];
                        if (output_len > 0) {
                            snprintf(error_response, sizeof(error_response), 
                                    "500 ERROR: Command execution failed (exit code %d):\n%s", 
                                    WEXITSTATUS(exit_status), output_buffer);
                        } else {
                            snprintf(error_response, sizeof(error_response), 
                                    "500 ERROR: Command execution failed (exit code %d)", 
                                    WEXITSTATUS(exit_status));
                        }
                        log_event(LOG_ERROR, client_ip, client_port, username, 
                                 "EXEC failed: exit code %d for %s", 
                                 WEXITSTATUS(exit_status), filename);
                        send(client_socket, error_response, strlen(error_response), 0);
                    } else {
                        // Send execution output to client
                        char response[8192 + 100];
                        if (output_len > 0) {
                            snprintf(response, sizeof(response), "200 EXEC_OUTPUT:\n%s", output_buffer);
                        } else {
                            snprintf(response, sizeof(response), "200 EXEC_OUTPUT:\n[Command executed successfully with no output]\n");
                        }
                        log_event(LOG_INFO, client_ip, client_port, username, 
                                 "EXEC success: %s executed successfully", filename);
                        send(client_socket, response, strlen(response), 0);
                    }
                }

            } else if (strncmp(buffer, "CHECKPOINT", 10) == 0) {
                // --- CHECKPOINT COMMAND ---
                char command[100], filename[256], checkpoint_tag[256];
                sscanf(buffer, "%s %s %s", command, filename, checkpoint_tag);
                printf("[User %s] Requesting CHECKPOINT %s (tag: %s)\n", username, filename, checkpoint_tag);

                // Use HashMap for O(1) lookup
                int file_index = get_file_index(filename);

                if (file_index == -1) {
                    send(client_socket, "404 ERROR: FILE NOT FOUND", 25, 0);
                } else {
                    pthread_mutex_lock(&file_table_mutex);
                    if (!check_permission(&file_table[file_index], username, false)) {
                        pthread_mutex_unlock(&file_table_mutex);
                        send(client_socket, "403 ERROR: PERMISSION DENIED", 28, 0);
                    } else {
                        FileMetadata meta = file_table[file_index];
                        pthread_mutex_unlock(&file_table_mutex);
                        
                        pthread_mutex_lock(&server_list_mutex);
                        StorageServer ss = server_list[meta.ss_index];
                        pthread_mutex_unlock(&server_list_mutex);
                        
                        // Connect to SS and create checkpoint
                        int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
                        struct sockaddr_in ss_addr;
                        memset(&ss_addr, 0, sizeof(ss_addr));
                        ss_addr.sin_family = AF_INET;
                        ss_addr.sin_port = htons(ss.port);
                        inet_pton(AF_INET, ss.ip, &ss_addr.sin_addr);

                        if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) {
                            send(client_socket, "500 ERROR: INTERNAL", 19, 0);
                            close(ss_sock);
                            continue;
                        }
                        
                        char ss_command[BUFFER_SIZE];
                        sprintf(ss_command, "INTERNAL_CHECKPOINT %s %s", meta.path_on_ss, checkpoint_tag);
                        send(ss_sock, ss_command, strlen(ss_command), 0);
                        
                        char ss_reply[BUFFER_SIZE] = {0};
                        recv(ss_sock, ss_reply, BUFFER_SIZE - 1, 0);
                        close(ss_sock);
                        
                        if (strncmp(ss_reply, "200 OK", 6) == 0) {
                            send(client_socket, "200 OK: CHECKPOINT CREATED", 26, 0);
                        } else {
                            send(client_socket, ss_reply, strlen(ss_reply), 0);
                        }
                    }
                }

            } else if (strncmp(buffer, "VIEWCHECKPOINT", 14) == 0) {
                // --- VIEWCHECKPOINT COMMAND ---
                char command[100], filename[256], checkpoint_tag[256];
                sscanf(buffer, "%s %s %s", command, filename, checkpoint_tag);
                printf("[User %s] Requesting VIEWCHECKPOINT %s (tag: %s)\n", username, filename, checkpoint_tag);

                // Use HashMap for O(1) lookup
                int file_index = get_file_index(filename);

                if (file_index == -1) {
                    send(client_socket, "404 ERROR: FILE NOT FOUND", 25, 0);
                } else {
                    pthread_mutex_lock(&file_table_mutex);
                    if (!check_permission(&file_table[file_index], username, false)) {
                        pthread_mutex_unlock(&file_table_mutex);
                        send(client_socket, "403 ERROR: PERMISSION DENIED", 28, 0);
                    } else {
                        FileMetadata meta = file_table[file_index];
                        pthread_mutex_unlock(&file_table_mutex);
                        
                        pthread_mutex_lock(&server_list_mutex);
                        StorageServer ss = server_list[meta.ss_index];
                        pthread_mutex_unlock(&server_list_mutex);
                        
                        // Connect to SS and view checkpoint
                        int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
                        struct sockaddr_in ss_addr;
                        memset(&ss_addr, 0, sizeof(ss_addr));
                        ss_addr.sin_family = AF_INET;
                        ss_addr.sin_port = htons(ss.port);
                        inet_pton(AF_INET, ss.ip, &ss_addr.sin_addr);

                        if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) {
                            send(client_socket, "500 ERROR: INTERNAL", 19, 0);
                            close(ss_sock);
                            continue;
                        }
                        
                        char ss_command[BUFFER_SIZE];
                        sprintf(ss_command, "INTERNAL_VIEWCHECKPOINT %s %s", meta.path_on_ss, checkpoint_tag);
                        send(ss_sock, ss_command, strlen(ss_command), 0);
                        
                        // Forward checkpoint content to client
                        char response[8192] = {0};
                        int total = 0;
                        int bytes;
                        while ((bytes = recv(ss_sock, response + total, sizeof(response) - total - 1, 0)) > 0) {
                            total += bytes;
                        }
                        close(ss_sock);
                        
                        send(client_socket, response, total, 0);
                    }
                }

            } else if (strncmp(buffer, "REVERT", 6) == 0) {
                // --- REVERT COMMAND ---
                char command[100], filename[256], checkpoint_tag[256];
                sscanf(buffer, "%s %s %s", command, filename, checkpoint_tag);
                printf("[User %s] Requesting REVERT %s to %s\n", username, filename, checkpoint_tag);

                pthread_mutex_lock(&file_table_mutex);
                int file_index = -1;
                for (int i = 0; i < file_count; i++) {
                    if (strcmp(file_table[i].name, filename) == 0) {
                        file_index = i;
                        break;
                    }
                }

                if (file_index == -1) {
                    pthread_mutex_unlock(&file_table_mutex);
                    send(client_socket, "404 ERROR: FILE NOT FOUND", 25, 0);
                } else if (!check_permission(&file_table[file_index], username, true)) {
                    pthread_mutex_unlock(&file_table_mutex);
                    send(client_socket, "403 ERROR: PERMISSION DENIED (WRITE)", 36, 0);
                } else {
                    FileMetadata meta = file_table[file_index];
                    pthread_mutex_unlock(&file_table_mutex);
                    
                    pthread_mutex_lock(&server_list_mutex);
                    if (meta.ss_index < 0 || meta.ss_index >= MAX_SERVERS) {
                        pthread_mutex_unlock(&server_list_mutex);
                        send(client_socket, "500 ERROR: Invalid storage server", 34, 0);
                    } else {
                        StorageServer ss = server_list[meta.ss_index];
                        pthread_mutex_unlock(&server_list_mutex);
                        
                        // Connect to SS and revert
                        int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
                        struct sockaddr_in ss_addr;
                        memset(&ss_addr, 0, sizeof(ss_addr));
                        ss_addr.sin_family = AF_INET;
                        ss_addr.sin_port = htons(ss.port);
                        inet_pton(AF_INET, ss.ip, &ss_addr.sin_addr);

                        if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) {
                            perror("REVERT: Failed to connect to SS");
                            send(client_socket, "500 ERROR: Cannot connect to storage server", 44, 0);
                            close(ss_sock);
                        } else {
                            char ss_command[BUFFER_SIZE];
                            sprintf(ss_command, "INTERNAL_REVERT %s %s", meta.path_on_ss, checkpoint_tag);
                            send(ss_sock, ss_command, strlen(ss_command), 0);
                            
                            char ss_reply[BUFFER_SIZE] = {0};
                            recv(ss_sock, ss_reply, BUFFER_SIZE - 1, 0);
                            close(ss_sock);
                            
                            send(client_socket, ss_reply, strlen(ss_reply), 0);
                        }
                    }
                }

            } else if (strncmp(buffer, "LISTCHECKPOINTS", 15) == 0) {
                // --- LISTCHECKPOINTS COMMAND ---
                char command[100], filename[256];
                sscanf(buffer, "%s %s", command, filename);
                printf("[User %s] Requesting LISTCHECKPOINTS %s\n", username, filename);

                pthread_mutex_lock(&file_table_mutex);
                int file_index = -1;
                for (int i = 0; i < file_count; i++) {
                    if (strcmp(file_table[i].name, filename) == 0) {
                        file_index = i;
                        break;
                    }
                }

                if (file_index == -1) {
                    pthread_mutex_unlock(&file_table_mutex);
                    send(client_socket, "404 ERROR: FILE NOT FOUND", 25, 0);
                } else if (!check_permission(&file_table[file_index], username, false)) {
                    pthread_mutex_unlock(&file_table_mutex);
                    send(client_socket, "403 ERROR: PERMISSION DENIED", 28, 0);
                } else {
                    FileMetadata meta = file_table[file_index];
                    pthread_mutex_unlock(&file_table_mutex);
                    
                    pthread_mutex_lock(&server_list_mutex);
                    if (meta.ss_index < 0 || meta.ss_index >= MAX_SERVERS) {
                        pthread_mutex_unlock(&server_list_mutex);
                        send(client_socket, "500 ERROR: Invalid storage server", 34, 0);
                    } else {
                        StorageServer ss = server_list[meta.ss_index];
                        pthread_mutex_unlock(&server_list_mutex);
                        
                        // Connect to SS and list checkpoints
                        int ss_sock = socket(AF_INET, SOCK_STREAM, 0);
                        struct sockaddr_in ss_addr;
                        memset(&ss_addr, 0, sizeof(ss_addr));
                        ss_addr.sin_family = AF_INET;
                        ss_addr.sin_port = htons(ss.port);
                        inet_pton(AF_INET, ss.ip, &ss_addr.sin_addr);

                        if (connect(ss_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) {
                            perror("LISTCHECKPOINTS: Failed to connect to SS");
                            send(client_socket, "500 ERROR: Cannot connect to storage server", 44, 0);
                            close(ss_sock);
                        } else {
                            char ss_command[BUFFER_SIZE];
                            sprintf(ss_command, "INTERNAL_LISTCHECKPOINTS %s", meta.path_on_ss);
                            send(ss_sock, ss_command, strlen(ss_command), 0);
                            
                            char response[4096] = {0};
                            recv(ss_sock, response, sizeof(response) - 1, 0);
                            close(ss_sock);
                            
                            send(client_socket, response, strlen(response), 0);
                        }
                    }
                }

            } else if (strncmp(buffer, "REQUEST", 7) == 0) {
                // --- REQUEST ACCESS COMMAND ---
                char command[100], flag[10], filename[256];
                if (sscanf(buffer, "%s %s %s", command, flag, filename) != 3) {
                    send(client_socket, "400 ERROR: Usage: REQUEST -R/-W <filename>", 43, 0);
                    continue;
                }
                
                bool wants_write = (strcmp(flag, "-W") == 0);
                printf("[User %s] Requesting %s access to %s\n", username, wants_write ? "R/W" : "R", filename);

                pthread_mutex_lock(&file_table_mutex);
                int file_index = -1;
                for (int i = 0; i < file_count; i++) {
                    if (strcmp(file_table[i].name, filename) == 0) {
                        file_index = i;
                        break;
                    }
                }

                if (file_index == -1) {
                    pthread_mutex_unlock(&file_table_mutex);
                    send(client_socket, "404 ERROR: FILE NOT FOUND", 25, 0);
                } else if (strcmp(file_table[file_index].owner, username) == 0) {
                    pthread_mutex_unlock(&file_table_mutex);
                    send(client_socket, "400 ERROR: You already own this file", 37, 0);
                } else if (check_permission(&file_table[file_index], username, wants_write)) {
                    pthread_mutex_unlock(&file_table_mutex);
                    send(client_socket, "400 ERROR: You already have this access", 40, 0);
                } else {
                    char owner[100];
                    strcpy(owner, file_table[file_index].owner);
                    pthread_mutex_unlock(&file_table_mutex);
                    
                    // Add to access request queue
                    pthread_mutex_lock(&access_request_mutex);
                    
                    // Check if request already exists
                    bool exists = false;
                    for (int i = 0; i < access_request_count; i++) {
                        if (strcmp(access_requests[i].filename, filename) == 0 &&
                            strcmp(access_requests[i].requester, username) == 0) {
                            // Update existing request
                            access_requests[i].wants_write = wants_write;
                            access_requests[i].request_time = time(NULL);
                            exists = true;
                            break;
                        }
                    }
                    
                    if (!exists && access_request_count < MAX_ACCESS_REQUESTS) {
                        strcpy(access_requests[access_request_count].filename, filename);
                        strcpy(access_requests[access_request_count].requester, username);
                        access_requests[access_request_count].wants_write = wants_write;
                        access_requests[access_request_count].request_time = time(NULL);
                        access_request_count++;
                    }
                    
                    pthread_mutex_unlock(&access_request_mutex);
                    
                    // NOTIFY THE OWNER
                    char notif_msg[512];
                    snprintf(notif_msg, sizeof(notif_msg),
                            "User '%s' is requesting %s access to your file '%s'",
                            username, wants_write ? "WRITE" : "READ", filename);
                    add_notification(NOTIF_ACCESS_REQUEST, owner, username, filename, notif_msg);
                    
                    send(client_socket, "200 OK: REQUEST SUBMITTED", 25, 0);
                    
                    // Check if owner is currently online and send notification to requester
                    int owner_online = 0;
                    pthread_mutex_lock(&active_users_mutex);
                    for (int i = 0; i < active_user_count; i++) {
                        if (strcmp(active_users[i], owner) == 0) {
                            owner_online = 1;
                            break;
                        }
                    }
                    pthread_mutex_unlock(&active_users_mutex);
                    
                    if (owner_online) {
                        char info_msg[256];
                        snprintf(info_msg, sizeof(info_msg), 
                                "\n[INFO] Owner '%s' is online and has been notified of your request.\n", owner);
                        send(client_socket, info_msg, strlen(info_msg), 0);
                    } else {
                        char info_msg[256];
                        snprintf(info_msg, sizeof(info_msg),
                                "\n[INFO] Owner '%s' is offline. They will be notified when they log in.\n", owner);
                        send(client_socket, info_msg, strlen(info_msg), 0);
                    }
                }

            } else if (strncmp(buffer, "VIEWREQUESTS", 12) == 0) {
                // --- VIEWREQUESTS COMMAND ---
                printf("[User %s] Requesting VIEWREQUESTS\n", username);

                char response[8192] = "Pending Access Requests:\n";
                int count = 0;
                
                pthread_mutex_lock(&access_request_mutex);
                pthread_mutex_lock(&file_table_mutex);
                
                for (int i = 0; i < access_request_count; i++) {
                    // Find the file to check ownership
                    int file_idx = -1;
                    for (int j = 0; j < file_count; j++) {
                        if (strcmp(file_table[j].name, access_requests[i].filename) == 0) {
                            file_idx = j;
                            break;
                        }
                    }
                    
                    if (file_idx != -1 && strcmp(file_table[file_idx].owner, username) == 0) {
                        char line[512];
                        sprintf(line, "[%d] %s wants %s access to %s\n", 
                               i, 
                               access_requests[i].requester,
                               access_requests[i].wants_write ? "R/W" : "R",
                               access_requests[i].filename);
                        strcat(response, line);
                        count++;
                    }
                }
                
                pthread_mutex_unlock(&file_table_mutex);
                pthread_mutex_unlock(&access_request_mutex);
                
                if (count == 0) {
                    strcpy(response, "No pending requests for your files.\n");
                }
                
                send(client_socket, response, strlen(response), 0);

            } else if (strncmp(buffer, "APPROVE", 7) == 0) {
                // --- APPROVE REQUEST COMMAND ---
                char command[100];
                int request_id;
                if (sscanf(buffer, "%s %d", command, &request_id) != 2) {
                    send(client_socket, "400 ERROR: Usage: APPROVE <request_id>", 39, 0);
                    continue;
                }
                
                printf("[User %s] Approving request %d\n", username, request_id);

                pthread_mutex_lock(&access_request_mutex);
                
                if (request_id < 0 || request_id >= access_request_count) {
                    pthread_mutex_unlock(&access_request_mutex);
                    send(client_socket, "404 ERROR: Invalid request ID", 30, 0);
                } else {
                    AccessRequest req = access_requests[request_id];
                    
                    pthread_mutex_lock(&file_table_mutex);
                    int file_idx = -1;
                    for (int i = 0; i < file_count; i++) {
                        if (strcmp(file_table[i].name, req.filename) == 0) {
                            file_idx = i;
                            break;
                        }
                    }
                    
                    if (file_idx == -1) {
                        pthread_mutex_unlock(&file_table_mutex);
                        pthread_mutex_unlock(&access_request_mutex);
                        send(client_socket, "404 ERROR: File no longer exists", 33, 0);
                    } else if (strcmp(file_table[file_idx].owner, username) != 0) {
                        pthread_mutex_unlock(&file_table_mutex);
                        pthread_mutex_unlock(&access_request_mutex);
                        send(client_socket, "403 ERROR: You don't own this file", 35, 0);
                    } else {
                        // Grant access by adding to ACL
                        int acl_idx = file_table[file_idx].acl_count;
                        
                        // Check if user already in ACL (update)
                        bool updated = false;
                        for (int i = 0; i < file_table[file_idx].acl_count; i++) {
                            if (strcmp(file_table[file_idx].acl[i].username, req.requester) == 0) {
                                file_table[file_idx].acl[i].can_write = req.wants_write;
                                updated = true;
                                break;
                            }
                        }
                        
                        if (!updated && acl_idx < MAX_ACL_USERS) {
                            strcpy(file_table[file_idx].acl[acl_idx].username, req.requester);
                            file_table[file_idx].acl[acl_idx].can_write = req.wants_write;
                            file_table[file_idx].acl_count++;
                        }
                        
                        // Remove request
                        access_requests[request_id] = access_requests[access_request_count - 1];
                        access_request_count--;
                        
                        pthread_mutex_unlock(&file_table_mutex);
                        pthread_mutex_unlock(&access_request_mutex);
                        
                        // NOTIFY THE REQUESTER
                        char notif_msg[512];
                        snprintf(notif_msg, sizeof(notif_msg),
                                "Your request for %s access to '%s' has been GRANTED by the owner",
                                req.wants_write ? "WRITE" : "READ", req.filename);
                        add_notification(NOTIF_ACCESS_GRANTED, req.requester, username, req.filename, notif_msg);
                        
                        save_metadata();
                        send(client_socket, "200 OK: ACCESS GRANTED", 22, 0);
                        
                        // Send immediate notification to requester
                        char immediate_notif[768];
                        snprintf(immediate_notif, sizeof(immediate_notif),
                                "\n[NOTIFICATION] %s for '%s' has been GRANTED!\n",
                                req.wants_write ? "WRITE access" : "READ access", req.filename);
                        send(client_socket, immediate_notif, strlen(immediate_notif), 0);
                    }
                }

            } else if (strncmp(buffer, "DENY", 4) == 0 || strncmp(buffer, "REJECT", 6) == 0) {
                // --- DENY/REJECT REQUEST COMMAND ---
                char command[100];
                int request_id;
                if (sscanf(buffer, "%s %d", command, &request_id) != 2) {
                    send(client_socket, "400 ERROR: Usage: DENY/REJECT <request_id>", 43, 0);
                    continue;
                }
                
                printf("[User %s] Denying request %d\n", username, request_id);

                pthread_mutex_lock(&access_request_mutex);
                
                if (request_id < 0 || request_id >= access_request_count) {
                    pthread_mutex_unlock(&access_request_mutex);
                    send(client_socket, "404 ERROR: Invalid request ID", 30, 0);
                } else {
                    AccessRequest req = access_requests[request_id];
                    
                    pthread_mutex_lock(&file_table_mutex);
                    int file_idx = -1;
                    for (int i = 0; i < file_count; i++) {
                        if (strcmp(file_table[i].name, req.filename) == 0) {
                            file_idx = i;
                            break;
                        }
                    }
                    
                    if (file_idx == -1) {
                        pthread_mutex_unlock(&file_table_mutex);
                        pthread_mutex_unlock(&access_request_mutex);
                        send(client_socket, "404 ERROR: File no longer exists", 33, 0);
                    } else if (strcmp(file_table[file_idx].owner, username) != 0) {
                        pthread_mutex_unlock(&file_table_mutex);
                        pthread_mutex_unlock(&access_request_mutex);
                        send(client_socket, "403 ERROR: You don't own this file", 35, 0);
                    } else {
                        // Remove request from queue
                        access_requests[request_id] = access_requests[access_request_count - 1];
                        access_request_count--;
                        
                        pthread_mutex_unlock(&file_table_mutex);
                        pthread_mutex_unlock(&access_request_mutex);
                        
                        // NOTIFY THE REQUESTER that their request was denied
                        char notif_msg[512];
                        snprintf(notif_msg, sizeof(notif_msg),
                                "Your request for %s access to '%s' has been DENIED by the owner",
                                req.wants_write ? "WRITE" : "READ", req.filename);
                        add_notification(NOTIF_ACCESS_GRANTED, req.requester, username, req.filename, notif_msg);
                        
                        send(client_socket, "200 OK: REQUEST DENIED", 22, 0);
                        
                        // Send immediate notification to owner
                        char immediate_notif[768];
                        snprintf(immediate_notif, sizeof(immediate_notif),
                                "\n[INFO] Access request from '%s' for '%s' has been REJECTED.\n",
                                req.requester, req.filename);
                        send(client_socket, immediate_notif, strlen(immediate_notif), 0);
                    }
                }

            } else if (strncmp(buffer, "SEARCH", 6) == 0) {
                // --- SEARCH COMMAND: Full-text search across all indexed files ---
                char search_word[100];
                if (sscanf(buffer, "SEARCH %99s", search_word) != 1) {
                    send(client_socket, "400 ERROR: Usage: SEARCH <word>", 32, 0);
                } else {
                    printf("[User %s] Searching for word: %s\n", username, search_word);
                    
                    char result_buffer[8192];
                    search_word_in_index(search_word, result_buffer, sizeof(result_buffer));
                    
                    send(client_socket, result_buffer, strlen(result_buffer), 0);
                }

            } else if (strncmp(buffer, "INDEX", 5) == 0) {
                // --- INDEX COMMAND: Index file content for search ---
                // Format: INDEX <filename> <content>
                char filename[256];
                char* content_start = NULL;
                
                // Parse: "INDEX <filename> <rest of line is content>"
                if (sscanf(buffer, "INDEX %255s", filename) == 1) {
                    // Find where content starts (after filename)
                    content_start = buffer + 6; // Skip "INDEX "
                    while (*content_start == ' ') content_start++;
                    while (*content_start && *content_start != ' ') content_start++;
                    while (*content_start == ' ') content_start++;
                    
                    if (*content_start) {
                        printf("[User %s] Indexing file: %s\n", username, filename);
                        index_file_content(filename, content_start);
                        send(client_socket, "200 OK: File indexed for search", 32, 0);
                    } else {
                        send(client_socket, "400 ERROR: No content to index", 31, 0);
                    }
                } else {
                    send(client_socket, "400 ERROR: Usage: INDEX <filename> <content>", 45, 0);
                }

            } else if (strncmp(buffer, "NOTIFICATIONS", 13) == 0) {
                // --- NOTIFICATIONS COMMAND: View all notifications ---
                printf("[User %s] Requesting NOTIFICATIONS\n", username);
                send_pending_notifications(client_socket, username);
                send(client_socket, "200 OK: Notifications displayed", 32, 0);

            } else {
                // Unknown command
                char reply_buffer[BUFFER_SIZE];
                sprintf(reply_buffer, "400 ERROR: UNKNOWN CMD: %s", buffer);
                send(client_socket, reply_buffer, strlen(reply_buffer), 0);
            }
            // --- END COMMAND PARSING ---
        }
    } else {
        // --- UNKNOWN TYPE ---
        printf("[Socket %d] Unknown connection type. Booting.\n", client_socket);
        send(client_socket, "400 ERROR: BAD REQUEST", 22, 0);
    }
    
    // --- CLEANUP: Remove user from active users list ---
    pthread_mutex_lock(&active_users_mutex);
    for (int i = 0; i < active_user_count; i++) {
        if (strcmp(active_users[i], username) == 0) {
            // Remove by swapping with last element
            strcpy(active_users[i], active_users[active_user_count - 1]);
            active_user_count--;
            printf("[Socket %d] User '%s' removed from active users list\n", client_socket, username);
            log_event(LOG_INFO, "N/A", 0, username, "User disconnected");
            break;
        }
    }
    pthread_mutex_unlock(&active_users_mutex);
    
    // Remove from active_clients (for push notifications)
    pthread_mutex_lock(&active_clients_mutex);
    for (int i = 0; i < active_client_count; i++) {
        if (strcmp(active_clients[i].username, username) == 0) {
            // Remove by swapping with last element
            active_clients[i] = active_clients[active_client_count - 1];
            active_client_count--;
            printf("[Socket %d] User '%s' removed from active clients list\n", client_socket, username);
            break;
        }
    }
    pthread_mutex_unlock(&active_clients_mutex);
    
    printf("[Socket %d] Session ended. Thread exiting.\n", client_socket);
    close(client_socket);
    return NULL;
}

// --- REPLICATION: Async replicate file to backup SS ---
void* async_replicate_file(void* arg) {
    typedef struct {
        int backup_ss_index;
        char path_on_ss[512];
        char primary_ss_ip[100];
        int primary_ss_port;
    } ReplicateArgs;
    
    ReplicateArgs* args = (ReplicateArgs*)arg;
    
    pthread_mutex_lock(&server_list_mutex);
    if (server_list[args->backup_ss_index].status != SS_ONLINE) {
        pthread_mutex_unlock(&server_list_mutex);
        free(args);
        return NULL;
    }
    StorageServer backup_ss = server_list[args->backup_ss_index];
    pthread_mutex_unlock(&server_list_mutex);
    
    // Connect to backup SS
    int backup_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in backup_addr;
    memset(&backup_addr, 0, sizeof(backup_addr));
    backup_addr.sin_family = AF_INET;
    backup_addr.sin_port = htons(backup_ss.port);
    inet_pton(AF_INET, backup_ss.ip, &backup_addr.sin_addr);
    
    if (connect(backup_sock, (struct sockaddr *)&backup_addr, sizeof(backup_addr)) < 0) {
        printf("[REPLICATION] Failed to connect to backup SS %d\n", args->backup_ss_index);
        close(backup_sock);
        free(args);
        return NULL;
    }
    
    // Send INTERNAL_REPLICATE command
    char command[1024];
    sprintf(command, "INTERNAL_REPLICATE %s %s:%d", args->path_on_ss, args->primary_ss_ip, args->primary_ss_port);
    send(backup_sock, command, strlen(command), 0);
    
    char reply[256] = {0};
    recv(backup_sock, reply, sizeof(reply) - 1, 0);
    close(backup_sock);
    
    if (strncmp(reply, "200 OK", 6) == 0) {
        printf("[REPLICATION] Successfully replicated %s to SS %d\n", args->path_on_ss, args->backup_ss_index);
    } else {
        printf("[REPLICATION] Failed to replicate %s to SS %d: %s\n", args->path_on_ss, args->backup_ss_index, reply);
    }
    
    free(args);
    return NULL;
}

// --- Helper: Test if Storage Server is reachable ---
int test_ss_connection(const char* ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return 0; // Can't create socket
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    
    // Set short timeout for quick check
    struct timeval timeout;
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    int result = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    close(sock);
    
    return (result == 0); // Returns 1 if connected successfully, 0 if failed
}

// --- SYNC: Replicate all missing files to recovered SS ---
void sync_files_to_recovered_ss(int recovered_ss_index) {
    printf("[SYNC] Starting sync for recovered SS %d\n", recovered_ss_index);
    
    pthread_mutex_lock(&file_table_mutex);
    pthread_mutex_lock(&server_list_mutex);
    
    StorageServer recovered_ss = server_list[recovered_ss_index];
    int sync_count = 0;
    
    // Check all files to see which ones should be on this SS
    for (int i = 0; i < file_count; i++) {
        FileMetadata* file = &file_table[i];
        bool should_have_file = false;
        int source_ss_index = -1;
        
        // Check if recovered SS should have this file (as primary or replica)
        if (file->ss_index == recovered_ss_index) {
            should_have_file = true;
            // Find a replica to copy from
            for (int r = 0; r < file->replica_count; r++) {
                int replica_idx = file->replica_ss_indices[r];
                if (server_list[replica_idx].status == SS_ONLINE) {
                    source_ss_index = replica_idx;
                    break;
                }
            }
        } else {
            // Check if it's a replica
            for (int r = 0; r < file->replica_count; r++) {
                if (file->replica_ss_indices[r] == recovered_ss_index) {
                    should_have_file = true;
                    // Copy from primary if online, otherwise from other replica
                    if (server_list[file->ss_index].status == SS_ONLINE) {
                        source_ss_index = file->ss_index;
                    } else {
                        // Find another replica
                        for (int r2 = 0; r2 < file->replica_count; r2++) {
                            int other_replica = file->replica_ss_indices[r2];
                            if (other_replica != recovered_ss_index && 
                                server_list[other_replica].status == SS_ONLINE) {
                                source_ss_index = other_replica;
                                break;
                            }
                        }
                    }
                    break;
                }
            }
        }
        
        // If this SS should have the file and we found a source, replicate it
        if (should_have_file && source_ss_index != -1) {
            StorageServer source_ss = server_list[source_ss_index];
            
            printf("[SYNC] Syncing %s from SS %d to recovered SS %d\n", 
                   file->path_on_ss, source_ss_index, recovered_ss_index);
            
            // Connect to recovered SS and send INTERNAL_REPLICATE command
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) continue;
            
            struct sockaddr_in ss_addr;
            memset(&ss_addr, 0, sizeof(ss_addr));
            ss_addr.sin_family = AF_INET;
            ss_addr.sin_port = htons(recovered_ss.port);
            inet_pton(AF_INET, recovered_ss.ip, &ss_addr.sin_addr);
            
            if (connect(sock, (struct sockaddr*)&ss_addr, sizeof(ss_addr)) == 0) {
                // Extract filename from path
                char* filename_only = strrchr(file->path_on_ss, '/');
                if (filename_only) filename_only++;
                else filename_only = file->path_on_ss;
                
                // Construct source path for the file
                char source_path[512];
                snprintf(source_path, sizeof(source_path), "data_ss_%d/%s",
                        9000 + source_ss_index + 1, filename_only);
                
                // Send INTERNAL_REPLICATE command (format: INTERNAL_REPLICATE <source_path> <ip:port>)
                char cmd[1024];
                snprintf(cmd, sizeof(cmd), "INTERNAL_REPLICATE %s %s:%d",
                        source_path, source_ss.ip, source_ss.port);
                send(sock, cmd, strlen(cmd), 0);
                
                // Wait for response
                char response[256];
                int n = recv(sock, response, sizeof(response) - 1, 0);
                if (n > 0) {
                    response[n] = '\0';
                    printf("[SYNC] Response: %s\n", response);
                    sync_count++;
                }
                close(sock);
            } else {
                close(sock);
            }
        }
    }
    
    pthread_mutex_unlock(&server_list_mutex);
    pthread_mutex_unlock(&file_table_mutex);
    
    printf("[SYNC] Initiated sync of %d files to recovered SS %d\n", sync_count, recovered_ss_index);
}

// --- HEARTBEAT: Monitor SS health ---
void* heartbeat_monitor(void* arg) {
    (void)arg;
    
    while (1) {
        sleep(10); // Check every 10 seconds
        
        pthread_mutex_lock(&server_list_mutex);
        for (int i = 0; i < server_count; i++) {
            if (server_list[i].status == SS_FAILED) continue;
            
            // Try to ping the SS
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(server_list[i].port);
            inet_pton(AF_INET, server_list[i].ip, &addr.sin_addr);
            
            // Set timeout for connection attempt
            struct timeval timeout;
            timeout.tv_sec = 3;
            timeout.tv_usec = 0;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            
            if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                // Connection failed - mark as FAILED
                if (server_list[i].status == SS_ONLINE) {
                    printf("[HEARTBEAT] Storage Server %d (%s:%d) FAILED\n", 
                           i, server_list[i].ip, server_list[i].port);
                    server_list[i].status = SS_FAILED;
                }
            } else {
                // Send PING
                send(sock, "PING", 4, 0);
                char reply[16] = {0};
                int bytes = recv(sock, reply, sizeof(reply) - 1, 0);
                
                if (bytes > 0 && strncmp(reply, "PONG", 4) == 0) {
                    server_list[i].last_heartbeat = time(NULL);
                    if (server_list[i].status == SS_FAILED) {
                        printf("[HEARTBEAT] Storage Server %d (%s:%d) RECOVERED\n", 
                               i, server_list[i].ip, server_list[i].port);
                        server_list[i].status = SS_ONLINE;
                        
                        // Unlock before triggering sync (to avoid deadlock)
                        pthread_mutex_unlock(&server_list_mutex);
                        
                        // Trigger sync for recovered SS
                        sync_files_to_recovered_ss(i);
                        
                        // Re-lock for next iteration
                        pthread_mutex_lock(&server_list_mutex);
                    }
                } else {
                    if (server_list[i].status == SS_ONLINE) {
                        printf("[HEARTBEAT] Storage Server %d (%s:%d) FAILED (no response)\n", 
                               i, server_list[i].ip, server_list[i].port);
                        server_list[i].status = SS_FAILED;
                    }
                }
            }
            close(sock);
        }
        pthread_mutex_unlock(&server_list_mutex);
    }
    
    return NULL;
}

// --- Helper: Pick N different backup SSs for replication ---
void pick_backup_servers(int primary_index, int* backup_indices, int* backup_count) {
    *backup_count = 0;
    
    pthread_mutex_lock(&server_list_mutex);
    
    // Count TOTAL files (primary + replicas) on each server for better load balancing
    int total_files[MAX_SERVERS] = {0};
    
    pthread_mutex_lock(&file_table_mutex);
    for (int i = 0; i < file_count; i++) {
        // Count primary file
        total_files[file_table[i].ss_index]++;
        
        // Count replicas
        for (int j = 0; j < file_table[i].replica_count; j++) {
            total_files[file_table[i].replica_ss_indices[j]]++;
        }
    }
    pthread_mutex_unlock(&file_table_mutex);
    
    printf("[REPLICATION] Total files per SS: ");
    for (int i = 0; i < server_count; i++) {
        printf("SS%d=%d ", i, total_files[i]);
    }
    printf("\n");
    
    // Pick MAX_REPLICAS servers with lowest total file count (excluding primary)
    while (*backup_count < MAX_REPLICAS) {
        int min_files = INT_MAX;
        int best_ss = -1;
        
        // Find server with minimum total files (excluding primary and already selected)
        for (int i = 0; i < server_count; i++) {
            if (i == primary_index) continue; // Skip primary
            if (server_list[i].status != SS_ONLINE) continue; // Skip offline
            
            // Check if already selected as backup
            bool already_selected = false;
            for (int j = 0; j < *backup_count; j++) {
                if (backup_indices[j] == i) {
                    already_selected = true;
                    break;
                }
            }
            if (already_selected) continue;
            
            // Check if this server has fewer files
            if (total_files[i] < min_files) {
                min_files = total_files[i];
                best_ss = i;
            }
        }
        
        // If found a suitable server, add it
        if (best_ss != -1) {
            backup_indices[*backup_count] = best_ss;
            (*backup_count)++;
            total_files[best_ss]++; // Increment for next iteration
            printf("[REPLICATION] Selected SS %d (port %d) as backup #%d (has %d total files)\n",
                   best_ss, server_list[best_ss].port, *backup_count, min_files);
        } else {
            break; // No more available servers
        }
    }
    
    pthread_mutex_unlock(&server_list_mutex);
    
    printf("[REPLICATION] Picked %d backup servers for primary SS %d\n", *backup_count, primary_index);
}


// --- Main Function ---
int main(void) {
    // Initialize hashmap and cache for O(1) file lookups
    hashmap_init(&file_hashmap);
    cache_init(&file_cache);
    printf("HashMap and LRU Cache initialized for efficient search\n");
    
    // Initialize logging system
    init_logger("nameserver.log");
    log_message(LOG_INFO, "=== Name Server Starting ===");
    
    // Load existing metadata from disk
    load_metadata();
    log_message(LOG_INFO, "Loaded metadata from disk");
    
    // Rebuild hashmap from loaded files
    pthread_mutex_lock(&file_table_mutex);
    for (int i = 0; i < file_count; i++) {
        hashmap_put(&file_hashmap, file_table[i].name, i);
    }
    pthread_mutex_unlock(&file_table_mutex);
    printf("Rebuilt hashmap with %d files\n", file_count);
    
    // Start heartbeat monitor thread
    pthread_t heartbeat_thread;
    if (pthread_create(&heartbeat_thread, NULL, heartbeat_monitor, NULL) != 0) {
        perror("Failed to create heartbeat thread");
        log_message(LOG_ERROR, "Failed to create heartbeat thread");
    } else {
        pthread_detach(heartbeat_thread);
        printf("Heartbeat monitor started\n");
        log_message(LOG_INFO, "Heartbeat monitor started");
    }
    
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket failed"); exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("setsockopt failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        log_message(LOG_ERROR, "Failed to bind to port %d", PORT);
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    if (listen(server_fd, 10) < 0) {
        perror("listen failed");
        log_message(LOG_ERROR, "Failed to listen on port %d", PORT);
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Name Server listening on port %d...\n", PORT);
    log_message(LOG_INFO, "Name Server listening on port %d", PORT);
    fflush(stdout);

    // Main server loop
    while (1) {
        int client_socket = accept(server_fd, NULL, NULL);
        if (client_socket < 0) {
            perror("accept failed");
            continue;
        }

        printf("Main loop: New connection accepted. Handing off to thread.\n");
        fflush(stdout);

        pthread_t thread_id;
        int* p_client_socket = malloc(sizeof(int));
        *p_client_socket = client_socket;

        if (pthread_create(&thread_id, NULL, handle_client, p_client_socket) != 0) {
            perror("pthread_create failed");
            close(client_socket);
            free(p_client_socket);
        }

        pthread_detach(thread_id);
    }

    close(server_fd);
    return 0;
}