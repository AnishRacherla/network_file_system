# Makefile for Network File System
# Compiles nameserver, storageserver, and client

CC = gcc
CFLAGS = -Wall -Wextra -pthread -g
LDFLAGS = -pthread -lm

# Target executables
NAMESERVER = nameserver
STORAGESERVER = storageserver
CLIENT = client

# Source files
NAMESERVER_SRC = nameserver.c logger.c locks.c bonus_features.c
STORAGESERVER_SRC = storageserver.c locks.c
CLIENT_SRC = client.c

# Object files
NAMESERVER_OBJ = $(NAMESERVER_SRC:.c=.o)
STORAGESERVER_OBJ = $(STORAGESERVER_SRC:.c=.o)
CLIENT_OBJ = $(CLIENT_SRC:.c=.o)

# Header files
HEADERS = logger.h filemap.h locks.h

# Default target: build all
all: $(NAMESERVER) $(STORAGESERVER) $(CLIENT)

# Build nameserver
$(NAMESERVER): $(NAMESERVER_OBJ)
	@echo "Linking nameserver..."
	$(CC) $(LDFLAGS) -o $@ $^
	@echo "✓ Nameserver compiled successfully!"

# Build storageserver
$(STORAGESERVER): $(STORAGESERVER_OBJ)
	@echo "Linking storageserver..."
	$(CC) $(LDFLAGS) -o $@ $^
	@echo "✓ Storage Server compiled successfully!"

# Build client
$(CLIENT): $(CLIENT_OBJ)
	@echo "Linking client..."
	$(CC) $(LDFLAGS) -o $@ $^
	@echo "✓ Client compiled successfully!"

# Compile object files
%.o: %.c $(HEADERS)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	rm -f $(NAMESERVER) $(STORAGESERVER) $(CLIENT)
	rm -f *.o
	@echo "✓ Clean complete!"

# Clean everything including logs and data
cleanall: clean
	@echo "Cleaning logs and data directories..."
	rm -f nameserver.log
	rm -f nameserver_metadata.dat
	rm -rf data_ss_*/
	@echo "✓ Full clean complete!"

# Rebuild everything from scratch
rebuild: clean all

# Run nameserver (for testing)
run-ns:
	./$(NAMESERVER) 8080

# Run storage server 1 (for testing)
run-ss1:
	./$(STORAGESERVER) 127.0.0.1 9001 8080

# Run storage server 2 (for testing)
run-ss2:
	./$(STORAGESERVER) 127.0.0.1 9002 8080

# Run client (for testing)
run-client:
	./$(CLIENT) 127.0.0.1 8080

# Help target
help:
	@echo "Network File System - Makefile Help"
	@echo "===================================="
	@echo ""
	@echo "Available targets:"
	@echo "  make              - Build all components (nameserver, storageserver, client)"
	@echo "  make all          - Same as 'make'"
	@echo "  make nameserver   - Build only nameserver"
	@echo "  make storageserver - Build only storage server"
	@echo "  make client       - Build only client"
	@echo "  make clean        - Remove compiled binaries and object files"
	@echo "  make cleanall     - Remove binaries, objects, logs, and data directories"
	@echo "  make rebuild      - Clean and rebuild everything"
	@echo ""
	@echo "Testing targets:"
	@echo "  make run-ns       - Run nameserver on port 8080"
	@echo "  make run-ss1      - Run storage server 1 on port 9001"
	@echo "  make run-ss2      - Run storage server 2 on port 9002"
	@echo "  make run-client   - Run client connecting to nameserver"
	@echo ""
	@echo "Usage Example:"
	@echo "  Terminal 1: make run-ns"
	@echo "  Terminal 2: make run-ss1"
	@echo "  Terminal 3: make run-ss2"
	@echo "  Terminal 4: make run-client"

# Phony targets (not actual files)
.PHONY: all clean cleanall rebuild run-ns run-ss1 run-ss2 run-client help
