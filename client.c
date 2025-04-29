#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<string.h>
#include<stdbool.h>


#include<sys/types.h>
#include<sys/fcntl.h>

#include<netinet/in.h>

#include<arpa/inet.h>

void DownloadFile(int socket){
    int fd = open("DowloadedFile.txt" , O_WRONLY | O_CREAT | O_TRUNC, 0666);
    char buff[128];
    int reade;
    int bytes_recieved;
    while (true){
        recv(socket , &reade , sizeof(reade) , MSG_WAITALL);
        printf("recieved");
        bytes_recieved = ntohl(reade);
        if (bytes_recieved <= 0)
        {
            break;
        }
        
        recv(socket , buff , bytes_recieved , MSG_WAITALL);
        printf("%s \n" , buff);
        ssize_t bytes_written = write(fd , buff , bytes_recieved);
        if (bytes_written < 0) {
            perror("Client Error: write to DowloadedFile.txt failed");
            // Handle error appropriately, maybe break the loop?
            break;
        } else if (bytes_written != bytes_recieved) {
            fprintf(stderr, "Client Warning: Partial write (%zd / %d bytes)\n", bytes_written, bytes_recieved);
            // Handle partial write if necessary
        } else {
             printf("datawritten in file"); // Only print if write succeeded
        }
        printf("datawritten in file");
    }
    close(fd);

}

void RequestGenerator(int socket){
    printf("Welcome..");
    sleep(1);
    printf("u can now start writing Commands \n");
    char comm[64];
    fflush(stdin);
    fgets(comm , sizeof(comm) , stdin);

    int bytes_in_string = (strlen(comm) + 1);
    int bytes_in_string_n = htonl (bytes_in_string);

    send(socket , &bytes_in_string_n , sizeof(bytes_in_string_n),0);
    send(socket , comm , strlen(comm) + 1 , 0);

    char req_type;
    recv(socket , &req_type , sizeof(req_type) , 0);//the server will tell wether the req is read or write
    bool iswrite;
    if (req_type == '1')
    {
        iswrite = true;
    }else{
        iswrite = false;
    }
    if (iswrite)
    {
        DownloadFile(socket);
    }
    
    
}



int main(){
    int sck_d;
    sck_d = socket(AF_INET , SOCK_STREAM , 0);
    if (sck_d == -1){
        printf("Socket creation for the main socket failed");
        exit(EXIT_FAILURE);
    }
    

    int port = 8080;
    struct sockaddr_in addr;
    
    addr.sin_family = AF_INET;//ipv4
    addr.sin_port = htons(port);
    inet_pton(AF_INET , "192.168.18.37" , &addr.sin_addr);

    int connected = connect(sck_d , (struct sockaddr *)&addr , sizeof(addr));
    if (connected < 0)
    {
        printf("Error Connecting to the Server");
        close(sck_d);
        exit(EXIT_FAILURE);
    }

    RequestGenerator(sck_d);
    printf("Doene");
    
    close(sck_d);
    return 0;
}