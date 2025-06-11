#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <errno.h>
#include <time.h>
#include "message.h"
#include <signal.h>

#define SERVER_IP "132.72.50.204"
#define SERVER_PORT 9080

#define MULTICAST_IP "239.0.0.1"
#define MULTICAST_PORT 12345

#define BUFFER_SIZE 1024
#define MAX_CLIENTS 100

#define MAX_FILES 100
#define FILE_CONTENT_SIZE 1024



int server_sock = -1;
int multicast_sock = -1;
int tcp_server_sock = -1;
char file_storage[MAX_FILES][FILE_CONTENT_SIZE];
ClientInfo clients[MAX_CLIENTS];
int client_count = 0;
int multicast_sock;
int my_id = -1;
char my_token[TOKEN_SIZE] = "TOKEN-NONE"; // Default token
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t token_mutex = PTHREAD_MUTEX_INITIALIZER;
struct sockaddr_in multicast_addr;


void sigint_handler(int signum) {
    printf("\nCaught SIGINT, cleaning up...\n");

    if (server_sock >= 0) {
        close(server_sock);
        server_sock = -1;
    }

    if (multicast_sock >= 0) {
        // Drop multicast group membership
        struct ip_mreq mreq;
        mreq.imr_multiaddr.s_addr = inet_addr(MULTICAST_IP);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        setsockopt(multicast_sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));

        close(multicast_sock);
        multicast_sock = -1;
    }

    if (tcp_server_sock >= 0) {
        close(tcp_server_sock);
        tcp_server_sock = -1;
    }

    // Close all client sockets
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; i++) {
        close(clients[i].tcp_sock);
    }
    pthread_mutex_unlock(&clients_mutex);

    printf("All sockets closed. Exiting now.\n");
    exit(0);
}



void add_client(int sock, struct sockaddr_in *addr, int sender_id) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; ++i) {
        if (clients[i].sender_id == sender_id) {
            pthread_mutex_unlock(&clients_mutex);
            return;
        }
    }

    if (client_count < MAX_CLIENTS) {
        clients[client_count].tcp_sock = sock;
        clients[client_count].addr = *addr;
        clients[client_count].last_ack = time(NULL);
        clients[client_count].sender_id = sender_id;
        client_count++;
        printf("Added client ID %d from %s:%d\n", sender_id,
               inet_ntoa(addr->sin_addr), ntohs(addr->sin_port));
    }
    pthread_mutex_unlock(&clients_mutex);
}



// Connect to server and establish initial TCP connection
int connect_to_server() {
    printf("DEBUG: Connecting to server %s:%d...\n", SERVER_IP, SERVER_PORT);
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("DEBUG: TCP socket");
        return -1;
    }

    struct sockaddr_in server_addr = {
            .sin_family = AF_INET,
            .sin_port = htons(SERVER_PORT)
    };
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("DEBUG: connect");
        close(sock);
        return -1;
    }

    Message msg;
    if (recv(sock, &msg, sizeof(msg), 0) <= 0 || msg.type != MSG_ASSIGN_ID) {
        perror("recv");
        close(sock);
        return -1;
    }

    my_id = msg.sender_id;
    strncpy(my_token, msg.token, TOKEN_SIZE);
    printf("Assigned ID: %d, Token: %s\n", my_id, my_token);

    return sock;
}


// Join multicast group and create UDP socket
int setup_multicast() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("multicast socket");
        return -1;
    }

    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt reuseaddr");
        close(sock);
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(MULTICAST_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind multicast");
        close(sock);
        return -1;
    }

    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(MULTICAST_IP);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);

    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("setsockopt multicast join");
        close(sock);
        return -1;
    }

    // Set global multicast address for sending
    multicast_addr.sin_family = AF_INET;
    multicast_addr.sin_addr.s_addr = inet_addr(MULTICAST_IP);
    multicast_addr.sin_port = htons(MULTICAST_PORT);

    return sock;
}

// Set up the TCP server to listen for incoming connections
int setup_tcp_server(int port) {
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("ERROR: Failed to create socket");
        return -1;
    }

    struct sockaddr_in server_addr = {
            .sin_family = AF_INET,
            .sin_port = htons(port),
            .sin_addr.s_addr = INADDR_ANY
    };

    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("ERROR: Failed to bind to address");
        close(server_sock);
        return -1;
    }

    if (listen(server_sock, 5) < 0) {
        perror("ERROR: Failed to listen for connections");
        close(server_sock);
        return -1;
    }

    return server_sock;
}




// Connect to another client via TCP
int connect_to_client(struct sockaddr_in *target_addr) {
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &target_addr->sin_addr, ip_str, sizeof(ip_str));
    int port = ntohs(target_addr->sin_port);

    printf("DEBUG: Attempting to connect to client at %s:%d\n", ip_str, port);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("ERROR: Failed to create socket");
        return -1;
    }

    // Connect to the target client
    if (connect(sock, (struct sockaddr*)target_addr, sizeof(*target_addr)) < 0) {
        perror("ERROR: Connection to target client failed");
        printf("DEBUG: Could not connect to %s:%d using socket %d\n", ip_str, port, sock);
        close(sock);
        return -1;
    }

    printf("DEBUG: Successfully connected to client at %s:%d (socket %d)\n",
           ip_str, port, sock);

    return sock;
}





// Periodically send "HELLO" messages to discover other clients
void* multicast_thread(void* arg) {
    Message msg;
    struct sockaddr_in sender_addr;
    socklen_t addr_len = sizeof(sender_addr);

    // Dynamically allocate memory for known client IDs
    int* known_client_ids = malloc(sizeof(int) * MAX_CLIENTS);
    int known_client_count = 0;
    known_client_ids[known_client_count++] = my_id;  // Add self

    while (1) {
        // Send HELLO
        msg.type = MSG_HELLO;
        msg.sender_id = my_id;
        msg.port = 9050;
        sendto(multicast_sock, &msg, sizeof(msg), 0,
               (struct sockaddr*)&multicast_addr, sizeof(multicast_addr));

        // Set timeout to wait for incoming HELLOs
        struct timeval tv = {2, 0};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(multicast_sock, &fds);
        int ret = select(multicast_sock + 1, &fds, NULL, NULL, &tv);

        if (ret > 0 && FD_ISSET(multicast_sock, &fds)) {
            // Receive HELLO message
            if (recvfrom(multicast_sock, &msg, sizeof(msg), 0,
                         (struct sockaddr*)&sender_addr, &addr_len) > 0) {
                if (msg.sender_id != my_id && msg.type == MSG_HELLO) {
                    // Check if we already know this ID
                    int known = 0;
                    for (int i = 0; i < known_client_count; ++i) {
                        if (known_client_ids[i] == msg.sender_id) {
                            known = 1;
                            break;
                        }
                    }

                    if (!known) {
                        known_client_ids[known_client_count++] = msg.sender_id;
                        sender_addr.sin_port = htons(msg.port);

                        int sock = socket(AF_INET, SOCK_STREAM, 0);
                        if (connect(sock, (struct sockaddr*)&sender_addr, sizeof(sender_addr)) == 0) {
                            add_client(sock, &sender_addr, msg.sender_id);
                        } else {
                            close(sock);
                        }
                    }
                }
            }
        }

        sleep(2);
    }

    free(known_client_ids);
    return NULL;
}

void* tcp_handler(void* arg) {
    int sock = *(int*)arg;
    free(arg);
    Message msg;

    while (1) {
        int n = recv(sock, &msg, sizeof(msg), 0);
        if (n <= 0) {
            printf("DEBUG: Connection closed or error on socket %d\n", sock);
            break;
        }


        if (msg.type == MSG_KEEP_ALIVE) {
            pthread_mutex_lock(&clients_mutex);
            int found = 0;
            for (int i = 0; i < client_count; ++i) {
                if (clients[i].sender_id == msg.sender_id) {
                    clients[i].last_ack = time(NULL);
                    found = 1;
                    break;
                }
            }
            if (!found) {
                printf("DEBUG: WARNING: KEEP_ALIVE received from unknown socket %d\n", sock);
            }
            pthread_mutex_unlock(&clients_mutex);
        }

        else if (msg.type == MSG_DATA_REQUEST) {
            int requested_index = atoi(msg.payload);
            Message response;
            memset(&response, 0, sizeof(response));
            response.sender_id = my_id;

            if (strlen(file_storage[requested_index]) > 0) {
                response.type = MSG_DATA_RESPONSE;
                strncpy(response.payload, file_storage[requested_index], sizeof(response.payload) - 1);
                send(sock, &response, sizeof(response), 0);
                printf("[Client] Sent file %d to peer.\n", requested_index);
            } else {
                response.type = MSG_DATA_NOT_FOUND;
                snprintf(response.payload, sizeof(response.payload), "NO");
                send(sock, &response, sizeof(response), 0);
                printf("[Client] I don't have file %d. Sent NO.\n", requested_index);
            }
        }

        else if (msg.type == MSG_TOKEN) {
            strncpy(my_token, msg.token, TOKEN_SIZE);
            my_token[TOKEN_SIZE - 1] = '\0';  // Ensure null-termination
            printf("DEBUG: Updated my token: '%s'\n", my_token);
        }

        else if (msg.type == MSG_DATA_RESPONSE) {

            continue;

        }

        else {
            printf("DEBUG: Unknown message type received: %d\n", msg.type);
        }
    }

    close(sock);
    printf("DEBUG: Closed socket %d\n", sock);
    return NULL;
}




// Thread 3: Request data from known clients, fallback to server
void* data_request_thread(void* arg) {
    server_sock = *(int*)arg;
    char input[32];
    Message msg;
    char filename[64];

    while (1) {
        printf("\nEnter file index to request from server (or 'q' to quit): \n");
        if (fgets(input, sizeof(input), stdin) == NULL) {
            continue;
        }

        if (input[0] == 'q' || input[0] == 'Q') {
            printf("Exiting data request thread.\n");
            break;
        }

        int file_index = atoi(input);
        if (file_index < 0) {
            printf("Invalid file index. Please enter a positive integer.\n");
            continue;
        }
        int file_found = 0;  // reset

        // Try known clients first
        pthread_mutex_lock(&clients_mutex);
        for (int i = 0; i < client_count; i++) {
            int peer_sock = clients[i].tcp_sock;

            // Send request to peer
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_DATA_REQUEST;
            msg.sender_id = my_id;
            snprintf(msg.payload, sizeof(msg.payload), "%d", file_index);
            send(peer_sock, &msg, sizeof(msg), 0);

            // Wait up to 2 seconds for response using select
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(peer_sock, &read_fds);
            struct timeval timeout = {2, 0};  // 2 seconds

            int ready = select(peer_sock + 1, &read_fds, NULL, NULL, &timeout);
            if (ready > 0) {
                recv(peer_sock, &msg, sizeof(msg), 0);

                if (msg.type == MSG_DATA_RESPONSE) {
                    // Save file to memory and disk
                    printf("Received token: '%s' from client ID %d\n", msg.token, msg.sender_id);
                    pthread_mutex_lock(&token_mutex);
                    char expected_token[TOKEN_SIZE];
                    strncpy(expected_token, my_token, TOKEN_SIZE);
                    expected_token[TOKEN_SIZE - 1] = '\0';  // Ensure null termination
                    pthread_mutex_unlock(&token_mutex);

                    if (strncmp(msg.token, expected_token, TOKEN_SIZE) != 0) {
                        printf("Received file with invalid token from client ID %d\n", msg.sender_id);
                        continue; // Discard the file
                    }
                    strncpy(file_storage[file_index], msg.payload, FILE_CONTENT_SIZE - 1);
                    file_storage[file_index][FILE_CONTENT_SIZE - 1] = '\0';

                    snprintf(filename, sizeof(filename), "received_file%d.txt", file_index);
                    FILE *fp = fopen(filename, "w");
                    if (fp) {
                        fwrite(msg.payload, 1, strlen(msg.payload), fp);
                        fclose(fp);
                    }

                    printf("[Client] Received file from peer %d: %s\n", clients[i].sender_id, filename);
                    printf("%s\n", file_storage[file_index]);
                    file_found = 1;
                    break;
                } else if (msg.type == MSG_DATA_NOT_FOUND) {
                    printf("[Client] Peer %d doesn't have the file.\n", clients[i].sender_id);
                }
            } else {
                printf("[Client] Peer %d did not respond.\n", clients[i].sender_id);
            }
        }
        pthread_mutex_unlock(&clients_mutex);

        // If no peer had the file, ask the server
        if (!file_found) {
            printf("[Client] No peer had the file. Asking server...\n");

            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_DATA_REQUEST;
            msg.sender_id = my_id;
            snprintf(msg.payload, sizeof(msg.payload), "%d", file_index);
            send(server_sock, &msg, sizeof(msg), 0);

            if (recv(server_sock, &msg, sizeof(msg), 0) <= 0) {
                printf("Failed to receive data from server.\n");
                continue;
            }

            if (msg.type == MSG_DATA_RESPONSE) {
                printf("Received token: '%s' from client ID %d\n", msg.token, msg.sender_id);
                // Save in memory and disk
                pthread_mutex_lock(&token_mutex);
                char expected_token[TOKEN_SIZE];
                strncpy(expected_token, my_token, TOKEN_SIZE);
                expected_token[TOKEN_SIZE - 1] = '\0';  // Ensure null termination
                pthread_mutex_unlock(&token_mutex);

                if (strncmp(msg.token, expected_token, TOKEN_SIZE) != 0) {
                    printf("Received file with invalid token from client ID %d\n", msg.sender_id);
                    continue; // Discard the file
                }


                strncpy(file_storage[file_index], msg.payload, FILE_CONTENT_SIZE - 1);
                file_storage[file_index][FILE_CONTENT_SIZE - 1] = '\0';

                snprintf(filename, sizeof(filename), "received_file%d.txt", file_index);
                FILE *fp = fopen(filename, "w");
                if (fp) {
                    fwrite(msg.payload, 1, strlen(msg.payload), fp);
                    fclose(fp);
                }

                printf("[Client] Received file from server: %s\n", filename);
                printf("%s\n", file_storage[file_index]);
            } else {
                printf("Server returned unexpected message.\n");
            }
        }
    }

    return NULL;
}



// KEEP ALIVE MODIFICATION: keep-alive thread
void* keep_alive_thread_func(void* arg) {
    Message msg = { .type = MSG_KEEP_ALIVE, .sender_id = -1 };
    while (1) {
        sleep(10);
        time_t now = time(NULL);
        pthread_mutex_lock(&clients_mutex);
        for (int i = 0; i < client_count; ++i) {
            msg.sender_id = my_id;
            send(clients[i].tcp_sock, &msg, sizeof(msg), 0);
            if (difftime(now, clients[i].last_ack) > 40) {
                printf("DEBUG: Client ID %d timed out. Removing.\n", clients[i].sender_id);
                close(clients[i].tcp_sock);
                clients[i] = clients[--client_count];  // Shift last to current slot
                i--;  // Check the same index again
            }
        }
        pthread_mutex_unlock(&clients_mutex);
    }
    return NULL;
}



int* create_sock_ptr(int sock) {
    int *p = malloc(sizeof(int));
    *p = sock;
    return p;
}




int main() {


    signal(SIGINT, sigint_handler);

    // Step 1: Connect to server
    server_sock = connect_to_server();
    if (server_sock < 0) return 1;

    // Start the thread to handle incoming messages from server
    pthread_t server_thread;
    pthread_create(&server_thread, NULL, tcp_handler, create_sock_ptr(server_sock));

    // Step 2: Set up multicast socket
    multicast_sock = setup_multicast();
    if (multicast_sock < 0) return 1;

    // Step 3: Set up TCP server to listen for incoming connections
    tcp_server_sock = setup_tcp_server(9050);
    if (tcp_server_sock < 0) return 1;

    // Step 4: Start multicast hello and keep-alive threads
    pthread_t hello_thread, keep_alive_thread, data_request_thread_id;
    pthread_create(&hello_thread, NULL, multicast_thread, NULL);
    pthread_create(&keep_alive_thread, NULL, keep_alive_thread_func, NULL);

    // **Start data request thread here**
    pthread_create(&data_request_thread_id, NULL, data_request_thread, create_sock_ptr(server_sock));

    // Step 5: Handle incoming client connections (main thread loop)
    while (1) {
        int client_sock = accept(tcp_server_sock, NULL, NULL);
        if (client_sock < 0) {
            perror("DEBUG: Accept failed");
            continue;
        }

        pthread_t client_thread;
        pthread_create(&client_thread, NULL, tcp_handler, create_sock_ptr(client_sock));
    }

    close(server_sock);
    close(multicast_sock);
    close(tcp_server_sock);

    return 0;
}