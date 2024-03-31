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
#define THREAD_COUNT 4

typedef struct {
    char *buffer;
    uint16_t header_offset;
    uint16_t body_offset;
    uint buffer_size;
} RESPONSE_BUFFER;

typedef struct {
    char *buffer;
    uint buffer_size;
} RECIEVE_BUFFER;

typedef struct {
    pthread_t thread;
    RECIEVE_BUFFER recv;
    RESPONSE_BUFFER resp;
    int *client_fd;
} THREAD;

THREAD thread_pool[THREAD_COUNT];

void init_recieve_buffers() {
    for (int i = 0; i < THREAD_COUNT; i++) {
        thread_pool[i].recv.buffer_size = BUFFER_SIZE;
        thread_pool[i].recv.buffer = malloc(sizeof(RECIEVE_BUFFER));
        memset(thread_pool[i].recv.buffer, 0, thread_pool[i].recv.buffer_size);
    }
}

void free_recieve_buffers() {
    for (int i = 0; i < THREAD_COUNT; i++) {
        free(thread_pool[i].recv.buffer);
    }
}

void init_response_buffers() {
    for (int i = 0; i < THREAD_COUNT; i++) {
        thread_pool[i].resp.buffer = malloc(sizeof(RESPONSE_BUFFER));
        memset(thread_pool[i].resp.buffer, 0, BUFFER_SIZE);
    }
}

void free_response_buffers() {
    for (int i = 0; i < THREAD_COUNT; i++) {
        free(thread_pool[i].resp.buffer);
    }
}

struct Task {
    int *client_fd;
    struct Task *next;
};
typedef struct Task Task;

Task *head = NULL;
Task *tail = NULL;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t condition_var = PTHREAD_COND_INITIALIZER;

void enqueue(int *client_fd) {
    Task *t = malloc(sizeof(Task));
    t->client_fd = client_fd;
    t->next = NULL;
    if (tail == NULL) head = t;
    else tail->next = t;

    tail = t;
}

int *dequeue() {
    if (head == NULL) return NULL;
    int *t = head->client_fd;
    Task *temp = head;
    head = head->next;
    if (head == NULL) tail = NULL;
    free(temp);
    return t;
}

void *handle_client(void *arg);
void *thread_handler(void *arg);
int create_res_buffer(RESPONSE_BUFFER *resp, RECIEVE_BUFFER *recv);
int get_file_contents(RESPONSE_BUFFER *res, char *file_name);
int build_header(RESPONSE_BUFFER *res, char *file_name);

int sockfd;

void handle_interrupt(int signum) {
    free_response_buffers();
    free_recieve_buffers();
    close(sockfd);
    exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[]) {
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    serv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) { //AF_INET == IPV4, SOCK_STREAM == TCP
        perror("server failed");
        exit(EXIT_FAILURE);
    }

    if (bind(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("failed to bind server");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(int))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    if (listen(sockfd, 10) < 0) {
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_create(&thread_pool[i].thread, NULL, thread_handler, &thread_pool[i]);
    }

    init_recieve_buffers();
    init_response_buffers();

    signal(SIGINT, handle_interrupt);

    while(1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int *client_fd = malloc(sizeof(int));

        if ((*client_fd = accept(sockfd, (struct sockaddr_in*)&client_addr, &client_addr_len)) < 0) {
            perror("failed to connect client");
            continue;
        }

        pthread_mutex_lock(&lock);
        enqueue(client_fd);
        pthread_cond_signal(&condition_var);
        pthread_mutex_unlock(&lock);
    }

    close(sockfd);
    free_response_buffers();
    free_recieve_buffers();
    return EXIT_SUCCESS;
}

void *thread_handler(void *arg) {
    THREAD *thread = (THREAD*)arg;
    while (1) {
        pthread_mutex_lock(&lock);
        int *client_fd = dequeue();
        if (client_fd == NULL) pthread_cond_wait(&condition_var, &lock);
        pthread_mutex_unlock(&lock);
        if (client_fd != NULL) {
            thread->client_fd = client_fd;
            handle_client(thread);
        }
    }
}

void *handle_client(void *arg) {
    THREAD *thread = (THREAD*)arg;
    int client_fd = *thread->client_fd;
    free(thread->client_fd);

    int valread = read(client_fd, thread->recv.buffer, thread->recv.buffer_size);

    if (create_res_buffer(&thread->resp, &thread->recv) == 1) {
        printf("Failed to create response buffer");
        return NULL;
    }

    int valwrite = write(client_fd, thread->resp.buffer, thread->resp.buffer_size);
    if (valwrite < 0) {
        perror("Failed to send response buffer");
        return NULL;
    };

    close(client_fd);
    return NULL;
}

int create_res_buffer(RESPONSE_BUFFER *resp, RECIEVE_BUFFER *recv) {
    char *file_name = malloc(FILE_NAME_SIZE);

    if (strncmp(recv->buffer, "GET", 3) != 0) return 1;

    if (strncmp(recv->buffer, "GET / HTTP/1.1", 14) == 0) {
        strcpy(file_name, "index.html");
    } else {
        strtok(recv->buffer,  "/");
        char *temp = strtok(NULL, " \n\r");
        strcpy(file_name, temp);

        if (strncmp(file_name, "assets", 6) == 0) return 1; //if it's not in assets, the client is trying to access unrelated files
    }

    resp->header_offset = 0;
    resp->buffer = malloc(BUFFER_SIZE);
    resp->buffer_size = BUFFER_SIZE;
    memset(resp->buffer, 0, BUFFER_SIZE); //important to init memory to remove any chances of it being used (took me way to long to debug)

    build_header(resp, file_name);
    get_file_contents(resp, file_name);
    resp->buffer_size = strlen(resp->buffer);

    free(file_name);
    return 0;
}

int build_header(RESPONSE_BUFFER *resp, char *file_name) {
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

    snprintf(resp->buffer+resp->header_offset, resp->buffer_size,
            "HTTP/1.1 200 OK\r\n"
             "Content-Type: %s; charset=utf-8\r\n"
             "X-Content-Type-Options: nosniff\r\n"
             "X-Frame-Options: Deny\r\n"
             "\r\n", content_type);
    //printf("\nhttp header:\n%s\n", res->buffer);
    resp->body_offset = strlen(resp->buffer);
    return 0;
}

int get_file_contents(RESPONSE_BUFFER *resp, char *file_name) {
    FILE *f = fopen(file_name, "r");
    if (f == NULL) return 1;
    
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    fread(resp->buffer+resp->body_offset, fsize, 1, f);

    fclose(f);
    return 0;
}