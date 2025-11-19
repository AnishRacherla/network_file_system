#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/stat.h> // For mkdir
#include <errno.h>    // For errno and EEXIST
#include <ctype.h>    // For isspace
#include <dirent.h>   // For directory scanning
#include "locks.h"    // Our new locking system
#include<stdbool.h>

#define NS_PORT 8080        // Name Server's port
#define MY_PORT 9001        // This SS's port for Clients/NS
#define BUFFER_SIZE 1024
#define CHUNK_SIZE 4096     // For file streaming
#define MAX_SENTENCES 1000
#define MAX_WORDS_UPDATE 100

// Global to track sentence delimiters
char sentence_delimiters[MAX_SENTENCES]; // Stores the delimiter that ended each sentence ('\n', '.', '!', '?', or '\0' for last)

// Global storage directory for this SS instance
char STORAGE_DIR[256] = "data";

// --- Helper: Read file into a string ---
// Returns a dynamically allocated string. Caller must free().
char* read_file_to_memory(const char* path) {
    FILE* file = fopen(path, "r");
    if (!file) {
        // If file doesn't exist (e.g., new file), return empty string
        char* empty_str = (char*)malloc(1);
        empty_str[0] = '\0';
        return empty_str;
    }
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    char* buffer = (char*)malloc(length + 1);
    if (buffer) {
        fread(buffer, 1, length, file);
        buffer[length] = '\0';
    }
    fclose(file);
    return buffer;
}

// --- Helper: Parse content into sentences ---
// Creates independent copies of each sentence (non-destructive)
// Sentences are split by: . ! ? OR newline
// Tracks delimiters in global sentence_delimiters array
int parse_sentences(char* content, char* sentences[], int max_sentences) {
    if (content == NULL) return 0;
    int count = 0;
    char* start = content;
    // Trim leading whitespace
    while (isspace(*start)) start++;
    
    if (*start == '\0') return 0; // Empty content

    for (int i = 0; content[i] != '\0' && count < max_sentences; i++) {
        // Check for sentence delimiters: . ! ? (newline is NOT a delimiter, just whitespace)
        if (content[i] == '.' || content[i] == '!' || content[i] == '?') {
            // Calculate sentence length (include the punctuation)
            int len = (content + i) - start + 1;
            
            // Always create a sentence, even if empty (just punctuation)
            sentences[count] = (char*)malloc(len + 1);
            if (len > 0) {
                strncpy(sentences[count], start, len);
            }
            sentences[count][len] = '\0';
            
            // Trim trailing whitespace from sentence (but keep the punctuation)
            int trim_len = len;
            while (trim_len > 0 && isspace(sentences[count][trim_len - 1])) {
                sentences[count][trim_len - 1] = '\0';
                trim_len--;
            }
            
            // Count ALL sentences, including empty ones (just punctuation)
            sentence_delimiters[count] = content[i]; // Store the delimiter
            count++;
            
            // Move to start of next sentence
            start = content + i + 1;
            // Skip whitespace for next sentence
            while (*start != '\0' && isspace(*start)) start++;
            i = start - content - 1; // Adjust i to match new start
        }
    }
    
    // If there's any text left, it's the last "sentence"
    if (*start != '\0' && count < max_sentences) {
        // Trim trailing whitespace
        char* end = start + strlen(start) - 1;
        while (end > start && isspace(*end)) end--;
        int len = end - start + 1;
        if (len > 0) {
            sentences[count] = (char*)malloc(len + 1);
            strncpy(sentences[count], start, len);
            sentences[count][len] = '\0';
            sentence_delimiters[count] = '\0'; // No delimiter for last sentence
            count++;
        }
    }
    return count;
}

// --- Helper: Count words and chars ---
void count_words_chars(const char* content, int* word_count, int* char_count) {
    *word_count = 0;
    *char_count = 0;
    int in_word = 0;
    if (content == NULL) return;

    for (int i = 0; content[i] != '\0'; i++) {
        *char_count += 1; // Count all characters
        if (isspace(content[i])) {
            in_word = 0;
        } else {
            if (in_word == 0) {
                *word_count += 1;
                in_word = 1;
            }
        }
    }
}

// --- Helper: Report metadata to NS ---
// Opens a new connection to the NS to report updates
void report_meta_to_ns(const char* path_on_ss, int wc, int cc) {
    int ns_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ns_sock < 0) {
        perror("ns_sock for meta update");
        return;
    }

    struct sockaddr_in ns_addr;
    memset(&ns_addr, 0, sizeof(ns_addr));
    ns_addr.sin_family = AF_INET;
    ns_addr.sin_port = htons(NS_PORT);
    if (inet_pton(AF_INET, "127.0.0.1", &ns_addr.sin_addr) <= 0) {
        perror("inet_pton for meta update");
        close(ns_sock);
        return;
    }

    if (connect(ns_sock, (struct sockaddr *)&ns_addr, sizeof(ns_addr)) < 0) {
        perror("connect to NS for meta update failed");
        close(ns_sock);
        return;
    }

    char msg[BUFFER_SIZE];
    snprintf(msg, BUFFER_SIZE, "UPDATE_META %s %d %d", path_on_ss, wc, cc);
    
    send(ns_sock, msg, strlen(msg), 0);
    recv(ns_sock, msg, BUFFER_SIZE - 1, 0); // Wait for OK
    close(ns_sock);
    
    printf("[SS_META_UPDATE] Reported to NS: %s, wc: %d, cc: %d\n", path_on_ss, wc, cc);
}


// --- Main Connection Handler ---
void* handle_ss_connection(void* arg) {
    int conn_fd = *(int*)arg;
    free(arg);

    printf("[SS_THREAD] New connection accepted (socket %d)\n", conn_fd);
    fflush(stdout);

    char buffer[BUFFER_SIZE];
    
    while (1) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_read = recv(conn_fd, buffer, BUFFER_SIZE - 1, 0);

        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            printf("[SS_THREAD %d] Received: %s\n", conn_fd, buffer);

            if (strncmp(buffer, "INTERNAL_CREATEFOLDER", 21) == 0) {
                // Create an actual directory using mkdir()
                char command[100], path[512];
                sscanf(buffer, "%s %s", command, path);
                printf("[SS_THREAD %d] Request to CREATE FOLDER: %s\n", conn_fd, path);
                
                // Create parent directories recursively (like mkdir -p)
                char temp_path[512];
                strcpy(temp_path, path);
                char* p = NULL;
                
                // Skip the first '/' if path starts with it
                for (p = temp_path + 1; *p; p++) {
                    if (*p == '/') {
                        *p = '\0';  // Temporarily terminate string
                        
                        // Try to create this parent directory
                        if (mkdir(temp_path, 0755) != 0 && errno != EEXIST) {
                            // Failed and it's not because directory already exists
                            perror("mkdir parent failed");
                        }
                        
                        *p = '/';  // Restore the slash
                    }
                }
                
                // Now create the final directory
                if (mkdir(path, 0755) == 0) {
                    printf("[SS_THREAD %d] Folder created: %s\n", conn_fd, path);
                    send(conn_fd, "200 OK", 6, 0);
                } else if (errno == EEXIST) {
                    printf("[SS_THREAD %d] Folder already exists: %s\n", conn_fd, path);
                    send(conn_fd, "200 OK", 6, 0);  // Consider existing folder as success
                } else {
                    perror("mkdir failed");
                    send(conn_fd, "500 ERROR: MKDIR FAILED", 23, 0);
                }
                // Don't break - keep the connection open for NS to receive the response
                close(conn_fd);
                return NULL;
                
            } else if (strncmp(buffer, "INTERNAL_CREATE", 15) == 0) {
                char command[100], path[512];
                sscanf(buffer, "%s %s", command, path);
                FILE *file = fopen(path, "w");
                if (file) { 
                    fclose(file); 
                    send(conn_fd, "200 OK", 6, 0);
                } else { 
                    perror("fopen create");
                    send(conn_fd, "500 ERROR: FOPEN", 16, 0); 
                }
                return NULL;
                
            } else if (strncmp(buffer, "CLIENT_READ", 11) == 0) {
                char command[100], path[512];
                sscanf(buffer, "%s %s", command, path);
                printf("[SS_THREAD %d] Request to READ: %s\n", conn_fd, path);

                FILE* file = fopen(path, "r");
                if (!file) {
                    perror("fopen read");
                    send(conn_fd, "404 ERROR: FILE NOT FOUND", 25, 0);
                    continue; // Keep connection open
                }

                char read_buffer[CHUNK_SIZE];
                size_t file_bytes_read;
                while ((file_bytes_read = fread(read_buffer, 1, CHUNK_SIZE, file)) > 0) {
                    if (send(conn_fd, read_buffer, file_bytes_read, 0) < 0) {
                        perror("send stream failed");
                        break; // Stop streaming if client disconnects
                    }
                }
                fclose(file);
                printf("[SS_THREAD %d] File stream complete.\n", conn_fd);
                break; // This operation is done, close the connection
                
            } else if (strncmp(buffer, "GET_SENTENCE_COUNT", 18) == 0) {
                // --- Get sentence count for INFO command ---
                char command[100], path[512];
                sscanf(buffer, "%s %s", command, path);
                printf("[SS_THREAD %d] Request to GET_SENTENCE_COUNT: %s\n", conn_fd, path);

                char* file_content = read_file_to_memory(path);
                if (!file_content) {
                    send(conn_fd, "0", 1, 0);
                } else {
                    char* temp_sentences[MAX_SENTENCES] = {0};
                    int sentence_count = parse_sentences(file_content, temp_sentences, MAX_SENTENCES);
                    free(file_content);
                    
                    char count_str[32];
                    sprintf(count_str, "%d", sentence_count);
                    send(conn_fd, count_str, strlen(count_str), 0);
                }
                close(conn_fd);
                return NULL;
                
            } else if (strncmp(buffer, "CLIENT_WRITE_LOCK", 17) == 0) {
                // --- REAL WRITE LOGIC ---
                char command[100], path[512], username[100];
                int sentence_num;
                sscanf(buffer, "%s %s %d %s", command, path, &sentence_num, username);
                printf("[SS_THREAD %d] %s requests to LOCK %s, sentence %d\n", conn_fd, username, path, sentence_num);

                // Validate sentence number BEFORE acquiring lock
                if (sentence_num < 0) {
                    char error_msg[BUFFER_SIZE];
                    snprintf(error_msg, BUFFER_SIZE, "ERR 114 INVALID_INDEX_NEGATIVE: Sentence index cannot be negative (got %d)", sentence_num);
                    send(conn_fd, error_msg, strlen(error_msg), 0);
                    break;
                }
                
                // Read file to check sentence count
                char* file_content_check = read_file_to_memory(path);
                char* temp_sentences[MAX_SENTENCES] = {0};
                int current_sentence_count = parse_sentences(file_content_check, temp_sentences, MAX_SENTENCES);
                
                // Check if trying to write to a new sentence (sentence_num == current_sentence_count)
                // This is only allowed if the last sentence ends with proper punctuation
                if (sentence_num == current_sentence_count && current_sentence_count > 0) {
                    // Check if the last sentence ends with . ! or ?
                    char* last_sentence = temp_sentences[current_sentence_count - 1];
                    size_t len = strlen(last_sentence);
                    bool properly_terminated = false;
                    
                    if (len > 0) {
                        char last_char = last_sentence[len - 1];
                        if (last_char == '.' || last_char == '!' || last_char == '?') {
                            properly_terminated = true;
                        }
                    }
                    
                    if (!properly_terminated) {
                        char error_msg[BUFFER_SIZE];
                        snprintf(error_msg, BUFFER_SIZE, 
                                "ERR 105 INCOMPLETE_SENTENCE: Cannot create sentence %d. Previous sentence %d is not properly terminated (must end with . ! or ?). Complete sentence %d first.", 
                                sentence_num, current_sentence_count - 1, current_sentence_count - 1);
                        send(conn_fd, error_msg, strlen(error_msg), 0);
                        if (file_content_check) free(file_content_check);
                        break;
                    }
                }
                
                // Allow writing to sentence_count (creates new sentence) but not beyond
                if (sentence_num > current_sentence_count) {
                    char error_msg[BUFFER_SIZE];
                    snprintf(error_msg, BUFFER_SIZE, "ERR 103 INDEX_OUT_OF_RANGE: Sentence %d out of range (file has %d sentences). Cannot skip - use sentence %d to append.", 
                             sentence_num, current_sentence_count, current_sentence_count);
                    send(conn_fd, error_msg, strlen(error_msg), 0);
                    if (file_content_check) free(file_content_check);
                    break;
                }
                if (file_content_check) free(file_content_check);

                // 1. Acquire Lock
                int lock_status = acquire_lock(path, sentence_num, username);
                if (lock_status == 1) {
                    send(conn_fd, "423 ERROR: LOCKED", 17, 0);
                    break; // Fail, close connection
                } else if (lock_status == -1) {
                    send(conn_fd, "500 ERROR: MALLOC", 17, 0);
                    break;
                }
                
                send(conn_fd, "200 OK: LOCKED", 14, 0);

                // 2. Buffer word updates
                char* word_updates[MAX_WORDS_UPDATE];
                int update_count = 0;
                while (1) {
                    memset(buffer, 0, BUFFER_SIZE);
                    bytes_read = recv(conn_fd, buffer, BUFFER_SIZE - 1, 0);
                    if (bytes_read <= 0) { 
                        printf("[SS_THREAD %d] Client disconnected mid-write.\n", conn_fd);
                        release_lock(path, sentence_num); // Release the lock
                        break; 
                    }
                    buffer[bytes_read] = '\0';
                    
                    if (strncmp(buffer, "ETIRW", 5) == 0) break; // Done!

                    if (update_count < MAX_WORDS_UPDATE) {
                        word_updates[update_count] = strdup(buffer);
                        update_count++;
                    }
                }
                if (bytes_read <= 0) break; // Exit if client disconnected

                // 3. Process the write
                printf("[SS_THREAD %d] ETIRW received. Processing write...\n", conn_fd);
                char* file_content = read_file_to_memory(path);
                printf("[SS_THREAD %d] File content length: %ld, first 50 chars: '%.50s'\n", 
                       conn_fd, strlen(file_content), file_content);
                
                char* sentences[MAX_SENTENCES] = {0};
                int sentence_count = parse_sentences(file_content, sentences, MAX_SENTENCES);

                printf("[SS_THREAD %d] File has %d sentences. Requested sentence: %d\n", conn_fd, sentence_count, sentence_num);
                
                // Debug: print all sentences
                for (int i = 0; i < sentence_count; i++) {
                    printf("[SS_THREAD %d]   Sentence[%d]: '%s'\n", conn_fd, i, sentences[i]);
                }

                // SPECIAL CASES:
                // 1. Empty file + sentence 0 request = create new sentence
                // 2. Content without delimiters + sentence 0 = treat as one sentence
                // 3. Requesting sentence at the end (sentence_num == sentence_count) = create new sentence
                if (sentence_count == 0 && sentence_num == 0) {
                    if (strlen(file_content) > 0) {
                        printf("[SS_THREAD %d] Special case: File has content but no delimiters. Treating as single sentence.\n", conn_fd);
                        sentences[0] = file_content;
                        sentence_count = 1;
                    } else {
                        printf("[SS_THREAD %d] Special case: Empty file. Creating new sentence 0.\n", conn_fd);
                        // Start with an empty sentence that will be populated by word updates
                        char* empty_sentence = strdup("");
                        sentences[0] = empty_sentence;
                        sentence_count = 1;
                        if (file_content) free(file_content);
                        file_content = empty_sentence; // Point to the new sentence
                    }
                } else if (sentence_num == sentence_count && sentence_num < MAX_SENTENCES) {
                    // User wants to create a new sentence at the end
                    printf("[SS_THREAD %d] Creating new sentence %d at end of file\n", conn_fd, sentence_num);
                    char* empty_sentence = strdup("");
                    sentences[sentence_num] = empty_sentence;
                    sentence_count++;
                }

                // Check if sentence_num is valid (can be at most sentence_count after expansion)
                if (sentence_num >= sentence_count) {
                    printf("[SS_THREAD %d] ERROR: Sentence %d doesn't exist (only %d sentences)\n", conn_fd, sentence_num, sentence_count);
                    if (file_content) free(file_content);
                    for(int i = 0; i < update_count; i++) free(word_updates[i]);
                    release_lock(path, sentence_num);
                    
                    char error_msg[BUFFER_SIZE];
                    snprintf(error_msg, BUFFER_SIZE, "404 ERROR: SENTENCE %d NOT FOUND (file has %d sentences). Cannot skip sentences - use WRITE <file> %d to add next sentence.", sentence_num, sentence_count, sentence_count);
                    send(conn_fd, error_msg, strlen(error_msg), 0);
                    break;
                }

                // Parse the target sentence into words
                char* target_sentence = sentences[sentence_num];
                char* words[1000]; // Max words in a sentence
                int word_count = 0;
                
                // Tokenize the sentence into words
                char* sentence_copy = strdup(target_sentence);
                char* token = strtok(sentence_copy, " \t\n");
                while (token != NULL && word_count < 1000) {
                    words[word_count++] = strdup(token);
                    token = strtok(NULL, " \t\n");
                }
                free(sentence_copy);

                printf("[SS_THREAD %d] Sentence %d has %d words\n", conn_fd, sentence_num, word_count);

                // Check if sentence already ends with punctuation
                int sentence_end_index = -1;
                for (int i = 0; i < word_count; i++) {
                    if (words[i] && (strcmp(words[i], ".") == 0 || 
                                     strcmp(words[i], "!") == 0 || 
                                     strcmp(words[i], "?") == 0)) {
                        sentence_end_index = i;
                        break; // First punctuation marks the end
                    }
                }

                // First pass: Validate all word indices to prevent gaps
                for (int i = 0; i < update_count; i++) {
                    int word_index;
                    char rest_of_line[1024];
                    if (sscanf(word_updates[i], "%d %[^\n]", &word_index, rest_of_line) == 2) {
                        if (word_index < 0) {
                            char error_msg[BUFFER_SIZE];
                            snprintf(error_msg, BUFFER_SIZE, 
                                    "ERR 114 INVALID_INDEX_NEGATIVE: Word index cannot be negative (got %d)", 
                                    word_index);
                            send(conn_fd, error_msg, strlen(error_msg), 0);
                            
                            // Cleanup
                            if (file_content) free(file_content);
                            for(int j = 0; j < update_count; j++) free(word_updates[j]);
                            for(int j = 0; j < word_count; j++) if(words[j]) free(words[j]);
                            release_lock(path, sentence_num);
                            break;
                        }
                        
                        // Check if trying to insert after sentence-ending punctuation (existing in file)
                        if (sentence_end_index >= 0 && word_index > sentence_end_index) {
                            char error_msg[BUFFER_SIZE];
                            snprintf(error_msg, BUFFER_SIZE, 
                                    "ERR 106 SENTENCE_ALREADY_ENDED: Cannot insert at word %d. Sentence already ended at word %d with '%s'. Use next sentence number to add more content.", 
                                    word_index, sentence_end_index, words[sentence_end_index]);
                            send(conn_fd, error_msg, strlen(error_msg), 0);
                            
                            // // Cleanup
                            // if (file_content) free(file_content);
                            // for(int j = 0; j < update_count; j++) free(word_updates[j]);
                            // for(int j = 0; j < word_count; j++) if(words[j]) free(words[j]);
                            // release_lock(path, sentence_num);
                            // return;
                        }
                        
                        // Check if any PREVIOUS insertion in this batch added punctuation
                        for (int j = 0; j < i; j++) {
                            int prev_idx;
                            char prev_text[1024];
                            if (sscanf(word_updates[j], "%d %[^\n]", &prev_idx, prev_text) == 2) {
                                // Check if previous insertion was punctuation
                                if (strcmp(prev_text, ".") == 0 || strcmp(prev_text, "!") == 0 || strcmp(prev_text, "?") == 0) {
                                    // If current insertion is after the punctuation, it's an error
                                    if (word_index > prev_idx) {
                                        char error_msg[BUFFER_SIZE];
                                        snprintf(error_msg, BUFFER_SIZE, 
                                                "ERR 106 SENTENCE_ALREADY_ENDED: Cannot insert at word %d. Previous insertion added '%s' at word %d, ending the sentence.", 
                                                word_index, prev_text, prev_idx);
                                        send(conn_fd, error_msg, strlen(error_msg), 0);
                                        
                                        // Cleanup
                                        // if (file_content) free(file_content);
                                        // for(int k = 0; k < update_count; k++) free(word_updates[k]);
                                        // for(int k = 0; k < word_count; k++) if(words[k]) free(words[k]);
                                        // release_lock(path, sentence_num);
                                        // return;
                                    }
                                }
                            }
                        }
                        
                        // Calculate what word_count will be after processing previous updates
                        int projected_word_count = word_count;
                        for (int j = 0; j < i; j++) {
                            int prev_idx;
                            char dummy[1024];
                            if (sscanf(word_updates[j], "%d %[^\n]", &prev_idx, dummy) == 2) {
                                if (prev_idx >= 0 && prev_idx <= projected_word_count) {
                                    projected_word_count++;
                                }
                            }
                        }
                        
                        if (word_index > projected_word_count) {
                            char error_msg[BUFFER_SIZE];
                            snprintf(error_msg, BUFFER_SIZE, 
                                    "ERR 105 INVALID_WORD_INDEX: Cannot insert at word %d (sentence will have %d words after previous insertions). Would create gap. Valid range: 0-%d.", 
                                    word_index, projected_word_count, projected_word_count);
                            send(conn_fd, error_msg, strlen(error_msg), 0);
                            
           
                        }
                    }
                }

                // Second pass: Apply all word updates - INSERT mode
                for (int i = 0; i < update_count; i++) {
                    int word_index;
                    char rest_of_line[1024];
                    if (sscanf(word_updates[i], "%d %[^\n]", &word_index, rest_of_line) == 2) {
                        if (word_index >= 0 && word_index <= word_count && word_count < 1000) {
                            // Replace literal \n with actual newline character
                            char processed_text[1024];
                            char* src = rest_of_line;
                            char* dst = processed_text;
                            while (*src) {
                                if (*src == '\\' && *(src + 1) == 'n') {
                                    *dst++ = '\n';  // Replace \n with actual newline
                                    src += 2;
                                    // Skip any whitespace immediately after \n
                                    while (*src == ' ' || *src == '\t') {
                                        src++;
                                    }
                                } else {
                                    *dst++ = *src++;
                                }
                            }
                            *dst = '\0';
                            
                            // Shift words to the right to make space for insertion
                            for (int j = word_count; j > word_index; j--) {
                                words[j] = words[j - 1];
                            }
                            // Insert the processed text at word_index
                            words[word_index] = strdup(processed_text);
                            word_count++;
                            printf("[SS_THREAD %d] Inserted '%s' at word[%d], word_count now %d\n", 
                                   conn_fd, processed_text, word_index, word_count);
                        }
                    }
                }

                // Rebuild the modified sentence
                char* new_sentence = (char*)malloc(4096);
                new_sentence[0] = '\0';
                for (int i = 0; i < word_count; i++) {
                    // Skip empty words created as placeholders
                    if (words[i] && strlen(words[i]) > 0) {
                        // Check if current word is just punctuation
                        int is_just_punctuation = 1;
                        for (int j = 0; words[i][j] != '\0'; j++) {
                            if (words[i][j] != '.' && words[i][j] != '!' && words[i][j] != '?') {
                                is_just_punctuation = 0;
                                break;
                            }
                        }
                        
                        // Only add space if:
                        // 1. Not the first word AND
                        // 2. new_sentence is not empty AND
                        // 3. Previous character is not a newline AND
                        // 4. Current word doesn't start with newline AND
                        // 5. Current word is not just punctuation
                        size_t len = strlen(new_sentence);
                        if (i > 0 && len > 0 && 
                            new_sentence[len - 1] != '\n' && 
                            words[i][0] != '\n' &&
                            !is_just_punctuation) {
                            strcat(new_sentence, " ");
                        }
                        strcat(new_sentence, words[i]);
                    }
                    if (words[i]) free(words[i]); // Free word memory
                }
                
                printf("[SS_THREAD %d] Rebuilt sentence[%d]: '%s'\n", conn_fd, sentence_num, new_sentence);
                
                // Replace the sentence in the array
                sentences[sentence_num] = new_sentence;

                // --- BACKUP FOR UNDO: Only save backup if actual changes were made ---
                if (update_count > 0) {
                    char backup_path[600];
                    snprintf(backup_path, sizeof(backup_path), "%s.undo", path);
                    FILE* backup_file = fopen(path, "r");
                    if (backup_file) {
                        FILE* undo_file = fopen(backup_path, "w");
                        if (undo_file) {
                            char backup_buffer[4096];
                            size_t bytes;
                            while ((bytes = fread(backup_buffer, 1, sizeof(backup_buffer), backup_file)) > 0) {
                                fwrite(backup_buffer, 1, bytes, undo_file);
                            }
                            fclose(undo_file);
                            printf("[SS_THREAD %d] Saved backup to %s (after %d updates)\n", conn_fd, backup_path, update_count);
                        }
                        fclose(backup_file);
                    }
                    
                    // --- INVALIDATE REDO: New changes invalidate any existing redo history ---
                    char redo_path[600];
                    snprintf(redo_path, sizeof(redo_path), "%s.redo", path);
                    if (access(redo_path, F_OK) == 0) {
                        remove(redo_path);
                        printf("[SS_THREAD %d] Removed redo file %s (invalidated by new changes)\n", conn_fd, redo_path);
                    }
                }

                // 4. Rebuild file and write to temp ONLY if changes were made
                if (update_count > 0) {
                    char temp_path[512];
                    snprintf(temp_path, 512, "%s.tmp", path); // SAFE snprintf
                    FILE* temp_file = fopen(temp_path, "w");
                    
                    size_t total_size = CHUNK_SIZE * 10;
                    char* full_new_content = (char*)malloc(total_size);
                    full_new_content[0] = '\0';

                    for (int i = 0; i < sentence_count; i++) {
                        if (strlen(full_new_content) + strlen(sentences[i]) + 3 > total_size) {
                            total_size *= 2;
                            full_new_content = (char*)realloc(full_new_content, total_size);
                        }
                        
                        // Trim trailing whitespace from sentence before adding
                        char* sentence = sentences[i];
                        size_t len = strlen(sentence);
                        while (len > 0 && isspace(sentence[len - 1])) {
                            sentence[len - 1] = '\0';
                            len--;
                        }
                        
                        strcat(full_new_content, sentence);
                        
                        // Restore the original delimiter
                        if (i < sentence_count - 1) {
                            // Check if next sentence is empty (just punctuation)
                            char* next_sentence = sentences[i + 1];
                            int next_is_empty = 1;
                            for (int j = 0; next_sentence[j] != '\0'; j++) {
                                if (next_sentence[j] != '.' && next_sentence[j] != '!' && next_sentence[j] != '?') {
                                    next_is_empty = 0;
                                    break;
                                }
                            }
                            
                            // If delimiter was newline, add newline
                            if (sentence_delimiters[i] == '\n') {
                                strcat(full_new_content, "\n");
                            } else if (sentence_delimiters[i] == '.' || 
                                      sentence_delimiters[i] == '!' || 
                                      sentence_delimiters[i] == '?') {
                                // Only add space if next sentence is NOT empty (has content beyond punctuation)
                                if (!next_is_empty) {
                                    strcat(full_new_content, " ");
                                }
                            } else {
                                // Default: add space
                                strcat(full_new_content, " ");
                            }
                        }
                    }
                    
                    // Normalize: remove spaces/tabs immediately before newlines so a newline
                    // does not start with a space on the next line (user-visible formatting)
                    char* norm = (char*)malloc(strlen(full_new_content) + 1);
                    int ni = 0;
                    for (size_t fi = 0; full_new_content[fi] != '\0'; fi++) {
                        if ((full_new_content[fi] == ' ' || full_new_content[fi] == '\t')) {
                            size_t k = fi;
                            while (full_new_content[k] == ' ' || full_new_content[k] == '\t') k++;
                            if (full_new_content[k] == '\n') {
                                // Skip these spaces (advance fi to k-1, loop will increment)
                                fi = k - 1;
                                continue;
                            }
                        }
                        norm[ni++] = full_new_content[fi];
                    }
                    norm[ni] = '\0';
                    fwrite(norm, 1, strlen(norm), temp_file);
                    fclose(temp_file);
                    free(norm);

                    // 5. Atomic rename
                    if (rename(temp_path, path) != 0) {
                        perror("rename failed");
                        send(conn_fd, "500 ERROR: RENAME", 18, 0);
                        // (cleanup memory)
                        release_lock(path, sentence_num);
                        break;
                    }

                    // 6. Recount and Report
                    int new_wc = 0, new_cc = 0;
                    count_words_chars(full_new_content, &new_wc, &new_cc);
                    report_meta_to_ns(path, new_wc, new_cc);
                    
                    free(full_new_content);
                }
                
                // 7. Free all memory
                if (file_content) free(file_content); // Free the original file content
                for (int i = 0; i < sentence_count; i++) {
                    if (sentences[i] && sentences[i] != new_sentence) {
                        free(sentences[i]); // Free sentence copies (except the one we just created)
                    }
                }
                for(int i = 0; i < update_count; i++) free(word_updates[i]);
                // Free the new sentence we malloc'd if it was created
                if (update_count > 0) free(new_sentence);
                
                // 8. Release lock and send success
                release_lock(path, sentence_num);
                send(conn_fd, "200 OK: WRITE COMPLETE", 22, 0);
                break; // This operation is done
            
            } else if (strncmp(buffer, "INTERNAL_APPEND", 15) == 0) {
                // --- NEW: APPEND COMMAND ---
                char command[100], path[512];
                sscanf(buffer, "%s %s", command, path);
                
                char* content_start = strstr(buffer, path);
                if (content_start) {
                    content_start += strlen(path);
                    while(*content_start == ' ') content_start++; // Skip spaces
                }
                
                if (content_start == NULL || *content_start == '\0') {
                    send(conn_fd, "400 ERROR: NO CONTENT", 21, 0);
                    continue;
                }
                
                printf("[SS_THREAD %d] Request to APPEND: %s\n", conn_fd, path);
                
                // --- BACKUP FOR UNDO: Save current file before appending ---
                char backup_path[600];
                snprintf(backup_path, sizeof(backup_path), "%s.undo", path);
                FILE* orig_file = fopen(path, "r");
                if (orig_file) {
                    FILE* undo_file = fopen(backup_path, "w");
                    if (undo_file) {
                        char backup_buffer[4096];
                        size_t bytes;
                        while ((bytes = fread(backup_buffer, 1, sizeof(backup_buffer), orig_file)) > 0) {
                            fwrite(backup_buffer, 1, bytes, undo_file);
                        }
                        fclose(undo_file);
                        printf("[SS_THREAD %d] Saved undo backup to %s\n", conn_fd, backup_path);
                    }
                    fclose(orig_file);
                }
                
                // --- INVALIDATE REDO: New changes invalidate any existing redo history ---
                char redo_path[600];
                snprintf(redo_path, sizeof(redo_path), "%s.redo", path);
                if (access(redo_path, F_OK) == 0) {
                    remove(redo_path);
                    printf("[SS_THREAD %d] Removed redo file %s (invalidated by append)\n", conn_fd, redo_path);
                }
                
                FILE* file = fopen(path, "a"); // "a" = append mode
                if (file) {
                    fprintf(file, " %s", content_start); // Add a space
                    fclose(file);
                    
                    // Re-read, re-count, and report
                    char* full_content = read_file_to_memory(path);
                    int new_wc = 0, new_cc = 0;
                    count_words_chars(full_content, &new_wc, &new_cc);
                    free(full_content);
                    report_meta_to_ns(path, new_wc, new_cc);
                    
                    send(conn_fd, "200 OK", 6, 0);
                } else {
                    perror("fopen append");
                    send(conn_fd, "500 ERROR: FOPEN", 16, 0);
                }
                break; // This operation is done
                
            } else if (strncmp(buffer, "CLIENT_STREAM", 13) == 0) {
                // --- STREAM COMMAND: Send file word-by-word ---
                char command[100], path[512];
                sscanf(buffer, "%s %s", command, path);
                printf("[SS_THREAD %d] Request to STREAM: %s\n", conn_fd, path);
                
                char* content = read_file_to_memory(path);
                if (content == NULL) {
                    send(conn_fd, "|END|", 5, 0);
                    break;
                }
                
                // Tokenize by spaces and send word-by-word
                char* token = strtok(content, " \t\n\r");
                while (token != NULL) {
                    send(conn_fd, token, strlen(token), 0);
                    
                    // Peek ahead to see if there's another word
                    char* next_token = strtok(NULL, " \t\n\r");
                    if (next_token != NULL) {
                        send(conn_fd, " ", 1, 0); // Send space between words
                        token = next_token;
                    } else {
                        token = NULL; // No more tokens
                    }
                    
                    usleep(100000); // 0.1s delay between words
                }
                
                // Send end marker
                send(conn_fd, "|END|", 5, 0);
                free(content);
                break;
                
            } else if (strncmp(buffer, "INTERNAL_UNDO", 13) == 0) {
                // --- UNDO COMMAND: Restore from backup ---
                char command[100], path[512];
                sscanf(buffer, "%s %s", command, path);
                printf("[SS_THREAD %d] Request to UNDO: %s\n", conn_fd, path);
                
                // Construct backup path
                char backup_path[600];
                snprintf(backup_path, sizeof(backup_path), "%s.undo", path);
                
                // Check if backup exists
                FILE* backup_file = fopen(backup_path, "r");
                if (backup_file == NULL) {
                    send(conn_fd, "404 ERROR: No undo history available", 36, 0);
                } else {
                    fclose(backup_file);
                    
                    // --- SAVE CURRENT STATE TO REDO: Before undoing, save current version for redo ---
                    char redo_path[600];
                    snprintf(redo_path, sizeof(redo_path), "%s.redo", path);
                    FILE* current_file = fopen(path, "r");
                    if (current_file) {
                        FILE* redo_file = fopen(redo_path, "w");
                        if (redo_file) {
                            char redo_buffer[4096];
                            size_t bytes;
                            while ((bytes = fread(redo_buffer, 1, sizeof(redo_buffer), current_file)) > 0) {
                                fwrite(redo_buffer, 1, bytes, redo_file);
                            }
                            fclose(redo_file);
                            printf("[SS_THREAD %d] Saved current state to %s for redo\n", conn_fd, redo_path);
                        }
                        fclose(current_file);
                    }
                    
                    // Restore from backup
                    char cmd[1024];
                    snprintf(cmd, sizeof(cmd), "cp %s %s", backup_path, path);
                    int ret = system(cmd);
                    
                    if (ret == 0) {
                        // Update metadata after undo
                        char* new_content = read_file_to_memory(path);
                        if (new_content) {
                            int wc, cc;
                            count_words_chars(new_content, &wc, &cc);
                            free(new_content);
                            report_meta_to_ns(path, wc, cc);
                        }
                        send(conn_fd, "200 OK: UNDO successful", 23, 0);
                    } else {
                        send(conn_fd, "500 ERROR: UNDO failed", 22, 0);
                    }
                }
                break;
                
            } else if (strncmp(buffer, "INTERNAL_REDO", 13) == 0) {
                // --- REDO COMMAND: Restore from redo backup ---
                char command[100], path[512];
                sscanf(buffer, "%s %s", command, path);
                printf("[SS_THREAD %d] Request to REDO: %s\n", conn_fd, path);
                
                // Construct redo path
                char redo_path[600];
                snprintf(redo_path, sizeof(redo_path), "%s.redo", path);
                
                // Check if redo backup exists
                FILE* redo_file = fopen(redo_path, "r");
                if (redo_file == NULL) {
                    send(conn_fd, "404 ERROR: No redo history available", 36, 0);
                } else {
                    fclose(redo_file);
                    
                    // --- SAVE CURRENT STATE TO UNDO: Before redoing, save current version for undo ---
                    char backup_path[600];
                    snprintf(backup_path, sizeof(backup_path), "%s.undo", path);
                    FILE* current_file = fopen(path, "r");
                    if (current_file) {
                        FILE* undo_file = fopen(backup_path, "w");
                        if (undo_file) {
                            char undo_buffer[4096];
                            size_t bytes;
                            while ((bytes = fread(undo_buffer, 1, sizeof(undo_buffer), current_file)) > 0) {
                                fwrite(undo_buffer, 1, bytes, undo_file);
                            }
                            fclose(undo_file);
                            printf("[SS_THREAD %d] Saved current state to %s for undo\n", conn_fd, backup_path);
                        }
                        fclose(current_file);
                    }
                    
                    // Restore from redo backup
                    char cmd[1024];
                    snprintf(cmd, sizeof(cmd), "cp %s %s", redo_path, path);
                    int ret = system(cmd);
                    
                    if (ret == 0) {
                        // Update metadata after redo
                        char* new_content = read_file_to_memory(path);
                        if (new_content) {
                            int wc, cc;
                            count_words_chars(new_content, &wc, &cc);
                            free(new_content);
                            report_meta_to_ns(path, wc, cc);
                        }
                        
                        // Remove redo file after successful redo (can't redo again)
                        remove(redo_path);
                        printf("[SS_THREAD %d] Removed redo file after successful redo\n", conn_fd);
                        
                        send(conn_fd, "200 OK: REDO successful", 23, 0);
                    } else {
                        send(conn_fd, "500 ERROR: REDO failed", 22, 0);
                    }
                }
                break;
                
            } else if (strncmp(buffer, "INTERNAL_DELETE", 15) == 0) {
                // --- NEW: DELETE COMMAND ---
                char command[100], path[512];
                sscanf(buffer, "%s %s", command, path);
                printf("[SS_THREAD %d] Request to DELETE: %s\n", conn_fd, path);
                
                // Check if file has any active locks (sentences being written)
                if (has_active_locks(path)) {
                    printf("[SS_THREAD %d] DELETE DENIED: File %s has active write locks\n", conn_fd, path);
                    send(conn_fd, "423 LOCKED: File is being written by another user", 50, 0);
                } else if (remove(path) == 0) {
                    send(conn_fd, "200 OK", 6, 0);
                } else {
                    perror("remove failed");
                    send(conn_fd, "500 ERROR: REMOVE FAILED", 24, 0);
                }
                break; // This operation is done
            
            } else if (strncmp(buffer, "INTERNAL_DELETEFOLDER", 21) == 0) {
                // --- DELETE FOLDER COMMAND ---
                char command[100], path[512];
                sscanf(buffer, "%s %s", command, path);
                printf("[SS_THREAD %d] Request to DELETE FOLDER: %s\n", conn_fd, path);
                
                // Use rmdir() to remove empty directory
                if (rmdir(path) == 0) {
                    printf("[SS_THREAD %d] Folder deleted: %s\n", conn_fd, path);
                    send(conn_fd, "200 OK", 6, 0);
                } else {
                    perror("rmdir failed");
                    send(conn_fd, "500 ERROR: RMDIR FAILED (folder may not be empty)", 50, 0);
                }
                // Close connection after folder operation
                close(conn_fd);
                return NULL;
                
            } else if (strncmp(buffer, "INTERNAL_MOVE", 13) == 0) {
                // --- MOVE FILE/FOLDER COMMAND ---
                char command[100], old_path[512], new_path[512];
                sscanf(buffer, "%s %s %s", command, old_path, new_path);
                printf("[SS_THREAD %d] Request to MOVE: %s -> %s\n", conn_fd, old_path, new_path);
                
                // Use rename() syscall to physically move the file/folder
                if (rename(old_path, new_path) == 0) {
                    printf("[SS_THREAD %d] Moved: %s -> %s\n", conn_fd, old_path, new_path);
                    
                    // Also move the .undo file if it exists
                    char old_undo_path[520], new_undo_path[520];
                    snprintf(old_undo_path, sizeof(old_undo_path), "%s.undo", old_path);
                    snprintf(new_undo_path, sizeof(new_undo_path), "%s.undo", new_path);
                    
                    // Check if undo file exists and move it
                    if (access(old_undo_path, F_OK) == 0) {
                        if (rename(old_undo_path, new_undo_path) == 0) {
                            printf("[SS_THREAD %d] Also moved undo file: %s -> %s\n", conn_fd, old_undo_path, new_undo_path);
                        } else {
                            perror("Warning: undo file rename failed");
                            // Don't fail the entire operation if just undo file move fails
                        }
                    }
                    
                    // Also move the .redo file if it exists
                    char old_redo_path[520], new_redo_path[520];
                    snprintf(old_redo_path, sizeof(old_redo_path), "%s.redo", old_path);
                    snprintf(new_redo_path, sizeof(new_redo_path), "%s.redo", new_path);
                    
                    // Check if redo file exists and move it
                    if (access(old_redo_path, F_OK) == 0) {
                        if (rename(old_redo_path, new_redo_path) == 0) {
                            printf("[SS_THREAD %d] Also moved redo file: %s -> %s\n", conn_fd, old_redo_path, new_redo_path);
                        } else {
                            perror("Warning: redo file rename failed");
                            // Don't fail the entire operation if just redo file move fails
                        }
                    }
                    
                    send(conn_fd, "200 OK", 6, 0);
                } else {
                    perror("rename failed");
                    send(conn_fd, "500 ERROR: RENAME FAILED", 24, 0);
                }
                close(conn_fd);
                return NULL;
                
            } else if (strncmp(buffer, "INTERNAL_CHECKPOINT", 19) == 0) {
                // --- CREATE CHECKPOINT COMMAND ---
                char command[100], path[512], checkpoint_tag[256];
                sscanf(buffer, "%s %s %s", command, path, checkpoint_tag);
                printf("[SS_THREAD %d] Request to CHECKPOINT: %s (tag: %s)\n", conn_fd, path, checkpoint_tag);
                
                // Create checkpoint by copying file to path.checkpoint_tag
                char checkpoint_path[768];
                snprintf(checkpoint_path, sizeof(checkpoint_path), "%s.checkpoint_%s", path, checkpoint_tag);
                
                // Use system cp command to copy file
                char cmd[1536];
                snprintf(cmd, sizeof(cmd), "cp \"%s\" \"%s\"", path, checkpoint_path);
                
                if (system(cmd) == 0) {
                    printf("[SS_THREAD %d] Checkpoint created: %s\n", conn_fd, checkpoint_path);
                    send(conn_fd, "200 OK", 6, 0);
                } else {
                    perror("checkpoint copy failed");
                    send(conn_fd, "500 ERROR: CHECKPOINT FAILED", 28, 0);
                }
                close(conn_fd);
                return NULL;
                
            } else if (strncmp(buffer, "INTERNAL_VIEWCHECKPOINT", 23) == 0) {
                // --- VIEW CHECKPOINT COMMAND ---
                char command[100], path[512], checkpoint_tag[256];
                sscanf(buffer, "%s %s %s", command, path, checkpoint_tag);
                printf("[SS_THREAD %d] Request to VIEW CHECKPOINT: %s (tag: %s)\n", conn_fd, path, checkpoint_tag);
                
                char checkpoint_path[768];
                snprintf(checkpoint_path, sizeof(checkpoint_path), "%s.checkpoint_%s", path, checkpoint_tag);
                
                FILE* file = fopen(checkpoint_path, "r");
                if (!file) {
                    send(conn_fd, "404 ERROR: CHECKPOINT NOT FOUND", 31, 0);
                } else {
                    send(conn_fd, "200 OK:", 7, 0);
                    usleep(10000); // Small delay
                    
                    char line[1024];
                    while (fgets(line, sizeof(line), file)) {
                        send(conn_fd, line, strlen(line), 0);
                    }
                    fclose(file);
                }
                close(conn_fd);
                return NULL;
                
            } else if (strncmp(buffer, "INTERNAL_REVERT", 15) == 0) {
                // --- REVERT TO CHECKPOINT COMMAND ---
                char command[100], path[512], checkpoint_tag[256];
                sscanf(buffer, "%s %s %s", command, path, checkpoint_tag);
                printf("[SS_THREAD %d] Request to REVERT: %s to checkpoint %s\n", conn_fd, path, checkpoint_tag);
                
                char checkpoint_path[768];
                snprintf(checkpoint_path, sizeof(checkpoint_path), "%s.checkpoint_%s", path, checkpoint_tag);
                
                // Copy checkpoint back to original file
                char cmd[1536];
                snprintf(cmd, sizeof(cmd), "cp \"%s\" \"%s\"", checkpoint_path, path);
                
                if (system(cmd) == 0) {
                    printf("[SS_THREAD %d] Reverted %s to checkpoint %s\n", conn_fd, path, checkpoint_tag);
                    send(conn_fd, "200 OK: REVERTED", 16, 0);
                } else {
                    perror("revert failed");
                    send(conn_fd, "500 ERROR: REVERT FAILED (checkpoint may not exist)", 51, 0);
                }
                close(conn_fd);
                return NULL;
                
            } else if (strncmp(buffer, "INTERNAL_LISTCHECKPOINTS", 24) == 0) {
                // --- LIST CHECKPOINTS COMMAND ---
                char command[100], path[512];
                sscanf(buffer, "%s %s", command, path);
                printf("[SS_THREAD %d] Request to LIST CHECKPOINTS: %s\n", conn_fd, path);
                
                // Use ls to find all checkpoint files
                char cmd[1024];
                snprintf(cmd, sizeof(cmd), "ls \"%s\".checkpoint_* 2>/dev/null | sed 's/.*checkpoint_//'", path);
                
                FILE* pipe = popen(cmd, "r");
                if (!pipe) {
                    send(conn_fd, "500 ERROR: FAILED TO LIST", 26, 0);
                } else {
                    char output[4096] = "200 CHECKPOINTS:\n";
                    char line[256];
                    int count = 0;
                    
                    while (fgets(line, sizeof(line), pipe)) {
                        strcat(output, line);
                        count++;
                    }
                    pclose(pipe);
                    
                    if (count == 0) {
                        strcpy(output, "200 NO_CHECKPOINTS");
                    }
                    
                    send(conn_fd, output, strlen(output), 0);
                }
                close(conn_fd);
                return NULL;
                
            } else if (strncmp(buffer, "PING", 4) == 0) {
                // --- PING COMMAND: Heartbeat response ---
                send(conn_fd, "PONG", 4, 0);
                
            } else if (strncmp(buffer, "INTERNAL_REPLICATE", 18) == 0) {
                // --- REPLICATE FILE FROM PRIMARY SS ---
                char command[100], source_path[512], primary_addr[256];
                sscanf(buffer, "%s %s %s", command, source_path, primary_addr);
                printf("[SS_THREAD %d] Request to REPLICATE: %s from %s\n", conn_fd, source_path, primary_addr);
                
                // Parse primary SS IP:port
                char primary_ip[100];
                int primary_port;
                sscanf(primary_addr, "%[^:]:%d", primary_ip, &primary_port);
                
                // Extract filename from source_path
                char* filename = strrchr(source_path, '/');
                if (filename == NULL) {
                    filename = source_path;
                } else {
                    filename++;
                }
                
                // Build local path in this SS's storage directory
                char local_path[512];
                snprintf(local_path, sizeof(local_path), "%s/%s", STORAGE_DIR, filename);
                printf("[SS_THREAD %d] Saving replica to: %s\n", conn_fd, local_path);
                
                // Connect to primary SS and fetch file
                int primary_sock = socket(AF_INET, SOCK_STREAM, 0);
                struct sockaddr_in primary_addr_struct;
                memset(&primary_addr_struct, 0, sizeof(primary_addr_struct));
                primary_addr_struct.sin_family = AF_INET;
                primary_addr_struct.sin_port = htons(primary_port);
                inet_pton(AF_INET, primary_ip, &primary_addr_struct.sin_addr);
                
                if (connect(primary_sock, (struct sockaddr *)&primary_addr_struct, sizeof(primary_addr_struct)) < 0) {
                    printf("[SS_THREAD %d] Failed to connect to primary %s:%d\n", conn_fd, primary_ip, primary_port);
                    send(conn_fd, "500 ERROR: Cannot connect to primary", 37, 0);
                    close(primary_sock);
                    close(conn_fd);
                    return NULL;
                }
                
                // Request file from primary
                char fetch_cmd[1024];
                sprintf(fetch_cmd, "CLIENT_READ %s", source_path);
                printf("[SS_THREAD %d] Sending to primary: %s\n", conn_fd, fetch_cmd);
                send(primary_sock, fetch_cmd, strlen(fetch_cmd), 0);
                
                // Read file content from primary and save to local path
                FILE* backup_file = fopen(local_path, "wb");
                if (!backup_file) {
                    printf("[SS_THREAD %d] Failed to create file: %s\n", conn_fd, local_path);
                    send(conn_fd, "500 ERROR: Cannot create backup file", 37, 0);
                    close(primary_sock);
                    close(conn_fd);
                    return NULL;
                }
                
                printf("[SS_THREAD %d] Receiving file content from primary...\n", conn_fd);
                char chunk[4096];
                int bytes;
                int total_bytes = 0;
                
                // Read until connection closes (CLIENT_READ closes after sending)
                while ((bytes = recv(primary_sock, chunk, sizeof(chunk), 0)) > 0) {
                    fwrite(chunk, 1, bytes, backup_file);
                    total_bytes += bytes;
                    printf("[SS_THREAD %d] Received %d bytes (total: %d)\n", conn_fd, bytes, total_bytes);
                }
                
                fclose(backup_file);
                close(primary_sock);
                
                printf("[SS_THREAD %d] Replicated %s successfully to %s (%d bytes total)\n", 
                       conn_fd, source_path, local_path, total_bytes);
                
                // --- REPLICATE .undo FILE IF IT EXISTS ---
                char undo_source_path[600];
                snprintf(undo_source_path, sizeof(undo_source_path), "%s.undo", source_path);
                char undo_local_path[600];
                snprintf(undo_local_path, sizeof(undo_local_path), "%s.undo", local_path);
                
                // Connect to primary SS again to fetch .undo file
                int undo_sock = socket(AF_INET, SOCK_STREAM, 0);
                if (undo_sock >= 0) {
                    struct sockaddr_in undo_addr;
                    memset(&undo_addr, 0, sizeof(undo_addr));
                    undo_addr.sin_family = AF_INET;
                    undo_addr.sin_port = htons(primary_port);
                    inet_pton(AF_INET, primary_ip, &undo_addr.sin_addr);
                    
                    if (connect(undo_sock, (struct sockaddr *)&undo_addr, sizeof(undo_addr)) == 0) {
                        // Request .undo file from primary
                        char undo_fetch_cmd[1024];
                        sprintf(undo_fetch_cmd, "CLIENT_READ %s", undo_source_path);
                        send(undo_sock, undo_fetch_cmd, strlen(undo_fetch_cmd), 0);
                        
                        // Try to save .undo file
                        FILE* undo_backup_file = fopen(undo_local_path, "wb");
                        if (undo_backup_file) {
                            char undo_chunk[4096];
                            int undo_bytes;
                            int undo_total = 0;
                            
                            while ((undo_bytes = recv(undo_sock, undo_chunk, sizeof(undo_chunk), 0)) > 0) {
                                fwrite(undo_chunk, 1, undo_bytes, undo_backup_file);
                                undo_total += undo_bytes;
                            }
                            
                            fclose(undo_backup_file);
                            
                            if (undo_total > 0) {
                                printf("[SS_THREAD %d] Also replicated .undo file: %s (%d bytes)\n", 
                                       conn_fd, undo_local_path, undo_total);
                            } else {
                                // No undo file exists on primary (normal for new files)
                                remove(undo_local_path);
                            }
                        }
                    }
                    close(undo_sock);
                }
                
                if (total_bytes == 0) {
                    printf("[SS_THREAD %d] WARNING: File is empty or replication failed!\n", conn_fd);
                    send(conn_fd, "500 ERROR: Empty file received", 31, 0);
                } else {
                    send(conn_fd, "200 OK", 6, 0);
                }
                
                close(conn_fd);
                return NULL;
                
            } else {
                send(conn_fd, "400 ERROR: UNKNOWN", 18, 0);
            }

        } else if (bytes_read == 0) {
            printf("[SS_THREAD %d] Connection closed.\n", conn_fd);
            break;
        } else {
            perror("recv failed");
            break;
        }
    }

    close(conn_fd);
    printf("[SS_THREAD %d] Thread exiting.\n", conn_fd);
    fflush(stdout);
    return NULL;
}


// --- Helper Function: Scan data directory for existing files ---
void scan_data_directory(char* file_list, size_t max_len) {
    file_list[0] = '\0'; // Initialize as empty
    
    // Open the storage directory
    DIR* dir = opendir(STORAGE_DIR);
    if (dir == NULL) {
        perror("Failed to open storage directory");
        return;
    }
    
    struct dirent* entry;
    bool first = true;
    
    while ((entry = readdir(dir)) != NULL) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        // Skip .undo files (backup files)
        if (strstr(entry->d_name, ".undo") != NULL) {
            continue;
        }
        
        // Skip .tmp files
        if (strstr(entry->d_name, ".tmp") != NULL) {
            continue;
        }
        
        // Build full path
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", STORAGE_DIR, entry->d_name);
        
        // Add to list with delimiter
        if (!first) {
            strncat(file_list, "|", max_len - strlen(file_list) - 1);
        }
        strncat(file_list, full_path, max_len - strlen(file_list) - 1);
        first = false;
        
        printf("[SCAN] Found existing file: %s\n", full_path);
    }
    
    closedir(dir);
    
    if (strlen(file_list) == 0) {
        printf("[SCAN] No existing files found in %s/\n", STORAGE_DIR);
    } else {
        printf("[SCAN] File list to send: %s\n", file_list);
    }
}

// --- Helper Function: Register with NS ---
int register_with_ns(const char* ns_ip, int ns_port, int my_port) {
    int ss_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ss_fd == -1) {
        perror("socket failed");
        return -1;
    }

    struct sockaddr_in ns_address;
    memset(&ns_address, 0, sizeof(ns_address));
    ns_address.sin_family = AF_INET;
    ns_address.sin_port = htons(ns_port);

    if (inet_pton(AF_INET, ns_ip, &ns_address.sin_addr) <= 0) {
        perror("inet_pton failed");
        close(ss_fd);
        return -1;
    }

    if (connect(ss_fd, (struct sockaddr *)&ns_address, sizeof(ns_address)) < 0) {
        perror("connect to NS failed");
        close(ss_fd);
        return -1;
    }

    printf("Connected to Name Server for registration.\n");

    // Scan data directory for existing files
    char file_list[4096] = {0};
    scan_data_directory(file_list, sizeof(file_list));

    // Send REGISTER with IP, port, and file list
    char register_msg[8192];
    snprintf(register_msg, sizeof(register_msg), "REGISTER 127.0.0.1 %d %s", my_port, file_list);
    send(ss_fd, register_msg, strlen(register_msg), 0);

    char buffer[BUFFER_SIZE] = {0};
    if (recv(ss_fd, buffer, BUFFER_SIZE - 1, 0) <= 0) {
        perror("recv from NS failed");
        close(ss_fd);
        return -1;
    }
    
    close(ss_fd); 

    if (strncmp(buffer, "200 OK", 6) == 0) {
        printf("Registration successful!\n");
        return 0; // Success
    } else {
        printf("Registration failed: %s\n", buffer);
        return -1; // Failure
    }
}


// --- Main Function ---
int main(int argc, char* argv[]) {
    
    // Parse command-line arguments for port (optional)
    int my_port = MY_PORT; // Default port
    if (argc > 1) {
        my_port = atoi(argv[1]);
        if (my_port <= 0 || my_port > 65535) {
            fprintf(stderr, "Invalid port number. Using default port %d\n", MY_PORT);
            my_port = MY_PORT;
        }
    }
    
    printf("Storage Server will use port: %d\n", my_port);
    
    // Create port-specific storage directory (e.g., data_ss_9001, data_ss_9002)
    snprintf(STORAGE_DIR, sizeof(STORAGE_DIR), "data_ss_%d", my_port);
    if (mkdir(STORAGE_DIR, 0777) == 0) {
        printf("Created storage directory: %s/\n", STORAGE_DIR);
    } else {
        printf("Using existing storage directory: %s/\n", STORAGE_DIR);
    }
    
    // --- Init the lock system ---
    init_lock_table();
    
    // --- Part 1: Register with Name Server ---
    if (register_with_ns("127.0.0.1", NS_PORT, my_port) != 0) {
        fprintf(stderr, "Could not register with Name Server. Exiting.\n");
        exit(EXIT_FAILURE);
    }

    // --- Part 2: Start our own Server Loop ---
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("SS socket failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("SS setsockopt failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(my_port); // Listen on our port (from cmdline or default)

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("SS bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 10) < 0) {
        perror("SS listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Storage Server listening on port %d...\n", my_port);
    fflush(stdout);

    // Main loop to accept connections
    while (1) {
        int conn_fd = accept(server_fd, NULL, NULL);
        if (conn_fd < 0) {
            perror("SS accept failed");
            continue;
        }

        // Spawn a new thread to handle this connection
        pthread_t thread_id;
        int* p_conn_fd = malloc(sizeof(int));
        *p_conn_fd = conn_fd;

        if (pthread_create(&thread_id, NULL, handle_ss_connection, p_conn_fd) != 0) {
            perror("SS pthread_create failed");
            close(conn_fd);
            free(p_conn_fd);
        }

        pthread_detach(thread_id);
    }

    close(server_fd);
    return 0;
}