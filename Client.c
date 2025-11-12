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

#define SERVER_IP "192.5.1.1"
#define SERVER_PORT 9080

#define MULTICAST_IP "239.0.0.1"
#define MULTICAST_PORT 12345

#define BUFFER_SIZE 1024
#define MAX_CLIENTS 100

#define MAX_FILES 100
#define FILE_CONTENT_SIZE 1024



int server_sock = -1;
int multicast_sock = -1;
int server_side_sock = -1;
char file_storage[MAX_FILES][FILE_CONTENT_SIZE];
ClientInfo clients[MAX_CLIENTS];
int client_count = 0;
int multicast_sock;
int my_id = -1;
char my_token[TOKEN_SIZE] = "TOKEN-NONE"; // Default token
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t token_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t pending_mutex = PTHREAD_MUTEX_INITIALIZER;

PendingRequest pending_request;


struct sockaddr_in multicast_addr;


void sigint_handler(int signum) {
    printf("\nCaught SIGINT, notifying others and cleaning up...\n");

    // Prepare MSG_CLIENT_OFF with token
    Message off_msg;
    memset(&off_msg, 0, sizeof(off_msg));
    off_msg.type = MSG_CLIENT_OFF;
    off_msg.sender_id = my_id;

    pthread_mutex_lock(&token_mutex);
    strncpy(off_msg.token, my_token, TOKEN_SIZE);
    pthread_mutex_unlock(&token_mutex);

    // 1. Send to all connected clients via TCP
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < client_count; ++i) {
        send(clients[i].tcp_sock, &off_msg, sizeof(off_msg), 0);
    }
    pthread_mutex_unlock(&clients_mutex);

    // 2. Send to server via TCP
    if (server_sock >= 0) {
        send(server_sock, &off_msg, sizeof(off_msg), 0);
    }

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

    if (server_side_sock >= 0) {
        close(server_side_sock);
        server_side_sock = -1;
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


// Creates and sets up a multicast socket. Returns the socket descriptor or -1 on failure.
int setup_multicast_socket(const char* mcast_ip, uint16_t mcast_port, const char* iface_ip) {

	int sock;
	struct sockaddr_in mcast_addr;
	struct ip_mreq mreq;
	int reuse = 1;

	// 1. Create the UDP socket
	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		perror("socket creation failed");
		return -1;
	}

	// 2. Allow address reuse
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
		perror("setsockopt SO_REUSEADDR failed");
		close(sock);
		return -1;
	}

	// 3. Bind the socket to the multicast port on all interfaces
	memset(&mcast_addr, 0, sizeof(mcast_addr));
	mcast_addr.sin_family = AF_INET;
	mcast_addr.sin_port = htons(mcast_port);
	mcast_addr.sin_addr.s_addr = htonl(INADDR_ANY); // Receive on all interfaces

	if (bind(sock, (struct sockaddr*)&mcast_addr, sizeof(mcast_addr)) < 0) {
		perror("bind failed");
		close(sock);
		return -1;
	}

	// 4. Join the multicast group on the given interface
	mreq.imr_multiaddr.s_addr = inet_addr(mcast_ip); // Multicast group
	mreq.imr_interface.s_addr = inet_addr(iface_ip); // Interface to join on

	if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
		perror("setsockopt IP_ADD_MEMBERSHIP failed");
		close(sock);
		return -1;
	}

	return sock; // Success
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
        msg.port = 5000;
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

                        int sock = connect_to_client(&sender_addr);
			if (sock >= 0) {
			    add_client(sock, &sender_addr, msg.sender_id);
			} else {
			    // Connection failed; already handled inside connect_to_client
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
            
            int peer_sock = -1;
	pthread_mutex_lock(&clients_mutex);
	for (int i = 0; i < client_count; ++i) {
	    if (clients[i].sender_id == msg.sender_id) {
		peer_sock = clients[i].tcp_sock;
		break;
	    }
	}
	pthread_mutex_unlock(&clients_mutex);

	if (peer_sock >= 0) {
	    send(peer_sock, &response, sizeof(response), 0);
	} else {
	    printf("DEBUG: Could not find socket for client ID %d\n", msg.sender_id);
	}

            if (requested_index >= 0 && requested_index < MAX_FILES && strlen(file_storage[requested_index]) > 0) {
                response.type = MSG_DATA_RESPONSE;
                int max_content_len = sizeof(response.payload) - 12;
                snprintf(response.payload, sizeof(response.payload), "%d:%.*s",
                         requested_index, max_content_len, file_storage[requested_index]);

                pthread_mutex_lock(&token_mutex);
                strncpy(response.token, my_token, TOKEN_SIZE);
                pthread_mutex_unlock(&token_mutex);

                send(peer_sock, &response, sizeof(response), 0);

                printf("[Client] Sent file %d to peer.\n", requested_index);
            } else {
                response.type = MSG_DATA_NOT_FOUND;
                snprintf(response.payload, sizeof(response.payload), "NO");

                pthread_mutex_lock(&token_mutex);
                strncpy(response.token, my_token, TOKEN_SIZE);
                pthread_mutex_unlock(&token_mutex);

                send(peer_sock, &response, sizeof(response), 0);

                printf("[Client] I don't have file %d. Sent NO.\n", sock);
            }
        }
        else if (msg.type == MSG_TOKEN) {
            strncpy(my_token, msg.token, TOKEN_SIZE);
            my_token[TOKEN_SIZE - 1] = '\0';
            printf("DEBUG: Updated my token: '%s'\n", my_token);
        }
        else if (msg.type == MSG_SERVER_OFF) {
            printf("DEBUG: Received MSG_SERVER_OFF from server. Shutting down.\n");
            raise(SIGINT);
            break;
        }
        else if (msg.type == MSG_CLIENT_OFF) {
            printf("DEBUG: Received MSG_CLIENT_OFF from client ID %d. Validating token...\n", msg.sender_id);
            pthread_mutex_lock(&token_mutex);
            int token_valid = strncmp(msg.token, my_token, TOKEN_SIZE) == 0;
            pthread_mutex_unlock(&token_mutex);

            if (!token_valid) {
                printf("WARNING: Received MSG_CLIENT_OFF with invalid token. Ignoring.\n");
                continue;
            }

            pthread_mutex_lock(&clients_mutex);
            for (int i = 0; i < client_count; ++i) {
                if (clients[i].sender_id == msg.sender_id) {
                    printf("Client ID %d is disconnecting (token valid). Removing from list.\n", msg.sender_id);
                    close(clients[i].tcp_sock);
                    clients[i] = clients[--client_count];
                    break;
                }
            }
            pthread_mutex_unlock(&clients_mutex);
        }
        else if (msg.type == MSG_DATA_RESPONSE || msg.type == MSG_DATA_NOT_FOUND) {
            pthread_mutex_lock(&pending_mutex);

            int resp_file_index = -1;
            sscanf(msg.payload, "%d:", &resp_file_index);

            printf("DEBUG: Received response for file %d, pending request file %d, msg type %d\n",
                   resp_file_index, pending_request.file_index, msg.type);

            if (resp_file_index == pending_request.file_index || msg.type == MSG_DATA_NOT_FOUND) {
                if (msg.type == MSG_DATA_RESPONSE) {
                    pthread_mutex_lock(&token_mutex);
                    if (strncmp(msg.token, my_token, TOKEN_SIZE) == 0) {
                        char *data_start = strchr(msg.payload, ':');
                        if (data_start) {
                            data_start++; // skip ':'
                            strncpy(pending_request.data, data_start, FILE_CONTENT_SIZE - 1);
                            pending_request.data[FILE_CONTENT_SIZE - 1] = '\0';
                            pending_request.received = 1;
                        } else {
                            strncpy(pending_request.data, msg.payload, FILE_CONTENT_SIZE - 1);
                            pending_request.data[FILE_CONTENT_SIZE - 1] = '\0';
                            pending_request.received = 1;
                        }
                    } else {
                        printf("Invalid token in response. Ignoring.\n");
                    }
                    pthread_mutex_unlock(&token_mutex);
                } else {
                    // MSG_DATA_NOT_FOUND
                    printf("NO");
                    pending_request.received = -1;
                }
            }

            pthread_mutex_unlock(&pending_mutex);
        }
        else {
            printf("DEBUG: Unknown message type received: %d\n", msg.type);
        }
    }

    close(sock);
    printf("DEBUG: Closed socket %d\n", sock);
    return NULL;
}




void* data_request_thread(void* arg) {
    server_sock = *(int*)arg;
    char input[32];
    Message msg;

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
        if (file_index < 0 || file_index >= MAX_FILES) {
            printf("Invalid file index. Please enter a valid number between 0 and %d.\n", MAX_FILES-1);
            continue;
        }

        if (strlen(file_storage[file_index]) > 0) {
            printf("[Client] File %d is already available locally.\n", file_index);
            printf("%s\n", file_storage[file_index]);
            continue;  // Skip request
        }

        int found = 0;

        pthread_mutex_lock(&clients_mutex);
        for (int i = 0; i < client_count; i++) {
            int peer_sock = clients[i].tcp_sock;

            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_DATA_REQUEST;
            msg.sender_id = my_id;
            snprintf(msg.payload, sizeof(msg.payload), "%d", file_index);
            send(peer_sock, &msg, sizeof(msg), 0);

            pthread_mutex_lock(&pending_mutex);
            pending_request.file_index = file_index;
            pending_request.received = 0;
            pthread_mutex_unlock(&pending_mutex);



            // Wait for response with timeout (2 seconds max)
            int j;
            for (j = 0; j < 20; j++) {
                usleep(100000);
                pthread_mutex_lock(&pending_mutex);
                if (pending_request.received != 0) {
                    pthread_mutex_unlock(&pending_mutex);
                    break;
                }
                pthread_mutex_unlock(&pending_mutex);
            }

            pthread_mutex_lock(&pending_mutex);
            if (pending_request.received == 1) {
                strncpy(file_storage[file_index], pending_request.data, FILE_CONTENT_SIZE - 1);
                file_storage[file_index][FILE_CONTENT_SIZE - 1] = '\0';
                pthread_mutex_unlock(&pending_mutex);
                printf("[Client] Received file %d from peer %d.\n", file_index, clients[i].sender_id);
                found = 1;
                break;
            }
            pthread_mutex_unlock(&pending_mutex);
        }
        pthread_mutex_unlock(&clients_mutex);

        if (!found) {
            printf("[Client] No peer had the file. Asking server...\n");

            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_DATA_REQUEST;
            msg.sender_id = my_id;
            snprintf(msg.payload, sizeof(msg.payload), "%d", file_index);
            send(server_sock, &msg, sizeof(msg), 0);

            pthread_mutex_lock(&pending_mutex);
            pending_request.file_index = file_index;
            pending_request.received = 0;
            pthread_mutex_unlock(&pending_mutex);

            // Wait for server response
            for (int j = 0; j < 20; j++) {
                usleep(100000);
                pthread_mutex_lock(&pending_mutex);
                if (pending_request.received != 0) {
                    pthread_mutex_unlock(&pending_mutex);
                    found = 1;
                    break;
                }
                pthread_mutex_unlock(&pending_mutex);
            }
        }

        pthread_mutex_lock(&pending_mutex);
        if (pending_request.received == 1) {
            strncpy(file_storage[file_index], pending_request.data, FILE_CONTENT_SIZE - 1);
            file_storage[file_index][FILE_CONTENT_SIZE - 1] = '\0';
            printf("[Client] Received file: %s\n", file_storage[file_index]);
        } else {
            printf("[Client] File not found on peers or server.\n");
        }
        pthread_mutex_unlock(&pending_mutex);
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
    multicast_sock = setup_multicast_socket("239.0.0.1", 12345, "0.0.0.0");
    if (multicast_sock < 0) return 1;

    // Step 3: Set up TCP server to listen for incoming connections
    server_side_sock = setup_tcp_server(5000);
    if (server_side_sock < 0) return 1;

    // Step 4: Start multicast hello and keep-alive threads
    pthread_t hello_thread, keep_alive_thread, data_request_thread_id;
    pthread_create(&hello_thread, NULL, multicast_thread, NULL);
    pthread_create(&keep_alive_thread, NULL, keep_alive_thread_func, NULL);

    // **Start data request thread here**
    pthread_create(&data_request_thread_id, NULL, data_request_thread, create_sock_ptr(server_sock));

    // Step 5: Handle incoming client connections (main thread loop)
    while (1) {
        int client_sock = accept(server_side_sock, NULL, NULL);

        if (client_sock < 0) {
            perror("DEBUG: Accept failed");
            continue;
        }

        pthread_t client_thread;
        pthread_create(&client_thread, NULL, tcp_handler, create_sock_ptr(client_sock));
    }

    close(server_sock);
    close(multicast_sock);
    close(server_side_sock);

    return 0;
}
