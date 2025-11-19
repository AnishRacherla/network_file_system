#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <termios.h>
#include <ctype.h>
#include <signal.h>

#define NS_PORT 8080
#define BUFFER_SIZE 32768  // Increased to match nameserver REPLY_BUFFER_SIZE
#define FILE_CHUNK_SIZE 4096
#define MAX_HISTORY 100
#define MAX_CMD_LEN 1024

// Command history structure
char command_history[MAX_HISTORY][MAX_CMD_LEN];
int history_count = 0;
int history_index = 0;

// Terminal settings
struct termios orig_termios;

// Signal handler for Ctrl+C
void handle_sigint(int sig) {
    (void)sig;
    printf("\n");
    disable_raw_mode();
    exit(0);
}

// Enable raw mode for terminal (to capture arrow keys)
void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON); // Disable echo and canonical mode
    raw.c_cc[VMIN] = 1;  // Minimum bytes to read
    raw.c_cc[VTIME] = 0; // Timeout (0 = wait indefinitely)
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

// Disable raw mode
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

// Add command to history (avoid duplicates of last command)
void add_to_history(const char* cmd) {
    if (strlen(cmd) == 0) return;
    
    // Don't add if it's the same as the last command
    if (history_count > 0 && strcmp(command_history[history_count - 1], cmd) == 0) {
        return;
    }
    
    // Add to history
    if (history_count < MAX_HISTORY) {
        strncpy(command_history[history_count], cmd, MAX_CMD_LEN - 1);
        command_history[history_count][MAX_CMD_LEN - 1] = '\0';
        history_count++;
    } else {
        // Shift history if full
        for (int i = 0; i < MAX_HISTORY - 1; i++) {
            strcpy(command_history[i], command_history[i + 1]);
        }
        strncpy(command_history[MAX_HISTORY - 1], cmd, MAX_CMD_LEN - 1);
        command_history[MAX_HISTORY - 1][MAX_CMD_LEN - 1] = '\0';
    }
}

// Read line with arrow key support for history navigation
int read_line_with_history(char* buffer, int max_len, const char* prompt) {
    int pos = 0;
    int temp_history_index = history_count;
    char temp_buffer[MAX_CMD_LEN] = {0};
    
    printf("%s", prompt);
    fflush(stdout);
    
    enable_raw_mode();
    
    while (1) {
        char c;
        if (read(STDIN_FILENO, &c, 1) != 1) break;
        
        // Handle escape sequences (arrow keys)
        if (c == 27) { // ESC
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) != 1) continue;
            if (read(STDIN_FILENO, &seq[1], 1) != 1) continue;
            
            if (seq[0] == '[') {
                if (seq[1] == 'A') { // Up arrow
                    if (temp_history_index > 0) {
                        // Save current input if at end of history
                        if (temp_history_index == history_count) {
                            strncpy(temp_buffer, buffer, pos);
                            temp_buffer[pos] = '\0';
                        }
                        
                        temp_history_index--;
                        
                        // Clear current line
                        printf("\r%s", prompt);
                        for (int i = 0; i < pos; i++) printf(" ");
                        printf("\r%s", prompt);
                        
                        // Copy history command
                        strcpy(buffer, command_history[temp_history_index]);
                        pos = strlen(buffer);
                        printf("%s", buffer);
                        fflush(stdout);
                    }
                } else if (seq[1] == 'B') { // Down arrow
                    if (temp_history_index < history_count) {
                        temp_history_index++;
                        
                        // Clear current line
                        printf("\r%s", prompt);
                        for (int i = 0; i < pos; i++) printf(" ");
                        printf("\r%s", prompt);
                        
                        if (temp_history_index == history_count) {
                            // Restore temporary buffer
                            strcpy(buffer, temp_buffer);
                        } else {
                            // Show next history command
                            strcpy(buffer, command_history[temp_history_index]);
                        }
                        pos = strlen(buffer);
                        printf("%s", buffer);
                        fflush(stdout);
                    }
                }
            }
            continue;
        }
        
        // Handle backspace
        if (c == 127 || c == '\b') {
            if (pos > 0) {
                pos--;
                buffer[pos] = '\0';
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }
        
        // Handle Enter
        if (c == '\n' || c == '\r') {
            buffer[pos] = '\0';
            printf("\n");
            disable_raw_mode();
            return pos;
        }
        
        // Handle Ctrl+C
        if (c == 3) {
            printf("^C\n");
            disable_raw_mode();
            return -1;
        }
        
        // Handle Ctrl+D
        if (c == 4) {
            if (pos == 0) {
                printf("\n");
                disable_raw_mode();
                return -1;
            }
            continue;
        }
        
        // Regular character
        if (isprint(c) && pos < max_len - 1) {
            buffer[pos++] = c;
            buffer[pos] = '\0';
            printf("%c", c);
            fflush(stdout);
        }
    }
    
    disable_raw_mode();
    return -1;
}

// --- HELPER FUNCTION: READ ---
// Handles the direct connection to the Storage Server for READ
void handle_direct_read(const char* ip, int port, const char* path, const char* filename, int ns_socket) {
    printf("--- Connecting to SS at %s:%d to read %s ---\n", ip, port, path);
    
    int temp_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (temp_sock < 0) {
        perror("Failed to create temp socket");
        return;
    }

    struct sockaddr_in ss_addr;
    memset(&ss_addr, 0, sizeof(ss_addr));
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &ss_addr.sin_addr) <= 0) {
        perror("inet_pton failed for SS");
        close(temp_sock);
        return;
    }

    if (connect(temp_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) {
        perror("Failed to connect to SS");
        close(temp_sock);
        return;
    }

    // Send the CLIENT_READ command to the SS
    char command[BUFFER_SIZE];
    snprintf(command, BUFFER_SIZE, "CLIENT_READ %s", path);
    send(temp_sock, command, strlen(command), 0);

    // Stream the file content back and collect for indexing
    char file_buffer[FILE_CHUNK_SIZE];
    char full_content[65536] = ""; // Buffer to collect full content
    int total_bytes = 0;
    int bytes_read;
    
    printf("--- Start of file ---\n");
    while ((bytes_read = recv(temp_sock, file_buffer, FILE_CHUNK_SIZE - 1, 0)) > 0) {
        file_buffer[bytes_read] = '\0'; // We assume text data
        printf("%s", file_buffer);
        
        // Collect content for indexing (if buffer has space)
        if (total_bytes + bytes_read < sizeof(full_content) - 1) {
            strcat(full_content, file_buffer);
            total_bytes += bytes_read;
        }
    }
    printf("\n--- End of file ---\n");

    close(temp_sock);
    
    // Automatically index the file content in nameserver
    if (total_bytes > 0) {
        char index_command[70000];
        snprintf(index_command, sizeof(index_command), "INDEX %s %s", filename, full_content);
        send(ns_socket, index_command, strlen(index_command), 0);
        
        // Receive indexing confirmation
        char index_reply[256];
        int reply_bytes = recv(ns_socket, index_reply, sizeof(index_reply) - 1, 0);
        if (reply_bytes > 0) {
            index_reply[reply_bytes] = '\0';
            // Silently indexed - don't print the reply
        }
    }
}

// --- HELPER FUNCTION: STREAM ---
// Handles word-by-word streaming with 0.1s delay
void handle_direct_stream(const char* ip, int port, const char* path) {
    printf("--- Streaming file word-by-word (0.1s delay) ---\n");
    
    int temp_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (temp_sock < 0) {
        perror("Failed to create temp socket");
        return;
    }

    struct sockaddr_in ss_addr;
    memset(&ss_addr, 0, sizeof(ss_addr));
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &ss_addr.sin_addr) <= 0) {
        perror("inet_pton failed for SS");
        close(temp_sock);
        return;
    }

    if (connect(temp_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) {
        perror("Failed to connect to SS");
        close(temp_sock);
        return;
    }

    // Send the CLIENT_STREAM command
    char command[BUFFER_SIZE];
    snprintf(command, BUFFER_SIZE, "CLIENT_STREAM %s", path);
    send(temp_sock, command, strlen(command), 0);

    // Receive and display words with delay
    char word_buffer[256];
    int bytes_read;
    while ((bytes_read = recv(temp_sock, word_buffer, sizeof(word_buffer) - 1, 0)) > 0) {
        word_buffer[bytes_read] = '\0';
        
        if (strcmp(word_buffer, "|END|") == 0) break; // End marker
        
        printf("%s ", word_buffer);
        fflush(stdout);
        usleep(100000); // 0.1 second delay
    }
    printf("\n--- End of stream ---\n");

    close(temp_sock);
}


// --- HELPER FUNCTION: WRITE ---
// Handles the direct connection to the Storage Server for WRITE
void handle_direct_write(const char* ip, int port, const char* path, int sentence_num, const char* username, const char* filename, int ns_socket) {
    printf("--- Connecting to SS at %s:%d to write %s ---\n", ip, port, path);
    
    int temp_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (temp_sock < 0) {
        perror("Failed to create temp socket");
        return;
    }

    struct sockaddr_in ss_addr;
    memset(&ss_addr, 0, sizeof(ss_addr));
    ss_addr.sin_family = AF_INET;
    ss_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &ss_addr.sin_addr) <= 0) {
        perror("inet_pton failed for SS");
        close(temp_sock);
        return;
    }

    if (connect(temp_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) < 0) {
        perror("Failed to connect to SS for WRITE");
        close(temp_sock);
        return;
    }

    // 1. Send the CLIENT_WRITE_LOCK command to the SS
    char command[BUFFER_SIZE];
    snprintf(command, BUFFER_SIZE, "CLIENT_WRITE_LOCK %s %d %s", path, sentence_num, username);
    send(temp_sock, command, strlen(command), 0);

    // 2. Wait for the "OK: LOCKED" reply
    char reply[BUFFER_SIZE] = {0};
    int bytes_read = recv(temp_sock, reply, BUFFER_SIZE - 1, 0);
    reply[bytes_read] = '\0';

    if (strncmp(reply, "200 OK: LOCKED", 14) != 0) {
        printf("Failed to get lock: %s\n", reply);
        close(temp_sock);
        return;
    }

    // 3. We have the lock! Enter the write loop
    printf("Lock acquired. Enter word updates in format: <word_index> <new_word>\n");
   // printf("Example: '1 Hello' will replace word at index 1 with 'Hello'\n");
    printf("Type 'ETIRW' (WRITE backwards) when done, or empty line to finish.\n");
    char user_input[BUFFER_SIZE];
    while(1) {
        printf("(write)$ ");
        fflush(stdout);

        if (fgets(user_input, BUFFER_SIZE, stdin) == NULL) { break; }
        user_input[strcspn(user_input, "\n")] = 0;

        // Empty line = finish writing
        if(strlen(user_input) == 0) {
            strcpy(user_input, "ETIRW");
        }

        // Send this line to the SS
        send(temp_sock, user_input, strlen(user_input), 0);

        if (strcmp(user_input, "ETIRW") == 0) {
            break; // Exit loop, time to get final confirmation
        }
        // (A more robust client would wait for an "OK" for each word)
    }

    // 4. Wait for the final "WRITE COMPLETE"
    memset(reply, 0, BUFFER_SIZE);
    bytes_read = recv(temp_sock, reply, BUFFER_SIZE - 1, 0);
    reply[bytes_read] = '\0';
    
    printf("Server replied: %s\n", reply);
    close(temp_sock);
    
    // 5. After successful write, re-read the file to update the search index
    if (strstr(reply, "WRITE COMPLETE") != NULL) {
        // Re-read the file content for indexing
        int read_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (read_sock >= 0) {
            if (connect(read_sock, (struct sockaddr *)&ss_addr, sizeof(ss_addr)) == 0) {
                // Send CLIENT_READ command
                char read_cmd[BUFFER_SIZE];
                snprintf(read_cmd, BUFFER_SIZE, "CLIENT_READ %s", path);
                send(read_sock, read_cmd, strlen(read_cmd), 0);
                
                // Collect file content
                char file_buffer[FILE_CHUNK_SIZE];
                char full_content[65536] = "";
                int total_bytes = 0;
                
                while ((bytes_read = recv(read_sock, file_buffer, FILE_CHUNK_SIZE - 1, 0)) > 0) {
                    file_buffer[bytes_read] = '\0';
                    if (total_bytes + bytes_read < sizeof(full_content) - 1) {
                        strcat(full_content, file_buffer);
                        total_bytes += bytes_read;
                    }
                }
                close(read_sock);
                
                // Send to nameserver for indexing
                if (total_bytes > 0) {
                    char index_command[70000];
                    snprintf(index_command, sizeof(index_command), "INDEX %s %s", filename, full_content);
                    send(ns_socket, index_command, strlen(index_command), 0);
                    
                    // Receive indexing confirmation
                    char index_reply[256];
                    int reply_bytes = recv(ns_socket, index_reply, sizeof(index_reply) - 1, 0);
                    if (reply_bytes > 0) {
                        index_reply[reply_bytes] = '\0';
                        // Silently indexed
                    }
                }
            } else {
                close(read_sock);
            }
        }
    }
}


// --- Main Function ---
int main(void) {
    // Initialize terminal settings
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode); // Ensure terminal is restored on any exit
    signal(SIGINT, handle_sigint); // Handle Ctrl+C gracefully
    
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd == -1) {
        perror("socket failed"); exit(EXIT_FAILURE);
    }

    struct sockaddr_in server_address;
    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(NS_PORT);

    if (inet_pton(AF_INET, "127.0.0.1", &server_address.sin_addr) <= 0) {
        perror("inet_pton failed");
        close(client_fd);
        exit(EXIT_FAILURE);
    }

    if (connect(client_fd, (struct sockaddr *)&server_address, sizeof(server_address)) < 0) {
        perror("connect failed");
        close(client_fd);
        exit(EXIT_FAILURE);
    }

    // --- Handshake Step ---
    printf("Connected to server. \nEnter your username: ");
    char username[100];
    if (fgets(username, 100, stdin) == NULL) {
        printf("Error reading username.\n");
        close(client_fd);
        exit(EXIT_FAILURE);
    }
    username[strcspn(username, "\n")] = 0; // Remove newline

    char connect_msg[BUFFER_SIZE];
    snprintf(connect_msg, BUFFER_SIZE, "CONNECT %s", username);
    
    send(client_fd, connect_msg, strlen(connect_msg), 0);
    
    // Wait for OK
    char server_reply[BUFFER_SIZE] = {0};
    int bytes_read = recv(client_fd, server_reply, BUFFER_SIZE - 1, 0);
    
    if (bytes_read > 0 && strncmp(server_reply, "200 OK", 6) == 0) {
         printf("Successfully logged in as %s. Type 'exit' to quit.\n", username);
         
         // Check for login notifications (non-blocking)
         struct timeval tv;
         tv.tv_sec = 0;
         tv.tv_usec = 200000; // 200ms timeout for notifications
         setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
         
         char notif_buffer[BUFFER_SIZE];
         int notif_bytes = recv(client_fd, notif_buffer, BUFFER_SIZE - 1, 0);
         if (notif_bytes > 0) {
             notif_buffer[notif_bytes] = '\0';
             // Display login notifications
             printf("%s", notif_buffer);
         }
         
         // Reset to blocking mode
         tv.tv_sec = 0;
         tv.tv_usec = 0;
         setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    } else {
         printf("Login failed: %s\n", server_reply);
         close(client_fd);
         exit(EXIT_FAILURE);
    }
    // --- End Handshake ---


    // --- Main Client Loop ---
    char user_input[BUFFER_SIZE];
    while (1) {
        // Check for pending notifications before showing prompt (non-blocking)
        struct timeval tv_prompt;
        tv_prompt.tv_sec = 0;
        tv_prompt.tv_usec = 50000; // 50ms timeout
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv_prompt, sizeof(tv_prompt));
        
        char prompt_notif[BUFFER_SIZE];
        int prompt_notif_bytes = recv(client_fd, prompt_notif, BUFFER_SIZE - 1, 0);
        if (prompt_notif_bytes > 0) {
            prompt_notif[prompt_notif_bytes] = '\0';
            // Print if it's a notification
            if (strstr(prompt_notif, "[NOTIFICATION]") || strstr(prompt_notif, "[INFO]")) {
                printf("%s", prompt_notif);
            }
        }
        
        // Reset to blocking mode
        tv_prompt.tv_sec = 0;
        tv_prompt.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv_prompt, sizeof(tv_prompt));
        
        char prompt[150];
        snprintf(prompt, sizeof(prompt), "(%s)$ ", username);
        
        // Use history-enabled input
        int len = read_line_with_history(user_input, BUFFER_SIZE, prompt);
        
        if (len < 0) {
            printf("Disconnecting.\n");
            break; // Ctrl+C or Ctrl+D
        }
        
        if (strlen(user_input) == 0) continue; // Skip empty input
        
        // Add to history (will skip duplicates automatically)
        add_to_history(user_input);

        if (send(client_fd, user_input, strlen(user_input), 0) < 0) {
            perror("send failed");
            break;
        }

        if (strcmp(user_input, "exit") == 0) {
            break; // We sent "exit", now we quit
        }
        
        if (strcmp(user_input, "help") == 0 || strcmp(user_input, "HELP") == 0) {
            printf("\n=== AVAILABLE COMMANDS ===\n\n");
            printf("File Operations:\n");
            printf("  VIEW                          - List all files\n");
            printf("  VIEW -al                      - List all files with detailed information\n");
            printf("  CREATE <filename>             - Create a new file\n");
            printf("  DELETE <filename>             - Delete a file\n");
            printf("  INFO <filename>               - Show file metadata\n");
            printf("  READ <filename>               - Read file contents\n");
            printf("  WRITE <filename> <sentence#>  - Edit a specific sentence (0-based index)\n");
            printf("  STREAM <filename>             - Stream file word-by-word with delays\n");
            printf("\nFolder Operations:\n");
            printf("  CREATEFOLDER <foldername>     - Create a new folder\n");
            printf("  DELETEFOLDER <foldername>     - Delete an empty folder\n");
            printf("\nAdvanced Operations:\n");
            printf("  APPEND <filename> <text>      - Append text to end of file\n");
            printf("  MOVE <old_path> <new_path>    - Move/rename a file or folder\n");
            printf("  UNDO <filename>               - Undo last write operation\n");
            printf("  REDO <filename>               - Redo previously undone operation\n");
            printf("\nCheckpoint Operations:\n");
            printf("  CHECKPOINT <filename> <tag>   - Save a checkpoint with a tag\n");
            printf("  VIEWCHECKPOINT <filename> <tag> - View a saved checkpoint\n");
            printf("  REVERT <filename> <tag>       - Revert file to a checkpoint\n");
            printf("  LISTCHECKPOINTS <filename>    - List all checkpoints for a file\n");
            printf("\nAccess Control:\n");
            printf("  APPROVE  <index>   - Grant access to another user\n");
            printf("  DENY/REJECT  <index>   - REJECT access to another user\n");
            printf("  LISTACCESS <filename>         - List all users with access\n");
            printf("\nSearch:\n");
            printf("  SEARCH <keyword>              - Search for keyword in all files\n");
            printf("\nWrite Operation Details:\n");
            printf("  - Words are 0-indexed (first word is at index 0)\n");
            printf("  - Insert at index N means insert BEFORE current word at position N\n");
            printf("  - Use \\n in word text to insert newlines (e.g., '0 HELLO \\n WORLD')\n");
            printf("  - Type 'ETIRW' (WRITE backwards) when done editing\n");
            printf("  - Empty line also finishes the write operation\n");
            printf("\nOther:\n");
            printf("  help                          - Show this help message\n");
            printf("  exit                          - Quit the client\n");
            printf("\n=========================\n\n");
            continue; // Skip sending to server
        }

        // --- NEW: Smart Reply Handling with multi-chunk support ---
        memset(server_reply, 0, BUFFER_SIZE);
        bytes_read = recv(client_fd, server_reply, BUFFER_SIZE - 1, 0);
        
        if (bytes_read > 0) {
            server_reply[bytes_read] = '\0';
            
            // For large responses (like VIEW -al), check if more data is available
            // Use a small delay to allow server to send all chunks
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 50000; // 50ms timeout
            setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            
            // Try to read additional chunks if buffer was nearly full
            while (bytes_read == BUFFER_SIZE - 1) {
                int additional_bytes = recv(client_fd, server_reply + bytes_read, 
                                           BUFFER_SIZE - bytes_read - 1, 0);
                if (additional_bytes > 0) {
                    bytes_read += additional_bytes;
                    server_reply[bytes_read] = '\0';
                } else {
                    break; // No more data or timeout
                }
            }
            
            // Reset to blocking mode
            tv.tv_sec = 0;
            tv.tv_usec = 0;
            setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            
            // Check if this is a "READ" reply
            if (strncmp(server_reply, "200 OK ", 7) == 0) {
                // This is a "matchmaker" reply
                char status[10], ok[10], ip[100], path[512];
                int port;
                sscanf(server_reply, "%s %s %s %d %s", status, ok, ip, &port, path);
                
                // Extract filename from original command
                char cmd[10], filename[256];
                sscanf(user_input, "%s %s", cmd, filename);
                
                // Call the helper function to handle the direct connection
                handle_direct_read(ip, port, path, filename, client_fd);

            } else if (strncmp(server_reply, "400 STREAM_OK ", 14) == 0) {
                // --- Handle STREAM reply ---
                char status[10], ok[20], ip[100], path[512];
                int port;
                sscanf(server_reply, "%s %s %s %d %s", status, ok, ip, &port, path);
                
                // Call streaming function
                handle_direct_stream(ip, port, path);

            } else if (strncmp(server_reply, "200 EXEC_OUTPUT:", 16) == 0) {
                // --- Handle EXEC reply: execution output from nameserver ---
                printf("--- Command Output ---\n");
                printf("%s\n", server_reply + 17); // Skip "200 EXEC_OUTPUT:\n"
                printf("--- End of Output ---\n");

            } else if (strncmp(server_reply, "200 SEARCH_RESULTS", 18) == 0) {
                // --- Handle SEARCH reply: display results ---
                printf("--- Search Results ---\n");
                
                // Parse and display results line by line
                char* line = server_reply + 19; // Skip "200 SEARCH_RESULTS\n"
                char* newline;
                
                while ((newline = strchr(line, '\n')) != NULL) {
                    *newline = '\0';
                    if (strlen(line) > 0) {
                        // Parse filename:sentence_number
                        char filename[256];
                        int sentence_num;
                        if (sscanf(line, "%[^:]:%d", filename, &sentence_num) == 2) {
                            printf("  File: %s, Sentence: %d\n", filename, sentence_num);
                        }
                    }
                    line = newline + 1;
                }
                
                printf("--- End of Results ---\n");

            } else if (strncmp(server_reply, "300 WRITE_OK ", 13) == 0) {
                // --- Handle WRITE reply ---
                char status[10], ok[10], ip[100], path[512], user_from_ns[100];
                int port;
                sscanf(server_reply, "%s %s %s %d %s %s", status, ok, ip, &port, path, user_from_ns);

                // We also need the original sentence number from the user's command
                char original_command[BUFFER_SIZE];
                strcpy(original_command, user_input); // user_input still has "WRITE..."
                
                char cmd[10], file[100];
                int sent_num;
                
                // Check if user provided sentence number
                if (sscanf(original_command, "%s %s %d", cmd, file, &sent_num) != 3) {
                    printf("Error: WRITE command requires sentence number.\n");
                    printf("Usage: WRITE <filename> <sentence_number>\n");
                  //  printf("Example: WRITE myfile.txt 0\n");
                  //  printf("Hint: Use READ first to see file content and count sentences.\n");
                  //  printf("      Sentences are separated by . ! or ?\n");
                    continue;
                }

                // Pass our local username to the write handler (with filename and ns socket for indexing)
                handle_direct_write(ip, port, path, sent_num, username, file, client_fd);

            } else {
                // This is a normal reply (from CREATE, VIEW, or an error)
                printf("Server: %s\n", server_reply);
            }
            
            // Check for additional notifications after each command
            // Send a quick peek for notifications (non-blocking)
            struct timeval tv_notif;
            tv_notif.tv_sec = 0;
            tv_notif.tv_usec = 100000; // 100ms timeout
            setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv_notif, sizeof(tv_notif));
            
            char notif_buffer[BUFFER_SIZE];
            int notif_bytes = recv(client_fd, notif_buffer, BUFFER_SIZE - 1, 0);
            if (notif_bytes > 0) {
                notif_buffer[notif_bytes] = '\0';
                // Print notification if it's actually a notification message
                if (strstr(notif_buffer, "[NOTIFICATION]") || strstr(notif_buffer, "[INFO]")) {
                    printf("%s", notif_buffer);
                }
            }
            
            // Reset to blocking mode
            tv_notif.tv_sec = 0;
            tv_notif.tv_usec = 0;
            setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv_notif, sizeof(tv_notif));
            
        } else {
            printf("Server closed the connection.\n");
            break;
        }
    }

    printf("Disconnecting...\n");
    disable_raw_mode(); // Ensure terminal is restored
    close(client_fd);
    return 0;
}