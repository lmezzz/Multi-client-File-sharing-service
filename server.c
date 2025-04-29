#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<string.h>
#include<stdbool.h>

#include<sys/types.h>
#include<sys/fcntl.h>

#include<netinet/in.h>

#include<arpa/inet.h>

struct Request{
    char* FileName;
    char* Command;
    bool IsWrite;

};

int sendingfile(int socket , struct Request *CR){
    int fd = open(CR->FileName , O_RDONLY);
    if (fd < 0){
        printf("couldnt open file");
        return(1);
    }
    char buffer[128];
    int bytesread;
    int b_read;
    while((bytesread = read(fd , buffer , sizeof(buffer))) > 0 ){
        b_read = htonl(bytesread);
        send(socket , &b_read , sizeof(b_read) , 0);
        send(socket , buffer , bytesread , 0);
        printf("sent data");

    }
    b_read = htonl(bytesread);
    send(socket , &b_read , sizeof(b_read) , 0);
    close(fd);
    return 1;

};

void RequestHandler(int socket){
    char buff[128];
    struct Request Client_req;
    
    Client_req.Command = malloc(10);
    Client_req.FileName = malloc(20);
    Client_req.IsWrite = false;
    
    int bytesread;
    int b_read;

    recv(socket , &bytesread , sizeof(bytesread) , MSG_WAITALL);
    b_read = ntohl(bytesread);
    recv(socket , buff , b_read , MSG_WAITALL);
    
    sscanf(buff , "%s %s" , Client_req.Command , Client_req.FileName );

    printf("%s \n" , buff);

    
    if (strcmp(Client_req.Command , "download") == 0){
        Client_req.IsWrite = true;
        char snd = '1';
        send(socket , &snd , 1 , 0);
        printf("Command read");
    }

    if (Client_req.IsWrite)
    {
        sendingfile(socket , &Client_req);
    }
    
    free(Client_req.Command);
    free(Client_req.FileName);
    close(socket);
};

int main(){
    int ssck_d;
    ssck_d = socket(AF_INET , SOCK_STREAM , 0); //SOCK_STREAM -> tcp protocol 
    if (ssck_d == -1){
        printf("Socket creation for the main socket failed");
        exit(EXIT_FAILURE);
    }
    

    int port = 8080;
    struct sockaddr_in addr;
    
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY; //it can pick any connection

    bind(ssck_d , (struct sockaddr *)&addr , sizeof(addr));

    listen(ssck_d , 5);

    int csck_d = accept(ssck_d, NULL , NULL);

    if (csck_d == -1){
        printf("Error in accepting");
        exit(EXIT_FAILURE);
    }else{
        printf("Succesfull");
    }

    RequestHandler(csck_d);

    close(ssck_d);
    return 0;
}