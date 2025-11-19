#ifndef LOGGER_H
#define LOGGER_H

#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

// Log levels
typedef enum {
    LOG_INFO,
    LOG_WARNING,
    LOG_ERROR,
    LOG_DEBUG
} LogLevel;

// Initialize logging system
void init_logger(const char* log_filename);

// Close logging system
void close_logger();

// Thread-safe logging function
void log_event(LogLevel level, const char* client_ip, int client_port, 
               const char* username, const char* format, ...);

// Simpler version without network info
void log_message(LogLevel level, const char* format, ...);

#endif // LOGGER_H
