#include "logger.h"
#include <stdlib.h>

static FILE* log_file = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

// Initialize logging system
void init_logger(const char* log_filename) {
    pthread_mutex_lock(&log_mutex);
    if (log_file != NULL) {
        fclose(log_file);
    }
    log_file = fopen(log_filename, "a"); // Append mode
    if (log_file == NULL) {
        perror("Failed to open log file");
    } else {
        // Write header
        fprintf(log_file, "\n========== Log Session Started ==========\n");
        fflush(log_file);
    }
    pthread_mutex_unlock(&log_mutex);
}

// Close logging system
void close_logger() {
    pthread_mutex_lock(&log_mutex);
    if (log_file != NULL) {
        fprintf(log_file, "========== Log Session Ended ==========\n\n");
        fclose(log_file);
        log_file = NULL;
    }
    pthread_mutex_unlock(&log_mutex);
}

// Get current timestamp
static void get_timestamp(char* buffer, size_t size) {
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", tm_info);
}

// Get log level string
static const char* log_level_string(LogLevel level) {
    switch(level) {
        case LOG_INFO: return "INFO";
        case LOG_WARNING: return "WARN";
        case LOG_ERROR: return "ERROR";
        case LOG_DEBUG: return "DEBUG";
        default: return "UNKNOWN";
    }
}

// Thread-safe logging function with network info
void log_event(LogLevel level, const char* client_ip, int client_port, 
               const char* username, const char* format, ...) {
    pthread_mutex_lock(&log_mutex);
    
    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));
    
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    
    // Format: [timestamp] [LEVEL] [IP:Port] [User: username] message
    char log_line[2048];
    if (client_ip != NULL && username != NULL) {
        snprintf(log_line, sizeof(log_line), 
                "[%s] [%s] [%s:%d] [User: %s] %s\n",
                timestamp, log_level_string(level), client_ip, client_port, 
                username, message);
    } else if (client_ip != NULL) {
        snprintf(log_line, sizeof(log_line), 
                "[%s] [%s] [%s:%d] %s\n",
                timestamp, log_level_string(level), client_ip, client_port, 
                message);
    } else if (username != NULL) {
        snprintf(log_line, sizeof(log_line), 
                "[%s] [%s] [User: %s] %s\n",
                timestamp, log_level_string(level), username, message);
    } else {
        snprintf(log_line, sizeof(log_line), 
                "[%s] [%s] %s\n",
                timestamp, log_level_string(level), message);
    }
    
    // Write to console
    printf("%s", log_line);
    
    // Write to file
    if (log_file != NULL) {
        fprintf(log_file, "%s", log_line);
        fflush(log_file);
    }
    
    pthread_mutex_unlock(&log_mutex);
}

// Simpler version without network info
void log_message(LogLevel level, const char* format, ...) {
    pthread_mutex_lock(&log_mutex);
    
    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));
    
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    
    char log_line[2048];
    snprintf(log_line, sizeof(log_line), "[%s] [%s] %s\n",
            timestamp, log_level_string(level), message);
    
    // Write to console
    printf("%s", log_line);
    
    // Write to file
    if (log_file != NULL) {
        fprintf(log_file, "%s", log_line);
        fflush(log_file);
    }
    
    pthread_mutex_unlock(&log_mutex);
}
