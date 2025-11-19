#include "locks.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Define the global table
LockHashTableBucket global_lock_table[HASH_TABLE_SIZE];

// Simple "djb2" hash function
unsigned long hash_function(const char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    return hash % HASH_TABLE_SIZE;
}

// Initialize all mutexes in the table
void init_lock_table() {
    for (int i = 0; i < HASH_TABLE_SIZE; i++) {
        global_lock_table[i].head = NULL;
        pthread_mutex_init(&global_lock_table[i].lock, NULL);
    }
}

// Tries to acquire a lock.
// Returns 0 on success.
// Returns 1 if already locked by someone else.
// Returns -1 on memory error.
int acquire_lock(const char* path, int sentence_num, const char* username) {
    unsigned long index = hash_function(path);
    
    // Lock this bucket (thread-safe)
    pthread_mutex_lock(&global_lock_table[index].lock);

    // 1. Find the file node in this bucket's chain
    FileLockNode* current_file = global_lock_table[index].head;
    FileLockNode* file_node = NULL;

    while (current_file != NULL) {
        if (strcmp(current_file->path, path) == 0) {
            file_node = current_file;
            break;
        }
        current_file = current_file->next;
    }

    // 2. If file node doesn't exist, create it
    if (file_node == NULL) {
        file_node = (FileLockNode*)malloc(sizeof(FileLockNode));
        if (file_node == NULL) {
            pthread_mutex_unlock(&global_lock_table[index].lock);
            return -1; // Malloc error
        }
        strcpy(file_node->path, path);
        file_node->head_lock = NULL;
        file_node->next = global_lock_table[index].head;
        global_lock_table[index].head = file_node;
    }

    // 3. Now we have file_node. Check its locks.
    LockedSentence* current_lock = file_node->head_lock;
    while (current_lock != NULL) {
        if (current_lock->sentence_num == sentence_num) {
            // LOCK FOUND!
            if (strcmp(current_lock->username, username) == 0) {
                 // It's locked... by us! That's fine.
            } else {
                // It's locked by someone else.
                pthread_mutex_unlock(&global_lock_table[index].lock);
                return 1; // "Already Locked"
            }
        }
        current_lock = current_lock->next;
    }

    // 4. Sentence is NOT locked. Create a new lock.
    LockedSentence* new_lock = (LockedSentence*)malloc(sizeof(LockedSentence));
    if (new_lock == NULL) {
        pthread_mutex_unlock(&global_lock_table[index].lock);
        return -1; // Malloc error
    }
    new_lock->sentence_num = sentence_num;
    strcpy(new_lock->username, username);
    
    // Add it to the front of the file's lock list
    new_lock->next = file_node->head_lock;
    file_node->head_lock = new_lock;

    pthread_mutex_unlock(&global_lock_table[index].lock);
    return 0; // Success
}

// Releases a specific lock
void release_lock(const char* path, int sentence_num) {
    unsigned long index = hash_function(path);
    pthread_mutex_lock(&global_lock_table[index].lock);

    // 1. Find the file
    FileLockNode* file_node = global_lock_table[index].head;
    if (file_node == NULL) {
        pthread_mutex_unlock(&global_lock_table[index].lock);
        return;
    }
    // (We'll assume the file is found)

    // 2. Find the lock in the file's list
    LockedSentence* current_lock = file_node->head_lock;
    LockedSentence* prev_lock = NULL;

    while (current_lock != NULL) {
        if (current_lock->sentence_num == sentence_num) {
            // Found it! Now remove it.
            if (prev_lock == NULL) {
                file_node->head_lock = current_lock->next;
            } else {
                prev_lock->next = current_lock->next;
            }
            free(current_lock); // Free the memory
            break; 
        }
        prev_lock = current_lock;
        current_lock = current_lock->next;
    }
    
    pthread_mutex_unlock(&global_lock_table[index].lock);
}

// Check if a file has any active locks
// Returns 1 if file has active locks, 0 if no locks
int has_active_locks(const char* path) {
    unsigned long index = hash_function(path);
    pthread_mutex_lock(&global_lock_table[index].lock);

    // Find the file node in this bucket's chain
    FileLockNode* current_file = global_lock_table[index].head;
    while (current_file != NULL) {
        if (strcmp(current_file->path, path) == 0) {
            // Found the file - check if it has any locks
            int has_locks = (current_file->head_lock != NULL) ? 1 : 0;
            pthread_mutex_unlock(&global_lock_table[index].lock);
            return has_locks;
        }
        current_file = current_file->next;
    }

    // File not found in lock table - no locks
    pthread_mutex_unlock(&global_lock_table[index].lock);
    return 0;
}