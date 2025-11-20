# Distributed Network File System (NFS)

A scalable, fault-tolerant distributed file system implementation with hierarchical folder structures, access control, checkpoints, and real-time collaboration features.

## 📋 Table of Contents
- [Overview](#overview)
- [Architecture](#architecture)
- [Features](#features)
- [Installation](#installation)
- [Usage](#usage)
- [Command Reference](#command-reference)
- [System Components](#system-components)
- [Advanced Features](#advanced-features)
- [Development](#development)

---

## 🎯 Overview

This Network File System (NFS) is a distributed storage solution that enables multiple clients to access, modify, and collaborate on files stored across multiple storage servers. The system is coordinated by a central nameserver that manages metadata, access control, and file operations.

### Key Highlights
- **Distributed Architecture**: Scalable design with multiple storage servers
- **Fault Tolerance**: Automatic replication and failover mechanisms
- **Access Control**: Fine-grained permissions with read/write access levels
- **Real-time Collaboration**: Live notifications for file modifications and access requests
- **Version Control**: Checkpoint system with undo/redo capabilities
- **Advanced Search**: Full-text search across all files
- **Performance Optimized**: LRU caching and hashmap-based file lookups

---

## 🏗️ Architecture

```
┌─────────────┐
│   Client 1  │◄──────┐
└─────────────┘       │
                      │
┌─────────────┐       │      ┌──────────────────┐
│   Client 2  │◄──────┼──────│   Name Server    │
└─────────────┘       │      │  (Port 8080)     │
                      │      │                  │
┌─────────────┐       │      │ - Metadata Mgmt  │
│   Client N  │◄──────┘      │ - Access Control │
└─────────────┘              │ - File Discovery │
                             │ - Replication    │
                             └────────┬─────────┘
                                      │
                      ┌───────────────┼───────────────┐
                      │               │               │
              ┌───────▼──────┐ ┌─────▼──────┐ ┌─────▼──────┐
              │ Storage Srv 1│ │Storage Srv 2│ │Storage Srv N│
              │  (Port 9001) │ │ (Port 9002) │ │ (Port 900N)│
              │              │ │             │ │            │
              │ - File Store │ │- Replication│ │- Streaming │
              │ - Locking    │ │- Backup     │ │- Operations│
              └──────────────┘ └─────────────┘ └────────────┘
```

### Component Roles
1. **Name Server**: Metadata management, file routing, access control, search indexing
2. **Storage Servers**: File storage, read/write operations, streaming, replication
3. **Clients**: User interface for file operations with command history and notifications

---

## ✨ Features

### Core Functionality
- ✅ **File Operations**: Create, read, write, delete, append
- ✅ **Folder Operations**: Hierarchical folder structure with create/delete
- ✅ **Metadata Management**: File size, word count, timestamps, ownership
- ✅ **Concurrent Access**: Thread-safe operations with file locking

### Advanced Features
- 🔒 **Access Control Lists (ACL)**: Owner-based permissions with read/write granularity
- 📸 **Checkpoints**: Save and revert to tagged file versions
- ↩️ **Undo/Redo**: Version history for write operations
- 🔍 **Full-Text Search**: Fast keyword search across all files
- 📡 **Real-Time Notifications**: Live updates for file changes and access requests
- 🔄 **Replication**: Automatic backup to multiple storage servers
- 💾 **Fault Tolerance**: Automatic failover on storage server failure
- 📊 **Command History**: Arrow key navigation through previous commands

### Performance Optimizations
- ⚡ **LRU Cache**: Fast file metadata lookups
- 🗺️ **HashMap Indexing**: O(1) file discovery
- 🔐 **Fine-Grained Locking**: Sentence-level write locks for concurrent editing
- 📝 **Logging System**: Thread-safe operation logging

---

## 🛠️ Installation

### Prerequisites
- **OS**: Linux/Unix-based system (uses POSIX sockets)
- **Compiler**: GCC with C11 support
- **Libraries**: pthread, math library

### Build Instructions

```bash
# Clone the repository
git clone <repository-url>
cd course-project-last-laugh

# Build all components
make

# Or build individually
make nameserver
make storageserver
make client

# Clean build artifacts
make clean

# Full clean (including logs and data)
make cleanall
```

---

## 🚀 Usage

### Starting the System

**Terminal 1 - Start Name Server:**
```bash
./nameserver 8080
```

**Terminal 2 - Start Storage Server 1:**
```bash
./storageserver 127.0.0.1 9001 8080
```

**Terminal 3 - Start Storage Server 2 (optional for replication):**
```bash
./storageserver 127.0.0.1 9002 8080
```

**Terminal 4 - Start Client:**
```bash
./client 127.0.0.1 8080
```

### Using Makefile Shortcuts

```bash
# Run name server
make run-ns

# Run storage servers
make run-ss1
make run-ss2

# Run client
make run-client
```

---

## 📖 Command Reference

### File Operations

| Command | Description | Example |
|---------|-------------|---------|
| `VIEW` | List all files | `VIEW` |
| `VIEW -al` | List files with details | `VIEW -al` |
| `CREATE <file>` | Create new file | `CREATE notes.txt` |
| `DELETE <file>` | Delete file | `DELETE notes.txt` |
| `INFO <file>` | Show metadata | `INFO notes.txt` |
| `READ <file>` | Read file contents | `READ notes.txt` |
| `WRITE <file> <sent#>` | Edit sentence | `WRITE notes.txt 0` |
| `APPEND <file> <text>` | Append to file | `APPEND notes.txt Hello World` |
| `STREAM <file>` | Stream word-by-word | `STREAM notes.txt` |

### Folder Operations

| Command | Description | Example |
|---------|-------------|---------|
| `CREATEFOLDER <name>` | Create folder | `CREATEFOLDER documents` |
| `DELETEFOLDER <name>` | Delete empty folder | `DELETEFOLDER old_docs` |

### Version Control

| Command | Description | Example |
|---------|-------------|---------|
| `UNDO <file>` | Undo last write | `UNDO notes.txt` |
| `REDO <file>` | Redo undone write | `REDO notes.txt` |
| `CHECKPOINT <file> <tag>` | Save checkpoint | `CHECKPOINT notes.txt v1.0` |
| `VIEWCHECKPOINT <file> <tag>` | View checkpoint | `VIEWCHECKPOINT notes.txt v1.0` |
| `REVERT <file> <tag>` | Revert to checkpoint | `REVERT notes.txt v1.0` |
| `LISTCHECKPOINTS <file>` | List all checkpoints | `LISTCHECKPOINTS notes.txt` |

### Access Control

| Command | Description | Example |
|---------|-------------|---------|
| `APPROVE <index>` | Grant access request | `APPROVE 0` |
| `DENY <index>` | Reject access request | `DENY 1` |
| `LISTACCESS <file>` | Show ACL entries | `LISTACCESS notes.txt` |

### Advanced Operations

| Command | Description | Example |
|---------|-------------|---------|
| `SEARCH <keyword>` | Search in all files | `SEARCH important` |
| `MOVE <old> <new>` | Move/rename file | `MOVE file.txt docs/file.txt` |
| `help` | Show help message | `help` |
| `exit` | Disconnect client | `exit` |

### Write Operation Details

When using `WRITE <file> <sentence#>`:
1. Words are 0-indexed (first word = index 0)
2. Format: `<word_index> <new_word>`
3. Insert before position N: `INSERT <N> <word>`
4. Support newlines: Use `\n` in text
5. Finish editing: Type `ETIRW` (WRITE backwards) or press Enter

**Example:**
```
(user)$ WRITE notes.txt 0
Lock acquired. Enter word updates...
(write)$ 0 Hello
(write)$ 1 World
(write)$ INSERT 2 Beautiful
(write)$ ETIRW
Server replied: 200 OK: WRITE COMPLETE
```

---

## 🔧 System Components

### Name Server (`nameserver.c`)

**Responsibilities:**
- File metadata management and routing
- Access control list (ACL) administration
- Search indexing and query processing
- Replication coordination
- User authentication and session management
- Real-time notification delivery

**Data Structures:**
- HashMap for O(1) file lookups
- LRU cache for recent file access
- Access request queue
- Active client tracking for notifications

### Storage Server (`storageserver.c`)

**Responsibilities:**
- Physical file storage and retrieval
- Read/write operation execution
- Sentence-level file locking
- File streaming with delays
- Replication synchronization
- Metadata reporting to nameserver

**Features:**
- Sentence parsing with delimiter preservation
- Word-by-word streaming (0.1s delay)
- Automatic metadata calculation (word/char count)
- Thread-safe lock management

### Client (`client.c`)

**Responsibilities:**
- User interface and command processing
- Direct storage server connections for I/O
- Command history with arrow key navigation
- Real-time notification display
- Automatic search index updates

**Features:**
- Raw terminal mode for better UX
- Command history (up/down arrows)
- Multi-chunk server response handling
- Non-blocking notification checks
- Graceful Ctrl+C/Ctrl+D handling

---

## 🎓 Advanced Features

### 1. Hierarchical Folder System
- Nested folder structure (e.g., `/documents/reports/2024/`)
- Path normalization and validation
- Recursive folder operations

### 2. Checkpoint System
- Tag-based version snapshots
- View historical versions without reverting
- Multiple checkpoints per file (max 20)

### 3. Access Request Workflow
```
1. User requests access to file → SEARCH or READ command
2. Owner receives notification
3. Owner approves/denies → APPROVE/DENY command
4. Requester gets notification of decision
5. Access granted → User added to ACL
```

### 4. Fault Tolerance
- **Replication**: Files copied to backup storage servers
- **Failover**: Automatic switch to replica on primary failure
- **Heartbeat**: Storage server health monitoring
- **Recovery**: Metadata persistence across nameserver restarts

### 5. Search Engine
- Inverted index for fast keyword lookup
- Sentence-level granularity
- Automatic index updates on file writes
- Returns file + sentence number

### 6. Locking Mechanism
- File-level and sentence-level locks
- Username-based lock ownership
- Deadlock prevention
- Lock timeout handling

---

## 👨‍💻 Development

### Project Structure

```
course-project-last-laugh/
├── nameserver.c          # Name server implementation
├── storageserver.c       # Storage server implementation
├── client.c              # Client application
├── bonus_features.c      # Advanced feature implementations
├── bonus_features.h      # Feature declarations
├── logger.c              # Logging system
├── logger.h              # Logger interface
├── locks.c               # Locking mechanisms
├── locks.h               # Lock interface
├── filemap.c            # File mapping utilities
├── filemap.h            # Filemap interface
├── Makefile             # Build configuration
└── README.md            # This file
```

### Configuration

**Name Server:**
- Default port: 8080
- Max storage servers: 10
- Max files: 1000
- Max users: 1000

**Storage Server:**
- Default ports: 9001, 9002, ...
- Storage directory: `data_ss_<port>/`
- Max concurrent locks: 1000

**Client:**
- Default nameserver: 127.0.0.1:8080
- Command history: 100 commands
- Buffer size: 32KB

### Logging

System logs are stored in:
- `nameserver.log` - All nameserver operations
- Storage server logs - Console output

### Troubleshooting

**Port already in use:**
```bash
# Find process using port
lsof -i :8080

# Kill process
kill -9 <PID>
```

**Storage server won't connect:**
- Ensure nameserver is running first
- Check firewall settings
- Verify IP address configuration

**Client can't authenticate:**
- Restart nameserver to clear sessions
- Check network connectivity
- Verify nameserver is accepting connections

---

## 📊 Performance

- **File Lookup**: O(1) with hashmap + LRU cache
- **Search**: O(k) where k = total sentences in system
- **Concurrent Clients**: Supports 1000+ simultaneous users
- **Replication**: Automatic with minimal overhead
- **Locking**: Fine-grained, sentence-level concurrency

---

## 🤝 Contributing

This project was developed as a course assignment. For questions or improvements, please contact the development team.

---

## 📝 License

Educational project - All rights reserved.

---

## 👥 Authors

Developed by: Last Laugh Team
Course: Operating Systems and Networks
Institution: [Your Institution]

---

## 🙏 Acknowledgments

- POSIX thread library for concurrent programming
- Linux networking stack for socket operations
- Course instructors for project guidance

---

**Note**: This system is designed for educational purposes and local network deployment. For production use, additional security measures (encryption, authentication tokens, etc.) should be implemented.


ASSUMPTION:
when multiple user are writing and one of them changes the sentence count(by adding delimeter), the indexing changes,so the second person write wont take place like what it should .(this is not handled in our project).