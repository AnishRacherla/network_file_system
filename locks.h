#ifndef LOCKS_H
#define LOCKS_H

#include <pthread.h>

// This struct holds one specific sentence lock
typedef struct LockedSentence {
    int sentence_num;
    char username[100];         // Who locked it
    struct LockedSentence* next; // Next lock *for this same file*
} LockedSentence;

// This struct holds all locks for a single file
typedef struct FileLockNode {
    char path[512];             // The file's unique path (e.g., "data/file_abc")
    LockedSentence* head_lock;  // The head of a linked list of locks
    struct FileLockNode* next;  // Next file *in the same hash bucket*
} FileLockNode;

// --- Define the Global Table ---
#define HASH_TABLE_SIZE 1024 // Power of 2 is good.

// This is the core of our system.
// It's an array of "buckets". Each bucket has:
// 1. A pointer to the first file in its chain (head)
// 2. A MUTEX to protect this one chain.
typedef struct {
    FileLockNode* head;
    pthread_mutex_t lock;
} LockHashTableBucket;

// The global table itself
extern LockHashTableBucket global_lock_table[HASH_TABLE_SIZE];

// --- Function Declarations ---
void init_lock_table();
int acquire_lock(const char* path, int sentence_num, const char* username);
void release_lock(const char* path, int sentence_num);
int has_active_locks(const char* path); // Check if file has any active locks

#endif // LOCKS_H