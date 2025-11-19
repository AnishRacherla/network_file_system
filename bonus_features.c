#include "bonus_features.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ============================================
// GLOBAL STATE FOR ACCESS REQUESTS
// ============================================

AccessRequest pending_requests[MAX_REQUESTS];
int request_count = 0;
int next_request_id = 1;
pthread_mutex_t request_mutex = PTHREAD_MUTEX_INITIALIZER;

// ============================================
// BONUS 1: FOLDER OPERATIONS
// ============================================

// Normalize path (remove trailing slashes, handle //)
void normalize_path(char* path) {
    if (path == NULL || strlen(path) == 0) return;
    
    // Ensure path starts with /
    if (path[0] != '/') {
        char temp[MAX_PATH_LENGTH];
        snprintf(temp, MAX_PATH_LENGTH, "/%s", path);
        strcpy(path, temp);
    }
    
    // Remove trailing slash (except for root "/")
    int len = strlen(path);
    if (len > 1 && path[len-1] == '/') {
        path[len-1] = '\0';
    }
}

// Check if a path is inside a folder
// e.g., is_path_in_folder("/public/file.txt", "/public") -> true
bool is_path_in_folder(const char* path, const char* folder) {
    char norm_path[MAX_PATH_LENGTH], norm_folder[MAX_PATH_LENGTH];
    strcpy(norm_path, path);
    strcpy(norm_folder, folder);
    
    normalize_path(norm_path);
    normalize_path(norm_folder);
    
    // Root folder contains everything
    if (strcmp(norm_folder, "/") == 0) return true;
    
    // Check if path starts with folder/
    int folder_len = strlen(norm_folder);
    if (strncmp(norm_path, norm_folder, folder_len) == 0) {
        // Must be followed by / or end of string
        if (norm_path[folder_len] == '/' || norm_path[folder_len] == '\0') {
            return true;
        }
    }
    
    return false;
}

// Get parent folder from path
// e.g., "/public/reports/file.txt" -> "/public/reports"
char* get_parent_folder(const char* path) {
    static char parent[MAX_PATH_LENGTH];
    strcpy(parent, path);
    
    // Find last /
    char* last_slash = strrchr(parent, '/');
    if (last_slash != NULL) {
        if (last_slash == parent) {
            // Root is parent
            strcpy(parent, "/");
        } else {
            *last_slash = '\0';
        }
    }
    
    return parent;
}

// Get filename from path
// e.g., "/public/reports/file.txt" -> "file.txt"
char* get_filename_from_path(const char* path) {
    char* last_slash = strrchr(path, '/');
    if (last_slash != NULL) {
        return last_slash + 1;
    }
    return (char*)path;
}

// ============================================
// BONUS 2: CHECKPOINT OPERATIONS
// ============================================

bool add_checkpoint(EnhancedFileMetadata* file, const char* tag) {
    if (file->checkpoint_count >= MAX_CHECKPOINTS) {
        return false; // No more space
    }
    
    // Check if tag already exists
    for (int i = 0; i < file->checkpoint_count; i++) {
        if (strcmp(file->checkpoints[i].tag, tag) == 0) {
            return false; // Duplicate tag
        }
    }
    
    strcpy(file->checkpoints[file->checkpoint_count].tag, tag);
    file->checkpoints[file->checkpoint_count].created_at = time(NULL);
    file->checkpoint_count++;
    
    return true;
}

Checkpoint* find_checkpoint(EnhancedFileMetadata* file, const char* tag) {
    for (int i = 0; i < file->checkpoint_count; i++) {
        if (strcmp(file->checkpoints[i].tag, tag) == 0) {
            return &file->checkpoints[i];
        }
    }
    return NULL;
}

bool remove_checkpoint(EnhancedFileMetadata* file, const char* tag) {
    for (int i = 0; i < file->checkpoint_count; i++) {
        if (strcmp(file->checkpoints[i].tag, tag) == 0) {
            // Shift remaining checkpoints
            for (int j = i; j < file->checkpoint_count - 1; j++) {
                file->checkpoints[j] = file->checkpoints[j + 1];
            }
            file->checkpoint_count--;
            return true;
        }
    }
    return false;
}

// ============================================
// BONUS 3: ACCESS REQUEST OPERATIONS
// ============================================

int add_access_request(const char* filename, const char* user, bool for_write) {
    pthread_mutex_lock(&request_mutex);
    
    if (request_count >= MAX_REQUESTS) {
        pthread_mutex_unlock(&request_mutex);
        return -1;
    }
    
    pending_requests[request_count].request_id = next_request_id++;
    strcpy(pending_requests[request_count].filename, filename);
    strcpy(pending_requests[request_count].requesting_user, user);
    pending_requests[request_count].for_write = for_write;
    pending_requests[request_count].requested_at = time(NULL);
    
    int id = pending_requests[request_count].request_id;
    request_count++;
    
    pthread_mutex_unlock(&request_mutex);
    return id;
}

// Get all requests where the user is the file owner
AccessRequest* get_requests_for_owner(const char* owner, int* count) {
    // This will be called from nameserver with file ownership info
    // Return value needs to be processed there
    *count = 0;
    return NULL; // Placeholder - actual implementation in nameserver
}

bool approve_request(int request_id) {
    pthread_mutex_lock(&request_mutex);
    
    for (int i = 0; i < request_count; i++) {
        if (pending_requests[i].request_id == request_id) {
            // Remove this request (will be processed by nameserver)
            for (int j = i; j < request_count - 1; j++) {
                pending_requests[j] = pending_requests[j + 1];
            }
            request_count--;
            pthread_mutex_unlock(&request_mutex);
            return true;
        }
    }
    
    pthread_mutex_unlock(&request_mutex);
    return false;
}

bool deny_request(int request_id) {
    return approve_request(request_id); // Same removal logic
}

// ============================================
// BONUS 4: REPLICATION OPERATIONS
// ============================================

bool add_replica(EnhancedFileMetadata* file, int ss_index, const char* path) {
    if (file->replica_count >= MAX_REPLICAS) {
        return false;
    }
    
    file->replicas[file->replica_count].ss_index = ss_index;
    strcpy(file->replicas[file->replica_count].path_on_ss, path);
    file->replica_count++;
    
    return true;
}

// Get first online replica (requires server_list from nameserver)
ReplicaLocation* get_online_replica(EnhancedFileMetadata* file) {
    // This needs access to server_list, so it will be implemented in nameserver
    return NULL; // Placeholder
}

// Get all online replicas
int get_all_online_replicas(EnhancedFileMetadata* file, ReplicaLocation* replicas, int max) {
    // This needs access to server_list, so it will be implemented in nameserver
    return 0; // Placeholder
}
