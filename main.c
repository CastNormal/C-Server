#include <stdio.h>  // console input/output, perror
#include <stdlib.h> // exit
#include <string.h> // string manipulation
#include <netdb.h>  // getnameinfo

#include <sys/socket.h> // socket APIs
#include <netinet/in.h> // sockaddr_in
#include <unistd.h>     // open, close

#include <signal.h> // signal handling
#include <time.h>   // time

#include <pthread.h>

#define PORT 8080
#define BUFFER_SIZE 4096
#define FILE_NAME_SIZE 256
#define CONTENT_TYPE_SIZE 64

typedef struct {
    char *buffer;
    uint16_t header_offset;
    uint16_t body_offset;
    uint buffer_size;
} RESPONSE_BUFFER;

void *handle_client(void *arg);
void free_res_buffer(RESPONSE_BUFFER *res);
int create_res_buffer(RESPONSE_BUFFER *res, char *recv_buffer);
int get_file_contents(RESPONSE_BUFFER *res, char *file_name);
int build_header(RESPONSE_BUFFER *res, char *file_name);

int main(int argc, char *argv[]) {
    int sockfd;
    static struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    serv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) { //AF_INET == IPV4, SOCK_STREAM == TCP
        perror("server failed");
        exit(EXIT_FAILURE);
    }

    if (bind(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("failed to bind server to ");
    }

    if (listen(sockfd, 10) < 0) {
        exit(EXIT_FAILURE);
    }

    while(1) {
        
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int *client_fd = malloc(sizeof(int));

        if ((*client_fd = accept(sockfd, (struct sockaddr_in*)&client_addr, &client_addr_len)) < 0) {
            perror("failed to connect client");
            continue;
        }

        pthread_t thread;
        pthread_create(&thread, NULL, handle_client, (void*)client_fd);
        pthread_detach(thread);

        //close(client_fd); we close it on the handle_client func
    }

    return 0;
}

void *handle_client(void *arg) {
    int client_fd = *((int*)arg);
    free(arg);

    char *recv_buffer = malloc(BUFFER_SIZE);
    int valread = read(client_fd, recv_buffer, BUFFER_SIZE);
    printf("recv_buffer: %s\n", recv_buffer);

    RESPONSE_BUFFER *res = malloc(sizeof(RESPONSE_BUFFER));
    if (create_res_buffer(res, recv_buffer) == 1) {
        printf("Failed to create response buffer");
        return 0;
    }
    printf("send_buffer: %s\n", res->buffer);
    int valwrite = write(client_fd, res->buffer, strlen(res->buffer));
    if (valwrite < 0) {
        perror("webserver (write)");
        return 0;
    };

    free(recv_buffer);
    free(res->buffer);
    free(res);
    close(client_fd);
    return 0;
}

int create_res_buffer(RESPONSE_BUFFER *res, char *recv_buffer) {
    char *file_name = malloc(FILE_NAME_SIZE);

    if (strncmp(recv_buffer, "GET", 3) != 0) return 1;

    if (strncmp(recv_buffer, "GET / HTTP/1.1", 14) == 0) {
        strcpy(file_name, "index.html");
    } else {
        strtok(recv_buffer,  "/");
        char *temp = strtok(NULL, " \n\r");
        strcpy(file_name, temp);
    }

    res->header_offset = 0;
    res->buffer = malloc(BUFFER_SIZE);
    res->buffer_size = BUFFER_SIZE;
    memset(res->buffer, 0, res->buffer_size); //important to init memory to remove any chances of it being used (took me way to long to debug)

    build_header(res, file_name);
    get_file_contents(res, file_name);

    free(file_name);
    return 0;
}

void free_res_buffer(RESPONSE_BUFFER *res) {
    free(res->buffer);
    free(res);
}

int build_header(RESPONSE_BUFFER *res, char *file_name) {
    char *extension = strchr(file_name, '.');
    if (extension == NULL) return 1;

    char content_type[CONTENT_TYPE_SIZE] = {0};

    if (!strcmp(extension, ".html")) {
        strcpy(content_type, "text/html");
    } else if (!strcmp(extension, ".css")) {
        strcpy(content_type, "text/css");
    } else if (!strcmp(extension, ".js")) {
        strcpy(content_type, "text/javascript");
    } else {
        return 1;
    }

    snprintf(res->buffer+res->header_offset, res->buffer_size,
            "HTTP/1.1 200 OK\r\n"
             "Content-Type: %s; charset=utf-8\r\n"
             "X-Content-Type-Options: nosniff\r\n"
             "X-Frame-Options: Deny\r\n"
             "\r\n", content_type);
    printf("\nhttp header:\n%s\n", res->buffer);
    res->body_offset = strlen(res->buffer);
    return 0;
}

int get_file_contents(RESPONSE_BUFFER *res, char *file_name) {
    FILE *f = fopen(file_name, "r");
    if (f == NULL) return 1;
    
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    fread(res->buffer+res->body_offset, fsize, 1, f);

    fclose(f);
    return 0;
}