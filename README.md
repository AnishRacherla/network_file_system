# Distributed Network File System (NFS)

A scalable, fault-tolerant distributed file system implementation with hierarchical folder structures, access control, checkpoints, and real-time collaboration features.

## 📋 Table of Contents
- [Overview](#overview)
- [Architecture](#architecture)
- [Features](#features)
- [Installation](#installation)
- [Quick Start](#quick-start)
- [Usage](#usage)
- [Command Reference](#command-reference)
- [System Components](#system-components)
- [Advanced Features](#advanced-features)
- [Development](#development)

## 📚 Additional Documentation
- **[QUICK_START.md](QUICK_START.md)** - Quick reference for network deployment
- **[NETWORK_DEPLOYMENT.md](NETWORK_DEPLOYMENT.md)** - Comprehensive network setup guide
- **[NETWORK_EXPLAINED.md](NETWORK_EXPLAINED.md)** - Why nameserver uses 0.0.0.0 and network architecture explained
- **[TESTING_GUIDE.md](TESTING_GUIDE.md)** - Complete testing procedures for network deployment

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

## 🚀 Quick Start

### For Local Testing (Same Machine)

```bash
# Terminal 1
./nameserver

# Terminal 2
./storageserver 127.0.0.1 9001 8080

# Terminal 3
./storageserver 127.0.0.1 9002 8080

# Terminal 4
./client 127.0.0.1 8080
```

### For Network Deployment (Different Machines)

**On Server Machine (e.g., 192.168.1.100):**
```bash
# Find your IP first
hostname -I | awk '{print $1}'  # Linux/macOS
# or
ipconfig                         # Windows

# Start nameserver
./nameserver
```

**On Storage Server Machine(s):**
```bash
./storageserver 192.168.1.100 9001 8080
```

**On Client Machine(s):**
```bash
./client 192.168.1.100 8080
```

> **Important:** Replace `192.168.1.100` with your actual nameserver IP address!

---

## 🚀 Usage

### Local Deployment (Single Laptop)

**Terminal 1 - Start Name Server:**
```bash
./nameserver
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

### Network Deployment (Multiple Laptops)

#### Step 1: Find IP Addresses
On each machine, find the local network IP address:

**Quick Method - Use Included Script:**
```bash
# On Linux/macOS
chmod +x find_ip.sh
./find_ip.sh

# On Windows PowerShell
.\find_ip.ps1
```

**Manual Methods:**

**On Linux/macOS:**
```bash
# Method 1: Using ip command
ip addr show | grep "inet " | grep -v 127.0.0.1

# Method 2: Using ifconfig
ifconfig | grep "inet " | grep -v 127.0.0.1

# Method 3: Using hostname
hostname -I
```

**On Windows:**
```powershell
ipconfig | findstr IPv4
```

Look for an IP address like `192.168.x.x` or `10.0.x.x` (your local network IP).

#### Step 2: Start Name Server (Server Laptop)
On the machine that will host the nameserver (e.g., IP: `192.168.1.100`):
```bash
./nameserver
# or with custom port:
# ./nameserver 8080
```

The nameserver automatically binds to **0.0.0.0 (all network interfaces)**, which means:
- ✅ It listens on ALL IP addresses of this machine
- ✅ Accepts connections from localhost (127.0.0.1)
- ✅ Accepts connections from local network (192.168.x.x)
- ✅ No need to specify an IP address - it handles everything automatically!

The startup message will show you how clients can connect.

#### Step 3: Start Storage Servers
You can run storage servers on the same machine as the nameserver or on different machines.

**On Server Machine (192.168.1.100):**
```bash
./storageserver 192.168.1.100 9001 8080
```

**On Another Laptop (192.168.1.101):**
```bash
./storageserver 192.168.1.100 9002 8080
```
> Note: First argument is the **nameserver's IP**, second is **this storage server's port**, third is **nameserver's port**.

#### Step 4: Start Clients
Clients can connect from any laptop on the network.

**On Client Laptop (192.168.1.102):**
```bash
./client 192.168.1.100 8080
```
> Note: First argument is the **nameserver's IP**, second is **nameserver's port**.

### Command Syntax Summary

```bash
# Name Server (binds to all interfaces - 0.0.0.0)
./nameserver [port]
# Example: ./nameserver 8080
# Default: ./nameserver (uses port 8080)

# Storage Server
./storageserver <nameserver_ip> <my_port> [ns_port]
# Example: ./storageserver 192.168.1.100 9001 8080

# Client
./client <nameserver_ip> [ns_port]
# Example: ./client 192.168.1.100 8080
```

**Important Notes:**
- **Nameserver** automatically binds to `0.0.0.0` (all network interfaces), so it accepts connections from:
  - Localhost: `127.0.0.1`
  - Local network: `192.168.x.x` or `10.0.x.x`
  - Any other network interface on the machine
- **Storage Servers** and **Clients** need to specify the nameserver's IP address
- On the same machine, use `127.0.0.1`; across network, use the actual network IP

### Network Deployment Example

**Scenario:** 3 laptops on WiFi network

| Machine | IP Address | Role |
|---------|------------|------|
| Laptop A | 192.168.1.100 | Name Server |
| Laptop B | 192.168.1.101 | Storage Server 1 |
| Laptop C | 192.168.1.102 | Storage Server 2 + Client |

**On Laptop A (Name Server):**
```bash
./nameserver
```

**On Laptop B (Storage Server 1):**
```bash
./storageserver 192.168.1.100 9001 8080
```

**On Laptop C (Storage Server 2):**
```bash
./storageserver 192.168.1.100 9002 8080
```

**On Laptop C (Client - same machine, different terminal):**
```bash
./client 192.168.1.100 8080
```

**On any other laptop (Client):**
```bash
# First find Laptop A's IP, then connect
./client 192.168.1.100 8080
```

### Using Makefile Shortcuts (Local Only)

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
├── find_ip.sh           # Helper script to find IP (Linux/macOS)
├── find_ip.ps1          # Helper script to find IP (Windows)
└── README.md            # This file
```

### Important Notes for Network Deployment

1. **Firewall Configuration**: Ensure ports 8080, 9001, 9002 are open on all machines
2. **Same Network**: All machines must be on the same local network (same WiFi/LAN)
3. **IP Address**: Use local network IP (192.168.x.x or 10.0.x.x), **NOT** 127.0.0.1
4. **Start Order**: Always start nameserver first, then storage servers, then clients
5. **IP Detection**: Storage servers automatically detect their IP from the socket connection
6. **VPN**: Disable VPN if experiencing connection issues on local network

### Network Architecture Explained

**Why nameserver uses 0.0.0.0 (INADDR_ANY):**
- Binding to `0.0.0.0` means "listen on ALL available network interfaces"
- This is **standard practice** for servers - they don't choose a specific IP
- The server accepts connections on any IP the machine has (localhost, WiFi, Ethernet, etc.)
- Clients decide which IP to use when connecting

**Example:**
- Machine A has IPs: `127.0.0.1` (localhost), `192.168.1.100` (WiFi)
- Nameserver binds to `0.0.0.0:8080`
- Clients can connect via:
  - `127.0.0.1:8080` (if on same machine)
  - `192.168.1.100:8080` (if on network)

**Why storage servers and clients specify IP:**
- They are **clients** connecting TO the nameserver
- Clients must specify exactly which IP to connect to
- On same machine: use `127.0.0.1`
- On network: use nameserver's network IP (e.g., `192.168.1.100`)

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
# On Linux/macOS - Find process using port
lsof -i :8080

# On Windows
netstat -ano | findstr :8080

# Kill process (Linux/macOS)
kill -9 <PID>

# Kill process (Windows)
taskkill /PID <PID> /F
```

**Storage server won't connect (Network Deployment):**
1. Ensure nameserver is running first
2. Verify nameserver's IP address is correct (use `ip addr` or `ipconfig`)
3. Check if firewall is blocking the ports:
   ```bash
   # On Linux - Allow ports through firewall
   sudo ufw allow 8080/tcp
   sudo ufw allow 9001/tcp
   sudo ufw allow 9002/tcp
   
   # On Windows - Add firewall rules (Run as Administrator)
   netsh advfirewall firewall add rule name="NFS NameServer" dir=in action=allow protocol=TCP localport=8080
   netsh advfirewall firewall add rule name="NFS Storage 1" dir=in action=allow protocol=TCP localport=9001
   netsh advfirewall firewall add rule name="NFS Storage 2" dir=in action=allow protocol=TCP localport=9002
   ```
4. Ensure all machines are on the same network (same subnet)
5. Try pinging the nameserver from storage server:
   ```bash
   ping 192.168.1.100
   ```

**Client can't connect (Network Deployment):**
1. Verify nameserver IP and port are correct
2. Check network connectivity: `ping <nameserver_ip>`
3. Ensure nameserver is listening on all interfaces (it should bind to 0.0.0.0)
4. Check firewall settings on nameserver machine
5. Verify you're using the correct local network IP (not localhost 127.0.0.1)

**Can't find local IP address:**
```bash
# Linux/macOS
ip route get 8.8.8.8 | awk '{print $7; exit}'

# Alternative for Linux
hostname -I | awk '{print $1}'

# Windows PowerShell
(Get-NetIPAddress -AddressFamily IPv4 | Where-Object {$_.InterfaceAlias -notlike "*Loopback*"}).IPAddress
```

**Connection works locally but not across network:**
1. Ensure you're using the **local network IP** (192.168.x.x or 10.0.x.x), not 127.0.0.1
2. Check if both machines are on the same WiFi/network
3. Disable VPN if active (can interfere with local network)
4. On some networks, client isolation might be enabled - check router settings

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
when multiple user are writing and one of them changes the sentence count(by adding delimeter), the indexing changes,so the second person write wont take place like what it should . if we want we can also  send error msg to user2 if user1 changes sentence count (by adding delimeter).