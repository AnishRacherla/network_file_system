#ifndef BONUS_FEATURES_H
#define BONUS_FEATURES_H

#include <stdbool.h>
#include <time.h>
#include <pthread.h>

// ============================================
// BONUS FEATURE 1: HIERARCHICAL FOLDERS
// ============================================

#define MAX_PATH_LENGTH 1024

// ============================================
// BONUS FEATURE 2: CHECKPOINTS
// ============================================

#define MAX_CHECKPOINTS 20
#define MAX_TAG_LENGTH 100

typedef struct {
    char tag[MAX_TAG_LENGTH];
    time_t created_at;
} Checkpoint;

// ============================================
// BONUS FEATURE 3: ACCESS REQUESTS
// ============================================

typedef struct {
    int request_id;
    char filename[MAX_PATH_LENGTH];
    char requesting_user[100];
    bool for_write;
    time_t requested_at;
} AccessRequest;

#define MAX_REQUESTS 1000

// ============================================
// BONUS FEATURE 4: FAULT TOLERANCE (REPLICATION)
// ============================================

typedef struct {
    int ss_index;
    char path_on_ss[512];
} ReplicaLocation;

#define MAX_REPLICAS 2  // Primary + 1 backup

// Enhanced Storage Server info
typedef struct {
    char ip[100];
    int port;
    bool is_online;
    time_t last_heartbeat;
    int connection_fd;  // For heartbeat monitoring
} StorageServerInfo;

// ============================================
// ENHANCED FILE METADATA
// ============================================

#define MAX_ACL_USERS 20

typedef struct {
    char username[100];
    bool can_write;
} AclEntry;

typedef struct {
    // BONUS 1: Hierarchical paths
    char full_path[MAX_PATH_LENGTH];  // e.g., "/public/reports/file.txt"
    bool is_folder;
    
    // Original fields
    char owner[100];
    
    // BONUS 2: Checkpoints
    Checkpoint checkpoints[MAX_CHECKPOINTS];
    int checkpoint_count;
    
    // BONUS 4: Replication
    ReplicaLocation replicas[MAX_REPLICAS];
    int replica_count;
    
    // Metadata
    int char_count;
    int word_count;
    time_t last_access;
    
    // Access Control
    AclEntry acl[MAX_ACL_USERS];
    int acl_count;
} EnhancedFileMetadata;

// ============================================
// FUNCTION DECLARATIONS
// ============================================

// Folder operations
bool is_path_in_folder(const char* path, const char* folder);
void normalize_path(char* path);
char* get_parent_folder(const char* path);
char* get_filename_from_path(const char* path);

// Checkpoint operations
bool add_checkpoint(EnhancedFileMetadata* file, const char* tag);
Checkpoint* find_checkpoint(EnhancedFileMetadata* file, const char* tag);
bool remove_checkpoint(EnhancedFileMetadata* file, const char* tag);

// Access request operations
int add_access_request(const char* filename, const char* user, bool for_write);
AccessRequest* get_requests_for_owner(const char* owner, int* count);
bool approve_request(int request_id);
bool deny_request(int request_id);

// Replication operations
bool add_replica(EnhancedFileMetadata* file, int ss_index, const char* path);
ReplicaLocation* get_online_replica(EnhancedFileMetadata* file);
int get_all_online_replicas(EnhancedFileMetadata* file, ReplicaLocation* replicas, int max);

#endif // BONUS_FEATURES_H
