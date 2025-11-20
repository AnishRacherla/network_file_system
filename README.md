1. Name Server

The nameserver coordinates the entire system. Its responsibilities include:
Managing metadata about all files and folders
Handling file routing: deciding which storage server stores which file
Managing access control lists (ACLs) for permissions
Handling search queries
Coordinating replication and failover
Sending notifications to clients when files change
The nameserver listens on all network interfaces (0.0.0.0), so clients on any connected network can reach it.

2. Storage Servers

Storage servers store the actual file data and perform lower-level operations such as:
Reading, writing, and updating file contents
Maintaining sentence-level locks to support concurrent editing
Streaming file content
Computing file metadata (word count, size, timestamps)
Replicating files to backup storage servers
Each storage server runs independently and registers itself with the nameserver.

3. Clients

Clients provide the user interface for interacting with the system. They connect to the nameserver to:
Browse files and folders
Create, delete, rename, and move files/folders
Request access to files
Receive notifications about file updates
Clients connect directly to storage servers only when reading or writing file content.
Core Features
File and Folder Operations
Create/read/write/delete files
Create/delete folders with hierarchical paths
View file info such as size, timestamps, and ownership
Concurrency and Collaboration
Sentence-level editing with locking
Real-time notifications when files change
Access control via owner approval

Versioning

Checkpoints: save and restore tagged file versions
Undo/redo for write operations
List and inspect old versions without affecting current data

Search

The system maintains an inverted index that enables fast keyword search across all files and folders. Search results include the file and sentence where the word appears.

Fault Tolerance

File replication across multiple storage servers
Automatic failover if a storage server becomes unreachable
Heartbeat monitoring
Metadata recovery after restart

Performance Optimizations

Hashmap-based file lookup
LRU cache for recent metadata
Fine-grained locking (sentence level)
Thread-safe logging and command handling

Basic Usage 

Start the nameserver
Start storage servers, giving them the nameserver IP
Start clients, connecting them to the nameserver

Perform operations like creating files, editing, streaming, searching, and managing permissions

For network deployment, the nameserver’s real IP (e.g., 192.168.x.x) must be used so other machines can connect.

Project Structure 

nameserver.c – metadata, routing, ACLs, search, replication
storageserver.c – file storage, write operations, replication
client.c – user interface and command processing
locks.c – locking mechanisms
logger.c – system logging
filemap.c – file/folder structure utilities
Makefile – build configuration

ASSUMPTION:
when multiple user are writing and one of them changes the sentence count(by adding delimeter), the indexing changes,so the second person write wont take place like what it should . if we want we can also  send error msg to user2 if user1 changes sentence count (by adding delimeter).