#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include "message.h"
#include <signal.h>


#define PORT 9080
#define MAX_CLIENTS 100
#define BUFFER_SIZE 1024
#define MAX_FILES 10


char* available_files[MAX_FILES] = {"file0.txt", "file1.txt", "file2.txt"};


int welcome_socket;
int client_sockets[MAX_CLIENTS];
int client_ids[MAX_CLIENTS];       // New: stores unique ID per client
int client_id_counter = 1;         // New: starts from 1
int client_count = 0;
char latest_token[100] = "TOKEN-NONE";

pthread_mutex_t client_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t token_mutex = PTHREAD_MUTEX_INITIALIZER;



void* accept_clients(void* arg) {
    (void)arg;
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    Message msg;

    while (1) {
        int client_sock = accept(welcome_socket, (struct sockaddr*)&client_addr, &addr_len);
        if (client_sock < 0) {
            perror("accept");
            continue;
        }

        pthread_mutex_lock(&client_mutex);
        if (client_count < MAX_CLIENTS) {
            int assigned_id = client_id_counter++;
            client_sockets[client_count] = client_sock;
            client_ids[client_count] = assigned_id;

            printf("New client connected: ID=%d, %s:%d (socket %d)\n",
                   assigned_id, inet_ntoa(client_addr.sin_addr),
                   ntohs(client_addr.sin_port), client_sock);

            // Prepare and send struct with ID and token
            Message msg;
            msg.sender_id = assigned_id;
            msg.type = MSG_ASSIGN_ID;

            pthread_mutex_lock(&token_mutex);
            strncpy(msg.token, latest_token, sizeof(msg.token));
            pthread_mutex_unlock(&token_mutex);

            if (send(client_sock, &msg, sizeof(msg), 0) < 0) {
                perror("send struct to client");
            } else {
                printf("Sent ID=%d and token='%s' to client\n", msg.sender_id, msg.token);
            }

            client_count++;
        } else {
            printf("Max clients reached. Rejecting connection.\n");
            close(client_sock);
        }
        pthread_mutex_unlock(&client_mutex);
    }
    return NULL;
}

void* token_broadcast(void* arg) {
    (void)arg;
    while (1) {
        time_t now = time(NULL);
        char token[100];
        snprintf(token, sizeof(token), "TOKEN-%ld", now);

        pthread_mutex_lock(&token_mutex);
        strncpy(latest_token, token, sizeof(latest_token));
        pthread_mutex_unlock(&token_mutex);

        // Create struct with ID = -1 and latest token
        Message msg;
        msg.port = 9000;
        msg.type = MSG_TOKEN;
        strncpy(msg.token, latest_token, sizeof(msg.token));

        pthread_mutex_lock(&client_mutex);
        for (int i = 0; i < client_count; i++) {
            int sock = client_sockets[i];
            if (send(sock, &msg, sizeof(msg), 0) < 0) {
                perror("send token struct failed");
            } else {
                printf("Broadcasted token to client ID=%d (socket %d): %s\n",
                       client_ids[i], sock, msg.token);
            }
        }
        pthread_mutex_unlock(&client_mutex);
        sleep(120);
    }
    return NULL;
}
void* handle_client_messages(void* arg) {
    (void)arg;

    while (1) {
        pthread_mutex_lock(&client_mutex);
        fd_set readfds;
        FD_ZERO(&readfds);
        int max_fd = 0;

        for (int i = 0; i < client_count; i++) {
            FD_SET(client_sockets[i], &readfds);
            if (client_sockets[i] > max_fd)
                max_fd = client_sockets[i];
        }
        pthread_mutex_unlock(&client_mutex);
        struct timeval tv = {3, 0};
        int activity = select(max_fd + 1, &readfds, NULL, NULL, &tv);
        if (activity < 0) {
            perror("select");
            continue;
        }

        pthread_mutex_lock(&client_mutex);
        for (int i = 0; i < client_count; i++) {
            int sock = client_sockets[i];
            if (FD_ISSET(sock, &readfds)) {
                Message msg;

                int bytes = recv(sock, &msg, sizeof(msg), 0);

                if (bytes <= 0) {
                    printf("Client ID=%d disconnected (socket %d)\n", client_ids[i], sock);
                    close(sock);
                    client_sockets[i] = client_sockets[client_count - 1];
                    client_ids[i] = client_ids[client_count - 1];
                    client_count--;
                    i--;
                } else {

                    if (msg.type == MSG_DATA_REQUEST) {
                        int requested_index = atoi(msg.payload);
                        char filename[64];
                        snprintf(filename, sizeof(filename), "file%d.txt", requested_index);

                        FILE* f = fopen(filename, "r");
                        if (!f) {
                            printf("[Server] File %s not found\n", filename);
                            Message resp = {0};
                            resp.type = MSG_DATA_RESPONSE;
                            resp.sender_id = 0;
                            snprintf(resp.payload, sizeof(resp.payload), "%d:", requested_index);
                            send(sock, &resp, sizeof(resp), 0);
                            continue;
                        }

                        char file_content[PAYLOAD_SIZE - 20] = {0};
                        size_t read_bytes = fread(file_content, 1, sizeof(file_content) - 1, f);
                        fclose(f);

                        Message resp = {0};
                        resp.type = MSG_DATA_RESPONSE;
                        resp.sender_id = 0;
                        snprintf(resp.payload, sizeof(resp.payload), "%d:%.*s",
                                 requested_index, (int)read_bytes, file_content);

// Add the current token to the message
                        pthread_mutex_lock(&token_mutex);
                        strncpy(resp.token, latest_token, sizeof(resp.token));
                        pthread_mutex_unlock(&token_mutex);

                        send(sock, &resp, sizeof(resp), 0);
                        printf("[Server] Sent data for file index %d with token '%s'\n", requested_index, resp.token);
                    }
                }
            }
        }
        pthread_mutex_unlock(&client_mutex);
    }

    return NULL;
}


void handle_sigint(int sig) {
    (void)sig;  // suppress unused warning

    printf("\nCaught SIGINT, shutting down server...\n");

    pthread_mutex_lock(&client_mutex);
    for (int i = 0; i < client_count; i++) {
        close(client_sockets[i]);
        printf("Closed client socket %d\n", client_sockets[i]);
    }
    client_count = 0;
    pthread_mutex_unlock(&client_mutex);

    close(welcome_socket);
    printf("Closed welcome socket\n");

    exit(0);  // exit gracefully
}


int main() {


    signal(SIGINT, handle_sigint);

    struct sockaddr_in server_addr;

    // Create welcome socket
    welcome_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (welcome_socket < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // Allow address reuse
    int opt = 1;
    setsockopt(welcome_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind socket
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(welcome_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(welcome_socket);
        exit(EXIT_FAILURE);
    }

    // Listen on welcome socket
    if (listen(welcome_socket, 5) < 0) {
        perror("listen");
        close(welcome_socket);
        exit(EXIT_FAILURE);
    }

    printf("Welcome socket created. Server listening on port %d...\n", PORT);
    printf("Available files:\n");
    for (int i = 0; i < MAX_FILES && available_files[i] != NULL; i++) {
        printf("%d: %s\n", i, available_files[i]);
    }


    // Create threads
    pthread_t accept_thread, message_thread, token_thread;
    pthread_create(&accept_thread, NULL, accept_clients, NULL);
    pthread_create(&message_thread, NULL, handle_client_messages, NULL);
    pthread_create(&token_thread, NULL, token_broadcast, NULL);

    pthread_join(accept_thread, NULL);
    pthread_join(message_thread, NULL);
    pthread_join(token_thread, NULL);

    close(welcome_socket);

    return 0;
}

