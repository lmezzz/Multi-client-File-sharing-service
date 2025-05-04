# Multi-client File Sharing Service

A robust C-based client-server application that enables multiple clients to concurrently upload and download files while maintaining data consistency through reader-writer locks.

## Features

- **Concurrent File Operations**: Multiple clients can simultaneously access the server
- **Reader-Writer Lock Implementation**: Ensures data consistency during file operations
- **Chunked File Transfer**: Files are transferred in chunks for efficient memory usage
- **Bi-directional Transfer**: Supports both upload and download operations
- **Network-safe Implementation**: Handles network byte ordering and connection issues gracefully

## Technical Details

### Server (`server.c`)
- Implements a multi-threaded server architecture
- Uses pthread library for thread management
- Implements custom reader-writer locks for file access control
- Handles multiple client connections concurrently
- Buffer size: 128 bytes
- Buffer capacity: 8 chunks

### Client (`client.c`)
- Provides a command-line interface for file operations
- Supports upload and download commands
- Chunk size: 1024 bytes
- Implements robust error handling

## Building the Project

### Prerequisites
- GCC compiler
- POSIX-compliant operating system (Linux/Unix)
- pthread library

### Compilation

To compile the server:
```bash
gcc -o server server.c -pthread
```

To compile the client:
```bash
gcc -o client client.c
```

## Usage

### Starting the Server
```bash
./server
```

### Running the Client
```bash
./client
```

### Available Commands
1. Upload a file:
```bash
upload <local_filename> <remote_filename>
```

2. Download a file:
```bash
download <remote_filename> <local_filename>
```

## Implementation Details

### File Access Control
- Uses a mutex-based reader-writer lock implementation
- Prevents write-write and read-write conflicts
- Allows multiple simultaneous readers
- Writers have priority to prevent starvation

### Data Transfer
- Files are transferred in chunks to manage memory efficiently
- Network byte ordering is handled for cross-platform compatibility
- Robust error handling for network disconnections and I/O errors

## Error Handling
- Graceful handling of client disconnections
- Proper cleanup of resources
- Comprehensive error messages for debugging
- Protection against buffer overflows

## Limitations
- Maximum filename length: 256 characters
- Fixed chunk sizes for transfer
- Server runs on local network only

## Contributing
Feel free to submit issues and enhancement requests.

## License
This project is open source and available under the MIT License. 