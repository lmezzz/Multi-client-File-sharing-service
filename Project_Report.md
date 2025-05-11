# Multi-client File Sharing Service: A Study in Concurrent Programming
## Project Report

### Table of Contents
1. [Introduction](#introduction)
2. [Theoretical Background](#theoretical-background)
3. [System Architecture](#system-architecture)
4. [Implementation Details](#implementation-details)
5. [Use Cases and Dry Run](#use-cases-and-dry-run)
6. [Performance Analysis](#performance-analysis)
7. [Conclusions](#conclusions)

## 1. Introduction

### 1.1 Project Overview
The Multi-client File Sharing Service demonstrates the practical application of concurrent programming concepts through a client-server architecture that enables multiple clients to simultaneously share and access files. The system implements advanced synchronization mechanisms to ensure data consistency and prevent race conditions.

### 1.2 Objectives
- Implement a robust concurrent file sharing system
- Demonstrate reader-writer synchronization
- Showcase thread management and resource sharing
- Ensure data consistency in multi-user scenarios
- Implement efficient network communication

## 2. Theoretical Background

### 2.1 Multithreading Concepts
#### 2.1.1 Thread Management
- **Thread Creation**: Using POSIX threads (pthreads) for concurrent execution
- **Thread Synchronization**: Mutex locks and condition variables
- **Thread Pooling**: Efficient resource utilization

#### 2.1.2 Critical Section Problem
- **Race Conditions**: Prevention through mutex locks
- **Deadlock Prevention**: Implementation of proper lock ordering
- **Resource Sharing**: Buffer management and file access control

### 2.2 Reader-Writer Problem
#### 2.2.1 Problem Statement
- Multiple readers can simultaneously read
- Only one writer can write at a time
- Writers have priority to prevent starvation

#### 2.2.2 Solution Implementation
```c
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

### 2.3 Producer-Consumer Problem
#### 2.3.1 Implementation in File Transfer
- Server as producer (upload) or consumer (download)
- Client as consumer (download) or producer (upload)
- Bounded buffer implementation for chunk transfer

## 3. System Architecture

### 3.1 Server Design
```
[Client 1] ─┐
[Client 2] ─┤     ┌─ [Thread Pool] ─┐
[Client 3] ─┼─ → ─┤                 ├─ [File System]
[Client n] ─┘     └─ [File Access   │
                      Control List] ─┘
```

### 3.2 Component Interaction
- **Thread Pool**: Manages client connections
- **File Access Control**: Implements reader-writer locks
- **Buffer Management**: Handles data transfer chunks

## 4. Implementation Details

### 4.1 Synchronization Mechanisms
#### 4.1.1 Mutex Implementation
```c
pthread_mutex_lock(&control->mutex);
// Critical section
pthread_mutex_unlock(&control->mutex);
```

#### 4.1.2 Condition Variables
```c
// Reader waiting
while (control->active_writer || control->waiting_writers > 0) {
    pthread_cond_wait(&control->can_read, &control->mutex);
}
```

### 4.2 File Transfer Protocol
#### 4.2.1 Chunked Transfer
```c
#define CHUNK_SIZE 128
#define BUFFER_CAPACITY 8

typedef struct {
    char data[CHUNK_SIZE];
    size_t bytes_read;
    int is_last_chunk;
} buffer_item;
```

## 5. Use Cases and Dry Run

### 5.1 Concurrent Read Scenario
#### Initial State:
- File: ayaan.txt
- Clients: 3 readers

#### Sequence of Events:
1. **Client 1 Requests Read**
   ```
   acquire_read_lock(control)
   active_readers = 1
   ```

2. **Client 2 Requests Read**
   ```
   acquire_read_lock(control)
   active_readers = 2
   ```

3. **Client 3 Requests Read**
   ```
   acquire_read_lock(control)
   active_readers = 3
   ```

#### Result:
- All clients read simultaneously
- No conflicts
- System maintains consistency

### 5.2 Read-Write Conflict Scenario
#### Initial State:
- File: ayaan.txt
- Clients: 2 readers, 1 writer

#### Sequence of Events:
1. **Two Readers Active**
   ```
   active_readers = 2
   active_writer = false
   ```

2. **Writer Requests Access**
   ```
   waiting_writers = 1
   // Writer waits for readers to finish
   ```

3. **Readers Complete**
   ```
   active_readers = 0
   // Writer granted access
   active_writer = true
   ```

### 5.3 File Upload Dry Run
#### Command:
```bash
upload ayaan.txt remote_ayaan.txt
```

#### Sequence:
1. **Client Initialization**
   ```c
   int fd = open("ayaan.txt", O_RDONLY);
   ```

2. **Chunk Transfer**
   ```c
   while (true) {
       bytes_read = read(fd, buff, CHUNK_SIZE);
       send(socket, &chunk_size_n, sizeof(int), 0);
       send(socket, buff, bytes_read, 0);
   }
   ```

3. **Server Processing**
   ```c
   acquire_write_lock(control);
   // Process chunks
   release_write_lock(control);
   ```

## 6. Performance Analysis

### 6.1 Concurrent Access Metrics
- Maximum simultaneous readers: Unlimited
- Maximum simultaneous writers: 1
- Writer priority implementation prevents reader starvation

### 6.2 Resource Utilization
- Thread pool efficiency
- Memory usage for buffering
- Network bandwidth utilization

### 6.3 Synchronization Overhead
- Mutex lock/unlock timing
- Context switching costs
- Reader-writer lock performance

## 7. Conclusions

### 7.1 Achievement of Objectives
- Successfully implemented concurrent file sharing
- Demonstrated synchronization principles
- Achieved data consistency
- Prevented race conditions and deadlocks

### 7.2 Learning Outcomes
- Practical application of theoretical concepts
- Implementation of complex synchronization mechanisms
- Network programming with concurrent access
- Resource management in multi-user scenarios

### 7.3 Future Enhancements
- Distributed system implementation
- Enhanced security features
- Dynamic thread pool sizing
- Improved file chunking algorithms

## References
1. "Operating System Concepts" - Silberschatz, Galvin, Gagne
2. POSIX Threads Programming Guide
3. "The Art of Multiprocessor Programming" - Herlihy, Shavit 