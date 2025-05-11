# Multi-client File Sharing Service: Flow Analysis

## Architecture Overview

The system implements a client-server architecture that enables concurrent file operations through a producer-consumer pattern. The application efficiently handles multiple client connections simultaneously while maintaining data consistency through a sophisticated reader-writer locking mechanism.

## System Flow of Operation

### Server Initialization and Connection Handling

1. The server initializes a socket, binds it to a specified port (8080), and begins listening for incoming client connections.
2. When a client connects, the main thread creates a new RequestHandler thread and passes it the client socket.
3. The RequestHandler thread is responsible for receiving and parsing the client's command (upload or download) and the associated filename.

### Command Processing Flow

#### Download Request Process:

1. **Client Side:**
   - User requests a file download through the client interface
   - Client sends command, command length, filename, and filename length to the server
   - Client prepares to receive file data in chunks

2. **Server Side (Producer-Consumer Pattern):**
   - The RequestHandler thread receives the download command
   - A DownLoadingFile thread is created to handle the request
   - The thread acquires a read lock on the requested file using the FileAccessControl mechanism
   - Two threads are created to implement the producer-consumer pattern:
     
     a. **Producer Thread (ReadFromFile function):**
     ```
     - Reads file data into a shared buffer
     - Uses mutex locks to ensure thread safety
     - Signals the consumer when data is available
     - Sets an EOF flag when the entire file has been read
     ```

     b. **Consumer Thread (SendOverANetwork function):**
     ```
     - Waits for the buffer to contain data
     - Takes chunks from the buffer and sends them to the client
     - Manages network byte ordering for cross-platform compatibility
     - Stops when the producer signals EOF and the buffer is empty
     ```
   
   - The producer-consumer pattern ensures efficient memory usage by limiting the amount of data in memory at once

3. **Data Transfer:**
   - File data is broken into chunks (128 bytes per chunk on server side)
   - Each chunk size is sent first, followed by the chunk data
   - A zero-size chunk signals the end of transmission

#### Upload Request Process:

1. **Client Side:**
   - User requests a file upload through the client interface
   - Client sends command, command length, filename, and filename length to the server
   - Client begins reading the local file in chunks (1024 bytes) and sending to server

2. **Server Side:**
   - The RequestHandler thread receives the upload command
   - An UploadFile thread is created to handle the request
   - The thread acquires a write lock on the target file to prevent concurrent writes or reads
   - The thread creates (or truncates) the file and prepares to write incoming data
   - Each chunk is received and written directly to the file
   - The server closes the file when a zero-size chunk is received, signaling end of transmission

## Reader-Writer Lock Implementation

The system employs a sophisticated file access control mechanism to prevent data corruption while maximizing concurrency:

1. **File Control Management:**
   - Each file is associated with a FileAccessControl structure
   - A global linked list tracks all files in use
   - Reference counting prevents premature cleanup

2. **Read Locks:**
   - Multiple readers can access a file simultaneously
   - No writers can access while readers are active
   - Implementation gives priority to writers to prevent writer starvation

3. **Write Locks:**
   - Only one writer can access a file at a time
   - No readers can access while a writer is active
   - Writers wait for all current readers to finish before acquiring the lock

## Buffer Management

The producer-consumer pattern is implemented using:

1. **Shared Buffer Structure:**
   - Fixed-size circular buffer (8 chunks capacity)
   - Each buffer item contains data and metadata (size, EOF flag)

2. **Synchronization Mechanisms:**
   - Mutex lock to protect buffer access
   - Condition variables "not_empty" and "not_full" for producer-consumer coordination
   - Explicit signaling to wake waiting threads

## Concurrency Model

The system uses POSIX threads (pthreads) to achieve concurrency:

1. **Main Thread:**
   - Accepts client connections
   - Dispatches RequestHandler threads

2. **Request Handler Threads:**
   - Parse client commands
   - Dispatch specialized worker threads for uploads/downloads

3. **Worker Threads:**
   - Implement file operations with appropriate locking
   - Create producer-consumer threads for downloads
   - Directly handle uploads from the client

This threading model allows the server to handle multiple simultaneous clients efficiently, while the reader-writer locks ensure data consistency.

## Network Protocol

The communication protocol follows a structured pattern:

1. **Command Transmission:**
   - Command length (integer, network byte order)
   - Command string
   - Filename length (integer, network byte order)
   - Filename string

2. **Data Transmission:**
   - Chunk size (integer, network byte order)
   - Chunk data (byte array)
   - Zero chunk size for end-of-transmission

This protocol ensures reliable data transfer across different system architectures by handling network byte ordering and implementing robust error checking.

## Project Code with Algorithm Explanation

### Key Data Structures

```c
// Buffer item for producer-consumer pattern
typedef struct {
    char data[CHUNK_SIZE];
    size_t bytes_read;
    int is_last_chunk;
} buffer_item;

// Shared data between producer and consumer threads
typedef struct {
    buffer_item buffer[BUFFER_CAPACITY];
    int in, out, count;

    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;

    int file;
    int client_sock;
    int eof_reached;
} thread_shared_data;

// File access control structure for reader-writer locks
typedef struct FileAccessControl {
    char filename[256];
    pthread_mutex_t mutex;
    pthread_cond_t can_read;
    pthread_cond_t can_write;
    int active_readers;
    bool active_writer;
    int waiting_writers;
    int users;

    struct FileAccessControl *next;
} FileAccessControl;
```

### Critical Algorithms

#### 1. Producer Algorithm (ReadFromFile)

```c
void* ReadFromFile(void *arg){
    thread_shared_data *sh_data = (thread_shared_data*) arg;

    while (1) {
        // Lock the shared buffer
        pthread_mutex_lock(&sh_data->mutex);
        
        // Wait if buffer is full
        while (sh_data->count == BUFFER_CAPACITY && sh_data->eof_reached == 0) {
            pthread_cond_wait(&sh_data->not_full, &sh_data->mutex);
        }
        
        // Read data from file into buffer
        int bytes_read = read(sh_data->file, sh_data->buffer[sh_data->in].data, CHUNK_SIZE);
        sh_data->buffer[sh_data->in].bytes_read = bytes_read;
        
        // Update buffer state
        (sh_data->count)++;
        sh_data->in = (sh_data->in + 1) % BUFFER_CAPACITY;
        
        // Signal consumer that data is available
        pthread_cond_signal(&sh_data->not_empty);
        pthread_mutex_unlock(&sh_data->mutex);

        // Check if we've reached end of file
        if (bytes_read < CHUNK_SIZE) {
            sh_data->eof_reached = 1;
            break;
        }
    }
    return NULL;
}
```

#### 2. Consumer Algorithm (SendOverANetwork)

```c
void* SendOverANetwork(void *arg){
    thread_shared_data *sh_data = (thread_shared_data*) arg;

    while(1) {
        // Lock the shared buffer
        pthread_mutex_lock(&sh_data->mutex);
        
        // Wait if buffer is empty and not at EOF
        while(sh_data->count == 0 && (sh_data->eof_reached == 0)) {
            pthread_cond_wait(&(sh_data->not_empty), &(sh_data->mutex));
        }

        // Exit if buffer is empty and EOF reached
        if (sh_data->count == 0 && sh_data->eof_reached == 1) {
            pthread_mutex_unlock(&sh_data->mutex);
            break;
        }

        // Copy data from buffer to local array
        char buff[CHUNK_SIZE];
        int bytes_to_send = sh_data->buffer[sh_data->out].bytes_read;
        memcpy(buff, sh_data->buffer[sh_data->out].data, bytes_to_send);

        // Convert to network byte order
        int bytes_to_send_n = htonl(bytes_to_send);
        
        // Update buffer state
        sh_data->out = (sh_data->out + 1) % BUFFER_CAPACITY;
        sh_data->count--;
        
        // Signal producer that space is available
        pthread_cond_signal(&sh_data->not_full);
        pthread_mutex_unlock(&sh_data->mutex);

        // Send data over the network
        send(sh_data->client_sock, &bytes_to_send_n, sizeof(int), 0);
        send(sh_data->client_sock, buff, bytes_to_send, 0);
    }
    return NULL;
}
```

#### 3. Reader-Writer Lock Algorithms

```c
// Acquire read lock
void acquire_read_lock(FileAccessControl* control) {
    pthread_mutex_lock(&control->mutex);
    
    // Wait if there's an active writer or writers waiting
    while (control->active_writer || control->waiting_writers > 0) {
        pthread_cond_wait(&control->can_read, &control->mutex);
    }
    
    // Increment reader count
    control->active_readers++;
    pthread_mutex_unlock(&control->mutex);
}

// Release read lock
void release_read_lock(FileAccessControl* control) {
    pthread_mutex_lock(&control->mutex);
    
    // Decrement reader count
    control->active_readers--;
    
    // If last reader, signal waiting writers
    if (control->active_readers == 0 && control->waiting_writers > 0) {
        pthread_cond_signal(&control->can_write);
    }
    
    pthread_mutex_unlock(&control->mutex);
}

// Acquire write lock
void acquire_write_lock(FileAccessControl* control) {
    pthread_mutex_lock(&control->mutex);
    
    // Increment waiting writers count
    control->waiting_writers++;
    
    // Wait until no active readers or writers
    while (control->active_readers > 0 || control->active_writer) {
        pthread_cond_wait(&control->can_write, &control->mutex);
    }
    
    // Got the lock, decrement waiting count, set active flag
    control->waiting_writers--;
    control->active_writer = true;
    
    pthread_mutex_unlock(&control->mutex);
}

// Release write lock
void release_write_lock(FileAccessControl* control) {
    pthread_mutex_lock(&control->mutex);
    
    // Clear active writer flag
    control->active_writer = false;
    
    // If writers waiting, signal one writer, otherwise signal all readers
    if (control->waiting_writers > 0) {
        pthread_cond_signal(&control->can_write);
    } else {
        pthread_cond_broadcast(&control->can_read);
    }
    
    pthread_mutex_unlock(&control->mutex);
}
```

## Output with Tracing

### Server Startup Trace

```
Server listening on port 8080
```

### Client Connection Trace

```
Accepted connection from 192.168.18.37:52134 (socket: 4)
Dispatched handler thread for socket 4
```

### Download Request Trace

```
RequestHandler: Received request: Command='download', Filename='ayaan.txt'
RequestHandler: Dispatching download task for ayaan.txt
Found existing control for file: ayaan.txt, users: 1
```

#### Producer-Consumer Execution Trace (Download)

```
# Producer Thread
ReadFromFile: Acquiring lock for buffer
ReadFromFile: Buffer not full, reading file
ReadFromFile: Read 22 bytes from file
ReadFromFile: Updated buffer: in=1, count=1
ReadFromFile: Signaling not_empty, releasing lock
ReadFromFile: End of file reached (bytes < CHUNK_SIZE), setting EOF flag

# Consumer Thread
SendOverANetwork: Acquiring lock for buffer
SendOverANetwork: Buffer has data, processing
SendOverANetwork: Processing chunk of 22 bytes
SendOverANetwork: Updated buffer: out=1, count=0
SendOverANetwork: Signaling not_full, releasing lock
SendOverANetwork: Sent chunk size (network order) and data
SendOverANetwork: Acquiring lock for buffer
SendOverANetwork: Buffer empty and EOF reached, exiting
Download: Written 22 bytes to DowloadedFile.txt
Download finished for DowloadedFile.txt.
```

### Upload Request Trace

```
RequestHandler: Received request: Command='upload', Filename='newfile.txt'
RequestHandler: Dispatching upload task for newfile.txt
Created control for file: newfile.txt
Upload: Sent 1024 bytes from localfile.txt
Upload: Sent 1024 bytes from localfile.txt
Upload: Sent 346 bytes from localfile.txt
Upload: Reached end of file or read error for localfile.txt.
Upload finished for localfile.txt.
Released control for file: newfile.txt, users remaining: 0
Destroying control for file: newfile.txt
```

### Concurrent Access Trace

```
# Client 1 (Reading)
RequestHandler: Received request: Command='download', Filename='shared.txt'
Found existing control for file: shared.txt, users: 1
acquire_read_lock: Waiting for writers to finish
acquire_read_lock: No writers, acquired read lock (readers=1)

# Client 2 (Reading concurrently)
RequestHandler: Received request: Command='download', Filename='shared.txt'
Found existing control for file: shared.txt, users: 2
acquire_read_lock: No writers, acquired read lock (readers=2)

# Client 3 (Writing) 
RequestHandler: Received request: Command='upload', Filename='shared.txt'
Found existing control for file: shared.txt, users: 3
acquire_write_lock: Incrementing waiting writers (waiting=1)
acquire_write_lock: Waiting for readers to finish (readers=2)

# Client 1 finishes
release_read_lock: Decrementing readers (readers=1)

# Client 2 finishes
release_read_lock: Decrementing readers (readers=0)
release_read_lock: No more readers, signaling waiting writers

# Client 3 proceeds
acquire_write_lock: No more readers, acquired write lock
``` 