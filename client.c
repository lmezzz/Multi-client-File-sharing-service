#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h> // Added for send/recv
#include <sys/fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h> // For error checking

#define CHUNK_SIZE 128 // Assuming this matches the server

// Function prototypes
void DownloadFileFromServer(int socket, const char* local_filename);
void UploadFileToServer(int socket, const char* local_filename);
void RequestGenerator(int socket);

// Renamed and corrected function to download a file from the server
void DownloadFileFromServer(int socket, const char* local_filename) {
    printf("Attempting to download to: %s\n", local_filename);
    int fd = open(local_filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        perror("Failed to open local file for writing");
        // We should probably close the socket or signal an error,
        // but for now, just return. The server will likely timeout or error.
        return;
    }

    char buff[CHUNK_SIZE];
    int chunk_size_n;
    ssize_t chunk_size; // Use ssize_t for sizes
    ssize_t bytes_received_data;
    ssize_t bytes_written_total;
    ssize_t bytes_written_now;

    while (true) {
        // 1. Receive chunk size from server
        bytes_received_data = recv(socket, &chunk_size_n, sizeof(int), MSG_WAITALL);
        if (bytes_received_data <= 0) {
            if (bytes_received_data == 0) {
                printf("Download: Server disconnected before chunk size.\n");
            } else {
                perror("Download: recv chunk size failed");
            }
            break; // Error or disconnect
        }

        chunk_size = ntohl(chunk_size_n);

        // Check for end-of-download signal (size 0)
        if (chunk_size == 0) {
            printf("Download: Received end-of-download signal.\n");
            break; // Normal end of download
        }

        // Validate chunk size (optional but good practice)
        if (chunk_size < 0 || chunk_size > CHUNK_SIZE) {
            fprintf(stderr, "Download: Invalid chunk size received: %zd\n", chunk_size);
            break; // Invalid size
        }

        // 2. Receive chunk data from server
        // Use the socket, not fd!
        bytes_received_data = recv(socket, buff, chunk_size, MSG_WAITALL);
        if (bytes_received_data <= 0) {
             if (bytes_received_data == 0) {
                printf("Download: Server disconnected during data receive.\n");
            } else {
                perror("Download: recv chunk data failed");
            }
            break; // Error or disconnect
        }
        // Check if received data matches expected chunk size (less critical with MSG_WAITALL)
         if (bytes_received_data != chunk_size) {
             fprintf(stderr, "Download: Received data size (%zd) != expected chunk size (%zd)\n", bytes_received_data, chunk_size);
             // Continue trying to write what was received, or break? For simplicity, break.
             break;
         }


        // 3. Write received data to local file (fd)
        bytes_written_total = 0;
        while (bytes_written_total < bytes_received_data) {
            bytes_written_now = write(fd, buff + bytes_written_total, bytes_received_data - bytes_written_total);
            if (bytes_written_now < 0) {
                perror("Download: write to local file failed");
                // Error during write, cleanup and exit loop
                close(fd);
                return; // Exit function on write error
            }
            bytes_written_total += bytes_written_now;
        }
         printf("Download: Written %zd bytes to %s\n", bytes_written_total, local_filename);

    } // End while loop

    close(fd);
    printf("Download finished for %s.\n", local_filename);
}


// Implementation for Upload function
void UploadFileToServer(int socket, const char* local_filename) {
    printf("Attempting to upload file: %s\n", local_filename);

    int fd = open(local_filename, O_RDONLY);
    if (fd < 0) {
        perror("Upload: Failed to open local file for reading");
        // Cannot proceed, maybe send an error signal to server?
        // For now, just return. Server might timeout or handle missing data.
        // Sending a 0-size chunk might be interpreted as success by the server.
        // It's better if the server expects data after UPLOAD command.
        return;
    }

    char buff[CHUNK_SIZE];
    ssize_t bytes_read;
    ssize_t bytes_sent_total;
    ssize_t bytes_sent_now;
    int chunk_size_n;

    while (true) {
        // 1. Read a chunk from the local file
        bytes_read = read(fd, buff, CHUNK_SIZE);

        if (bytes_read < 0) {
            perror("Upload: Failed to read from local file");
            // Error reading file, break loop and send 0 size? Or just close?
            // Let's break and proceed to send 0 size to terminate server side cleanly.
            bytes_read = 0; // Ensure we send 0 size below
            break;
        }

        // 2. Send chunk size (network byte order)
        chunk_size_n = htonl(bytes_read); // bytes_read will be 0 on EOF or error now
        bytes_sent_total = 0;
        while (bytes_sent_total < sizeof(int)) {
             bytes_sent_now = send(socket, ((char*)&chunk_size_n) + bytes_sent_total, sizeof(int) - bytes_sent_total, 0);
             if (bytes_sent_now < 0) {
                 perror("Upload: send chunk size failed");
                 close(fd);
                 return; // Cannot continue
             }
             bytes_sent_total += bytes_sent_now;
        }


        // 3. If chunk size was 0 (EOF or error), break loop *after* sending 0 size
        if (bytes_read == 0) {
            printf("Upload: Reached end of file or read error for %s.\n", local_filename);
            break;
        }

        // 4. Send chunk data
        bytes_sent_total = 0;
        while (bytes_sent_total < bytes_read) {
            bytes_sent_now = send(socket, buff + bytes_sent_total, bytes_read - bytes_sent_total, 0);
             if (bytes_sent_now < 0) {
                 perror("Upload: send chunk data failed");
                 close(fd);
                 return; // Cannot continue
             }
             bytes_sent_total += bytes_sent_now;
        }
         printf("Upload: Sent %zd bytes from %s\n", bytes_sent_total, local_filename);

    } // End while loop

    close(fd);
    printf("Upload finished for %s.\n", local_filename);
}


// Refactored function to handle user input and initiate requests
void RequestGenerator(int socket) {
    char command[32];
    char filename[256];
    char input_buffer[300]; // Buffer for combined input

    printf("Commands:\n");
    printf("  upload <local_filename> <remote_filename>\n");
    printf("  download <remote_filename> <local_filename>\n");
    printf("Enter command: ");

    // Read the whole line
    if (fgets(input_buffer, sizeof(input_buffer), stdin) == NULL) {
        printf("Error reading input.\n");
        return;
    }

    // Remove trailing newline if present
    input_buffer[strcspn(input_buffer, "\n")] = 0;

    // --- Parse the input ---
    char* token;
    char* rest = input_buffer;
    char* args[3]; // command, arg1, arg2
    int arg_count = 0;

    while ((token = strtok_r(rest, " ", &rest)) != NULL && arg_count < 3) {
        args[arg_count++] = token;
    }

    if (arg_count < 2) {
        printf("Invalid command format.\n");
        return;
    }

    strncpy(command, args[0], sizeof(command) - 1);
    command[sizeof(command) - 1] = '\0'; // Ensure null termination

    const char* remote_filename = NULL;
    const char* local_filename = NULL;

    // Determine filenames based on command
    if (strcasecmp(command, "UPLOAD") == 0) {
        if (arg_count != 3) {
             printf("UPLOAD format: UPLOAD <local_filename> <remote_filename>\n");
             return;
        }
        local_filename = args[1];
        remote_filename = args[2];
    } else if (strcasecmp(command, "DOWNLOAD") == 0) {
         if (arg_count != 3) {
             printf("DOWNLOAD format: DOWNLOAD <remote_filename> <local_filename>\n");
             return;
         }
        remote_filename = args[1];
        local_filename = args[2];
    } else {
        printf("Unknown command: %s\n", command);
        return;
    }

     // Ensure filenames are not empty
    if (strlen(local_filename) == 0 || strlen(remote_filename) == 0) {
        printf("Filenames cannot be empty.\n");
        return;
    }

    printf("Command: %s, Local: %s, Remote: %s\n", command, local_filename, remote_filename);


    // --- Send request to server according to protocol ---
    // 1. Send Command Length + Command
    int command_len = strlen(command) + 1; // Include null terminator
    int command_len_n = htonl(command_len);
    if (send(socket, &command_len_n, sizeof(command_len_n), 0) < 0) {
        perror("send command length failed"); return;
    }
    if (send(socket, command, command_len, 0) < 0) {
        perror("send command failed"); return;
    }

    // 2. Send Filename Length + Filename (Use the *remote* filename for the server)
    int filename_len = strlen(remote_filename) + 1; // Include null terminator
    int filename_len_n = htonl(filename_len);
     if (send(socket, &filename_len_n, sizeof(filename_len_n), 0) < 0) {
        perror("send filename length failed"); return;
    }
    if (send(socket, remote_filename, filename_len, 0) < 0) {
        perror("send filename failed"); return;
    }

    printf("Sent request to server: %s %s\n", command, remote_filename);

    // --- Call appropriate handler based on command ---
    if (strcasecmp(command, "UPLOAD") == 0) {
        UploadFileToServer(socket, local_filename); // Pass local filename to upload
    } else if (strcasecmp(command, "DOWNLOAD") == 0) {
        DownloadFileFromServer(socket, local_filename); // Pass local filename to save to
    }
}


int main() {
    int sck_d;
    sck_d = socket(AF_INET, SOCK_STREAM, 0);
    if (sck_d == -1){
        printf("Socket creation for the main socket failed");
        exit(EXIT_FAILURE);
    }
    

    int port = 8080;
    struct sockaddr_in addr;
    
    addr.sin_family = AF_INET;//ipv4
    addr.sin_port = htons(port);
    inet_pton(AF_INET , "172.31.153.78" , &addr.sin_addr);

    int connected = connect(sck_d , (struct sockaddr *)&addr , sizeof(addr));
    if (connected < 0)
    {
        printf("Error Connecting to the Server");
        close(sck_d);
        exit(EXIT_FAILURE);
    }

    RequestGenerator(sck_d);
    printf("Done");
    
    close(sck_d);
    return 0;
}