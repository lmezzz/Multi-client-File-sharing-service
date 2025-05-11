#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<string.h>
#include<stdbool.h>
#include <pthread.h>


#include<sys/types.h>
#include<sys/fcntl.h>

#include<netinet/in.h>

#include<arpa/inet.h>

#define CHUNK_SIZE 128
#define BUFFER_CAPACITY 8


typedef struct {
    char data[CHUNK_SIZE];
    size_t bytes_read;
    int is_last_chunk;
} buffer_item;

typedef struct {
    buffer_item buffer[BUFFER_CAPACITY];
    int in, out, count;

    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;

    int  file;
    int client_sock;
    int eof_reached;
} thread_shared_data;

typedef struct{
    char filename[20];
    int fd;
    pthread_mutex_t mutex;
    pthread_cond_t can_write;
    pthread_cond_t can_read;

    int writers;
    int readers;
    int writers_waiting;
}file_meta_data;

// No longer need file_meta_data per request
// struct Request{
//     file_meta_data f_data; // REMOVED
//     int socket;
// };

// New struct to pass arguments to worker threads
typedef struct {
    int client_socket;
    char filename[256]; // Ensure this matches FileAccessControl filename size
} ClientTaskArgs;


// --- Reader/Writer Lock Implementation using Mutex/Cond Vars ---
typedef struct FileAccessControl {
    char filename[256];           // Max filename length (adjust if needed)
    pthread_mutex_t mutex;        // Mutex protecting this structure's fields
    pthread_cond_t can_read;     // Condition variable for readers to wait
    pthread_cond_t can_write;    // Condition variable for writers to wait
    int active_readers;         // How many threads are currently reading
    bool active_writer;         // Is a thread currently writing?
    int waiting_writers;        // How many threads are waiting to write
    int users;                  // How many requests are currently associated (for cleanup)

    struct FileAccessControl *next; // Pointer for implementing a linked list
} FileAccessControl;

// Global head of the linked list of file control structs
FileAccessControl *g_file_list_head = NULL;

// Mutex to protect access to the global list (g_file_list_head)
pthread_mutex_t g_file_list_mutex = PTHREAD_MUTEX_INITIALIZER;


// Finds or creates a FileAccessControl struct for a given filename.
// Returns a pointer to the struct, or NULL on failure.
// Increments the user count, caller must call release_file_control later.
FileAccessControl* get_or_create_file_control(const char* filename) {
    FileAccessControl *current = NULL;
    FileAccessControl *new_control = NULL;

    if (filename == NULL || filename[0] == '\0') {
        fprintf(stderr, "Error: Invalid filename provided to get_or_create_file_control\n");
        return NULL;
    }

    pthread_mutex_lock(&g_file_list_mutex);

    // 1. Search for existing control struct
    current = g_file_list_head;
    while (current != NULL) {
        if (strcmp(current->filename, filename) == 0) {
            current->users++; // Increment user count
            pthread_mutex_unlock(&g_file_list_mutex);
            printf("Found existing control for file: %s, users: %d\n", current->filename, current->users); // Debug print
            return current; // Found existing one
        }
        current = current->next;
    }

    // 2. Not found, create a new one
    new_control = (FileAccessControl*)malloc(sizeof(FileAccessControl));
    if (new_control == NULL) {
        perror("malloc FileAccessControl failed");
        pthread_mutex_unlock(&g_file_list_mutex);
        return NULL; // Allocation failed
    }

    // Initialize the new struct
    strncpy(new_control->filename, filename, sizeof(new_control->filename) - 1);
    new_control->filename[sizeof(new_control->filename) - 1] = '\0'; // Ensure null termination

    // Check for initialization errors
    if (pthread_mutex_init(&new_control->mutex, NULL) != 0 ||
        pthread_cond_init(&new_control->can_read, NULL) != 0 ||
        pthread_cond_init(&new_control->can_write, NULL) != 0) {
        perror("Failed to initialize mutex/cond vars for FileAccessControl");
        free(new_control);
        pthread_mutex_unlock(&g_file_list_mutex);
        return NULL;
    }

    new_control->active_readers = 0;
    new_control->active_writer = false;
    new_control->waiting_writers = 0;
    new_control->users = 1; // First user

    // Add to the head of the global list
    new_control->next = g_file_list_head;
    g_file_list_head = new_control;

    pthread_mutex_unlock(&g_file_list_mutex);
    printf("Created control for file: %s\n", new_control->filename); // Debug print
    return new_control;
}

// Releases a reference to a file control struct.
// Cleans up if this was the last user.
void release_file_control(FileAccessControl* control) {
    if (control == NULL) return;
    bool should_destroy = false;

    pthread_mutex_lock(&g_file_list_mutex);

    control->users--;
    printf("Released control for file: %s, users remaining: %d\n", control->filename, control->users); // Debug print

    // Cleanup if no longer in use
    if (control->users == 0) {
        should_destroy = true;
        // Remove from list
        FileAccessControl *prev = NULL;
        FileAccessControl *current = g_file_list_head;
        while (current != NULL) {
            if (current == control) {
                if (prev == NULL) { // It's the head
                    g_file_list_head = current->next;
                } else {
                    prev->next = current->next;
                }
                 printf("Removing control for file: %s from list\n", control->filename); // Debug print
                break; // Found and logically removed
            }
            prev = current;
            current = current->next;
        }
         if (current == NULL) {
             // This case should ideally not happen if release is called correctly
             fprintf(stderr, "Error: Tried to remove control for %s, but not found in list!\n", control->filename);
             should_destroy = false; // Don't destroy if not found
         }
    }

    pthread_mutex_unlock(&g_file_list_mutex);

    // Destroy mutex/cond vars and free memory *outside* the global list lock
    if (should_destroy) {
        printf("Destroying control for file: %s\n", control->filename); // Debug print
        pthread_mutex_destroy(&control->mutex);
        pthread_cond_destroy(&control->can_read);
        pthread_cond_destroy(&control->can_write);
        free(control);
    }
}


// --- Locking Functions ---

// Acquire read access
void acquire_read_lock(FileAccessControl* control) {
    if (control == NULL) {
        fprintf(stderr, "Error: acquire_read_lock called with NULL control\n");
        return;
    }
    
    time_t now;
    char timestamp[26];
    time(&now);
    ctime_r(&now, timestamp);
    timestamp[24] = '\0';  // Remove newline
    
    pthread_t tid = pthread_self();
    printf("\n[%s] Reader Thread %lu: Attempting to acquire read lock for file: %s\n", timestamp, (unsigned long)tid, control->filename);
    
    pthread_mutex_lock(&control->mutex);
    printf("[%s] Reader Thread %lu: Acquired mutex, checking conditions\n", timestamp, (unsigned long)tid);
    
    // Wait while there's an active writer OR waiting writers (preference to writers)
    while (control->active_writer || control->waiting_writers > 0) {
        printf("[%s] Reader Thread %lu: Waiting - Active writer: %d, Waiting writers: %d\n", 
               timestamp, (unsigned long)tid, control->active_writer, control->waiting_writers);
        pthread_cond_wait(&control->can_read, &control->mutex);
        time(&now);
        ctime_r(&now, timestamp);
        timestamp[24] = '\0';
        printf("[%s] Reader Thread %lu: Woke up from wait, rechecking conditions\n", timestamp, (unsigned long)tid);
    }
    
    control->active_readers++;
    printf("[%s] Reader Thread %lu: Successfully acquired read lock. Active readers: %d\n", 
           timestamp, (unsigned long)tid, control->active_readers);
    
    pthread_mutex_unlock(&control->mutex);
    printf("[%s] Reader Thread %lu: Released mutex after acquiring read lock\n", timestamp, (unsigned long)tid);
}

// Release read access
void release_read_lock(FileAccessControl* control) {
    if (control == NULL) {
        fprintf(stderr, "Error: release_read_lock called with NULL control\n");
        return;
    }
    
    time_t now;
    char timestamp[26];
    time(&now);
    ctime_r(&now, timestamp);
    timestamp[24] = '\0';  // Remove newline
    
    pthread_t tid = pthread_self();
    printf("\n[%s] Reader Thread %lu: Attempting to release read lock for file: %s\n", timestamp, (unsigned long)tid, control->filename);
    
    pthread_mutex_lock(&control->mutex);
    printf("[%s] Reader Thread %lu: Acquired mutex for release\n", timestamp, (unsigned long)tid);
    
    control->active_readers--;
    printf("[%s] Reader Thread %lu: Decremented active readers. New count: %d\n", 
           timestamp, (unsigned long)tid, control->active_readers);
    
    // If I was the last reader AND writers are waiting, signal one writer
    if (control->active_readers == 0 && control->waiting_writers > 0) {
        printf("[%s] Reader Thread %lu: Last reader out, signaling waiting writer. Writers waiting: %d\n", 
               timestamp, (unsigned long)tid, control->waiting_writers);
        pthread_cond_signal(&control->can_write);
    }
    
    pthread_mutex_unlock(&control->mutex);
    printf("[%s] Reader Thread %lu: Released mutex after releasing read lock\n", timestamp, (unsigned long)tid);
}

// Acquire write access
void acquire_write_lock(FileAccessControl* control) {
    if (control == NULL) {
        fprintf(stderr, "Error: acquire_write_lock called with NULL control\n");
        return;
    }
    
    time_t now;
    char timestamp[26];
    time(&now);
    ctime_r(&now, timestamp);
    timestamp[24] = '\0';  // Remove newline
    
    pthread_t tid = pthread_self();
    printf("\n[%s] Writer Thread %lu: Attempting to acquire write lock for file: %s\n", timestamp, (unsigned long)tid, control->filename);
    
    pthread_mutex_lock(&control->mutex);
    printf("[%s] Writer Thread %lu: Acquired mutex, checking conditions\n", timestamp, (unsigned long)tid);
    
    control->waiting_writers++; // Indicate intention to write
    printf("[%s] Writer Thread %lu: Registered as waiting writer. Total waiting writers: %d\n", 
           timestamp, (unsigned long)tid, control->waiting_writers);
    
    // Wait while there are active readers OR an active writer
    while (control->active_readers > 0 || control->active_writer) {
        printf("[%s] Writer Thread %lu: Waiting - Active readers: %d, Active writer: %d\n", 
               timestamp, (unsigned long)tid, control->active_readers, control->active_writer);
        pthread_cond_wait(&control->can_write, &control->mutex);
        time(&now);
        ctime_r(&now, timestamp);
        timestamp[24] = '\0';
        printf("[%s] Writer Thread %lu: Woke up from wait, rechecking conditions\n", timestamp, (unsigned long)tid);
    }
    
    control->waiting_writers--; // No longer waiting
    control->active_writer = true; // I am the active writer now
    printf("[%s] Writer Thread %lu: Successfully acquired write lock. Remaining waiting writers: %d\n", 
           timestamp, (unsigned long)tid, control->waiting_writers);
    
    pthread_mutex_unlock(&control->mutex);
    printf("[%s] Writer Thread %lu: Released mutex after acquiring write lock\n", timestamp, (unsigned long)tid);
}

// Release write access
void release_write_lock(FileAccessControl* control) {
    if (control == NULL) {
        fprintf(stderr, "Error: release_write_lock called with NULL control\n");
        return;
    }
    
    time_t now;
    char timestamp[26];
    time(&now);
    ctime_r(&now, timestamp);
    timestamp[24] = '\0';  // Remove newline
    
    pthread_t tid = pthread_self();
    printf("\n[%s] Writer Thread %lu: Attempting to release write lock for file: %s\n", timestamp, (unsigned long)tid, control->filename);
    
    pthread_mutex_lock(&control->mutex);
    printf("[%s] Writer Thread %lu: Acquired mutex for release\n", timestamp, (unsigned long)tid);
    
    control->active_writer = false; // No longer writing
    printf("[%s] Writer Thread %lu: Marked as no longer active writer\n", timestamp, (unsigned long)tid);
    
    // Check if writers are waiting first (preference to writers)
    if (control->waiting_writers > 0) {
        printf("[%s] Writer Thread %lu: Signaling next waiting writer. Writers waiting: %d\n", 
               timestamp, (unsigned long)tid, control->waiting_writers);
        pthread_cond_signal(&control->can_write); // Signal one waiting writer
    } else {
        // Otherwise, signal all waiting readers (broadcast needed as multiple readers can proceed)
        printf("[%s] Writer Thread %lu: No waiting writers, broadcasting to all waiting readers\n", timestamp, (unsigned long)tid);
        pthread_cond_broadcast(&control->can_read);
    }
    
    pthread_mutex_unlock(&control->mutex);
    printf("[%s] Writer Thread %lu: Released mutex after releasing write lock\n", timestamp, (unsigned long)tid);
}

// --- End Reader/Writer Lock Implementation ---



void* ReadFromFile(void *arg){
    thread_shared_data *sh_data = (thread_shared_data*) arg;

    while (1)
    {
        pthread_mutex_lock(&sh_data->mutex);
        while (sh_data->count == BUFFER_CAPACITY && sh_data->eof_reached == 0)
        {
            pthread_cond_wait(&sh_data->not_full , &sh_data->mutex);
        }
        int bytes_read = read(sh_data->file , sh_data->buffer[sh_data->in].data, CHUNK_SIZE);
        sh_data->buffer[sh_data->in].bytes_read = bytes_read;
        (sh_data->count)++;
        sh_data->in = (sh_data->in +1) % BUFFER_CAPACITY;
        
        pthread_cond_signal(&sh_data->not_empty);
        pthread_mutex_unlock(&sh_data->mutex);

        if (bytes_read < CHUNK_SIZE)
        {
            sh_data->eof_reached = 1;
            break;
        }
        
    }
    
};

void* SendOverANetwork(void *arg){
    thread_shared_data *sh_data = (thread_shared_data*) arg;

    while(1){
        pthread_mutex_lock(&sh_data->mutex);
        while(sh_data->count == 0 &&(sh_data->eof_reached == 0)){
            pthread_cond_wait(&(sh_data->not_empty) , &(sh_data->mutex) );
        }

        if (sh_data->count == 0 && sh_data->eof_reached == 1) {
            pthread_mutex_unlock(&sh_data->mutex);
            break;
        }

        char buff[CHUNK_SIZE];
        int bytes_to_send = sh_data->buffer[sh_data->out].bytes_read;
        memcpy(buff, sh_data->buffer[sh_data->out].data, bytes_to_send); // Use . operator and actual size

        int bytes_to_send_n;
        bytes_to_send_n = htonl(bytes_to_send);
         // Update shared buffer state
        sh_data->out = (sh_data->out + 1) % BUFFER_CAPACITY; // Update out index
        sh_data->count--;                                // Decrement count
        pthread_cond_signal(&sh_data->not_full);
        pthread_mutex_unlock(&sh_data->mutex);

        send(sh_data->client_sock , &bytes_to_send_n , sizeof(int) , 0);
        send(sh_data->client_sock , buff , bytes_to_send, 0); // Send the actual bytes copied
    }
};

// Worker thread function for handling download requests
void* DownLoadingFile(void *arg){
    ClientTaskArgs* task_args = (ClientTaskArgs*) arg;
    if (task_args == NULL) {
        fprintf(stderr, "DownLoadingFile received NULL arguments\n");
        return NULL;
    }

    // Removed: printf("Download thread started...")

    FileAccessControl* control = get_or_create_file_control(task_args->filename);
    if (control == NULL) {
        fprintf(stderr, "Failed to get file control for %s\n", task_args->filename);
        // Send error to client?
        close(task_args->client_socket);
        free(task_args);
        return NULL;
    }

    // Acquire read lock for the file
    acquire_read_lock(control);

    int file_fd = open(task_args->filename, O_RDONLY);
    if (file_fd < 0) {
        perror("open failed in DownLoadingFile");
        release_read_lock(control); // Release lock before exiting
        release_file_control(control); // Release control struct reference
        // Send error to client?
        close(task_args->client_socket);
        free(task_args);
        return NULL;
    }

    // --- Producer-Consumer Setup ---
    pthread_t producer_thread;
    pthread_t consumer_thread;
    thread_shared_data shared;
    memset(&shared, 0, sizeof(shared));

    // Initialize mutex and cond vars for the buffer
    // TODO: Check return values of init functions
    pthread_mutex_init(&shared.mutex, NULL);
    pthread_cond_init(&shared.not_empty, NULL);
    pthread_cond_init(&shared.not_full, NULL);

    shared.file = file_fd; // Use the opened file descriptor
    shared.client_sock = task_args->client_socket; // Use the client socket from args

    // --- Start Producer and Consumer ---
    // TODO: Check return values of pthread_create
    if (pthread_create(&producer_thread, NULL, ReadFromFile, &shared) != 0) {
         perror("pthread_create producer failed");
         close(file_fd);
         release_read_lock(control);
         release_file_control(control);
         close(task_args->client_socket);
         free(task_args);
         // Destroy shared mutex/cond?
         return NULL;
    }
     if (pthread_create(&consumer_thread, NULL, SendOverANetwork, &shared) != 0) {
         perror("pthread_create consumer failed");
         // Need to cancel/join producer? Difficult state.
         close(file_fd);
         release_read_lock(control);
         release_file_control(control);
         close(task_args->client_socket);
         free(task_args);
         // Destroy shared mutex/cond?
         return NULL;
     }


    // --- Wait for threads to finish ---
    pthread_join(producer_thread , NULL);
    pthread_join(consumer_thread , NULL);

    // --- Cleanup ---
    close(file_fd); // Close the file descriptor
    pthread_mutex_destroy(&shared.mutex); // Destroy buffer mutex
    pthread_cond_destroy(&shared.not_empty); // Destroy buffer cond vars
    pthread_cond_destroy(&shared.not_full);

    release_read_lock(control); // Release the file read lock
    release_file_control(control); // Release the reference to the control struct
// Removed: printf("Download thread finished...")
close(task_args->client_socket); // Close the client socket
free(task_args); // Free the arguments struct


    return NULL; // Indicate success
};

// Worker thread function for handling upload requests
void* UploadFile(void* arg){
    ClientTaskArgs* task_args = (ClientTaskArgs*) arg;
     if (task_args == NULL) {
        fprintf(stderr, "UploadFile received NULL arguments\n");
        return NULL;
    }

    // Removed: printf("Upload thread started...")

    FileAccessControl* control = get_or_create_file_control(task_args->filename);
    if (control == NULL) {
        fprintf(stderr, "Failed to get file control for %s\n", task_args->filename);
        // Send error to client?
        close(task_args->client_socket);
        free(task_args);
        return NULL;
    }

    // Acquire write lock for the file
    acquire_write_lock(control);

    // Open file for writing (create if not exists, truncate if exists)
    int file_fd = open(task_args->filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (file_fd < 0) {
        perror("open failed in UploadFile");
        release_write_lock(control); // Release lock before exiting
        release_file_control(control); // Release control struct reference
        // Send error to client?
        close(task_args->client_socket);
        free(task_args);
        return NULL;
    }

    // --- Receive data from client and write to file ---
    char recv_buff[CHUNK_SIZE];
    int chunk_size_n;          // Network byte order size
    ssize_t chunk_size;        // Host byte order size
    ssize_t bytes_received_net; // Return value from network recv
    ssize_t bytes_received_data;// Return value from data recv
    ssize_t bytes_written_total;
    ssize_t bytes_written_now;

    while (true) {
        // 1. Receive chunk size
        bytes_received_net = recv(task_args->client_socket, &chunk_size_n, sizeof(int), MSG_WAITALL);
        if (bytes_received_net <= 0) { // Handles disconnect (0) or error (<0)
            if (bytes_received_net != 0) { // Only print perror on actual error
                 perror("UploadFile: recv chunk size failed");
            }
            // Removed: printf for disconnect
            goto upload_error_cleanup; // Jump to error cleanup section
        }
        // Note: Check for bytes_received != sizeof(int) is less critical with MSG_WAITALL

        chunk_size = ntohl(chunk_size_n);

        // Check for end-of-upload signal (size 0)
        if (chunk_size == 0) {
            // Removed: printf("Received end-of-upload signal...")
            break; // Normal end of upload, proceed to normal cleanup outside loop
        }

        // Validate chunk size
        if (chunk_size < 0 || chunk_size > CHUNK_SIZE) {
            fprintf(stderr, "UploadFile: Invalid chunk size received: %zd for %s\n", chunk_size, task_args->filename);
            goto upload_error_cleanup;
        }

        // 2. Receive chunk data
        bytes_received_net = recv(task_args->client_socket, recv_buff, chunk_size, MSG_WAITALL);
         if (bytes_received_net <= 0) { // Handles disconnect (0) or error (<0)
             if (bytes_received_net != 0) { // Only print perror on actual error
                 perror("UploadFile: recv chunk data failed");
             }
             // Removed: printf for disconnect
            goto upload_error_cleanup;
        }
        // Note: Check for bytes_received != chunk_size is less critical with MSG_WAITALL

        // 3. Write chunk data to file
        bytes_written_total = 0;
        while (bytes_written_total < bytes_received_net) {
            bytes_written_now = write(file_fd, recv_buff + bytes_written_total, bytes_received_net - bytes_written_total);
            if (bytes_written_now < 0) {
                perror("UploadFile: write to file failed");
                goto upload_error_cleanup; // Critical error, jump to error cleanup
            }
            bytes_written_total += bytes_written_now;
        }
         // Optional: printf("Written %zd bytes to %s\n", bytes_written_total, task_args->filename);
    } // End while(true) loop

// Normal cleanup path (after loop breaks successfully on chunk_size == 0)
    // Removed: printf("Upload completed successfully...")
    close(file_fd);
    release_write_lock(control);
    release_file_control(control);
    close(task_args->client_socket);
    free(task_args);
    return NULL; // Indicate success

// Error cleanup path (jumped to on error via goto)
upload_error_cleanup:
    fprintf(stderr, "UploadFile: Upload failed for %s.\n", task_args->filename);
    // Ensure file is closed even on error before releasing lock/control
    close(file_fd); // close() handles negative fd if open failed earlier
    release_write_lock(control);
    release_file_control(control);
    close(task_args->client_socket);
    free(task_args);
    return NULL; // Indicate failure (or return specific error code)
}

// Renamed parameter for clarity, now takes void* for pthread_create
// Handles a single client connection: reads request, dispatches to worker thread.
void* RequestHandler(void* p_client_socket){
    int socket = *(int*)p_client_socket;
    free(p_client_socket); // Free the dynamically allocated socket descriptor pointer passed from main

    // Buffers for receiving command and filename parts
    char command[32];        // Increased size
    char filename_buff[256]; // Ensure matches ClientTaskArgs/FileAccessControl
    int filename_len_n, filename_len;
    int command_len_n, command_len;
    ssize_t bytes_received; // Use ssize_t for recv return value

    pthread_t worker_thread;
    ClientTaskArgs *task_args = NULL;

    // Protocol: Receive CommandLen(int), Command(char*), FilenameLen(int), Filename(char*)

    // 1. Receive Command Length
    bytes_received = recv(socket, &command_len_n, sizeof(int), MSG_WAITALL);
    if (bytes_received <= 0) { // Check for error or closed connection
        if (bytes_received == 0) printf("RequestHandler: Client disconnected before command length.\n");
        else perror("recv command length failed");
        close(socket);
        return NULL;
    }
    command_len = ntohl(command_len_n);
    if (command_len <= 0 || command_len >= sizeof(command)) {
         fprintf(stderr, "RequestHandler: Invalid command length received: %d\n", command_len);
         close(socket);
         return NULL;
    }

    // 2. Receive Command
    bytes_received = recv(socket, command, command_len, MSG_WAITALL);
     if (bytes_received <= 0) {
        if (bytes_received == 0) printf("RequestHandler: Client disconnected before command.\n");
        else perror("recv command failed");
        close(socket);
        return NULL;
    }
    command[command_len] = '\0'; // Null-terminate

    // 3. Receive Filename Length
    bytes_received = recv(socket, &filename_len_n, sizeof(int), MSG_WAITALL);
     if (bytes_received <= 0) {
        if (bytes_received == 0) printf("RequestHandler: Client disconnected before filename length.\n");
        else perror("recv filename length failed");
        close(socket);
        return NULL;
    }
    filename_len = ntohl(filename_len_n);
     if (filename_len <= 0 || filename_len >= sizeof(filename_buff)) {
         fprintf(stderr, "RequestHandler: Invalid filename length received: %d\n", filename_len);
         close(socket);
         return NULL;
    }

    // 4. Receive Filename
    bytes_received = recv(socket, filename_buff, filename_len, MSG_WAITALL);
     if (bytes_received <= 0) {
        if (bytes_received == 0) printf("RequestHandler: Client disconnected before filename.\n");
        else perror("recv filename failed");
        close(socket);
        return NULL;
    }
    filename_buff[filename_len] = '\0'; // Null-terminate

    printf("RequestHandler: Received request: Command='%s', Filename='%s'\n", command, filename_buff);

    // Prepare arguments for worker thread
    task_args = (ClientTaskArgs*)malloc(sizeof(ClientTaskArgs));
    if (task_args == NULL) {
        perror("RequestHandler: malloc ClientTaskArgs failed");
        close(socket); // Close socket as we can't handle the request
        return NULL;
    }
    task_args->client_socket = socket; // Pass the socket
    strncpy(task_args->filename, filename_buff, sizeof(task_args->filename) - 1);
    task_args->filename[sizeof(task_args->filename) - 1] = '\0';


    // 5. Dispatch based on command
    if (strcmp(command, "download") == 0) {
        printf("RequestHandler: Dispatching download task for %s\n", task_args->filename);
        if (pthread_create(&worker_thread, NULL, DownLoadingFile, task_args) != 0) {
            perror("RequestHandler: pthread_create for DownLoadingFile failed");
            free(task_args); // Clean up allocated args
            close(socket); // Close socket as we couldn't start handler
        } else {
            pthread_detach(worker_thread); // Detach thread, it will clean up itself (including task_args and socket)
        }
    } else if (strcmp(command, "upload") == 0) {
         printf("RequestHandler: Dispatching upload task for %s\n", task_args->filename);
         if (pthread_create(&worker_thread, NULL, UploadFile, task_args) != 0) {
            perror("RequestHandler: pthread_create for UploadFile failed");
            free(task_args);
            close(socket);
         } else {
            pthread_detach(worker_thread);
         }
    } else {
        fprintf(stderr, "RequestHandler: Unknown command received: %s\n", command);
        // Send error response to client?
        free(task_args); // Clean up allocated args
        close(socket); // Close socket for unknown commands
    }

    // RequestHandler thread finishes here. The socket and task_args are now managed
    // by the worker thread (if created successfully) or closed/freed on error.
    return NULL;
};

int main(){
    int server_fd;
    struct sockaddr_in server_addr;
    int opt = 1;
    int port = 8080;

    // Create server socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

     // Optional: Allow reuse of address
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) { // Removed SO_REUSEPORT for broader compatibility
        perror("setsockopt failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Prepare server address structure
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; // Listen on all available interfaces
    server_addr.sin_port = htons(port);

    // Bind socket to address and port
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Listen for incoming connections
    if (listen(server_fd, 10) < 0) { // Increased backlog queue size
        perror("Listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Server listening on port %d\n", port);

    // Main accept loop
    while(1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int client_socket;

        // Accept new connection
        client_socket = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_socket < 0) {
            perror("Accept failed");
            continue; // Continue listening for other connections
        }

        // Convert client IP to string for logging
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        printf("Accepted connection from %s:%d (socket: %d)\n", client_ip, ntohs(client_addr.sin_port), client_socket);

        // Create a thread to handle the client request
        pthread_t handler_thread;
        // Dynamically allocate memory for the client socket descriptor to pass to the thread
        int *p_client_socket = malloc(sizeof(int));
        if (p_client_socket == NULL) {
            perror("Failed to allocate memory for client socket pointer");
            close(client_socket); // Close the accepted socket
            continue; // Continue listening
        }
        *p_client_socket = client_socket;

        if (pthread_create(&handler_thread, NULL, RequestHandler, p_client_socket) != 0) {
            perror("Failed to create handler thread");
            free(p_client_socket); // Free the allocated memory
            close(client_socket); // Close the accepted socket
        } else {
             // Detach the handler thread so it cleans up automatically on exit
             // The RequestHandler is responsible for freeing p_client_socket
             pthread_detach(handler_thread);
             printf("Dispatched handler thread for socket %d\n", client_socket);
        }
    } // End of while(1) accept loop

    // --- Cleanup (won't be reached in this infinite loop) ---
    // TODO: Implement graceful shutdown (e.g., signal handling) to reach here
    printf("Server shutting down.\n");
    close(server_fd); // Close listening socket
    // TODO: Clean up the global file list (destroy mutexes/conds, free nodes)
    // pthread_mutex_destroy(&g_file_list_mutex);
    // ... iterate g_file_list_head and destroy/free ...

    return 0;
}