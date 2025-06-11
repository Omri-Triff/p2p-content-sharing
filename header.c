#ifndef MESSAGE_H
#define MESSAGE_H

#include <stdint.h>
#include <netinet/in.h>
#include <time.h>

#define BUFFER_SIZE 1024
#define TOKEN_SIZE 100
#define PAYLOAD_SIZE 512

// Enum to define the different types of messages exchanged
typedef enum {
    MSG_HELLO = 1,         // Multicast discovery
    MSG_KEEP_ALIVE,        // Heartbeat to maintain connection
    MSG_ACK,               // Acknowledgment (optional)
    MSG_DATA_REQUEST,      // Request data from client/server
    MSG_DATA_RESPONSE,
    MSG_DATA_NOT_FOUND,     // Response to data request
    MSG_ASSIGN_ID,         // Server assigns client ID and token
    MSG_CUSTOM,             // Custom/extended message
    MSG_TOKEN
} MessageType;

// Message structure used across TCP and UDP communication
typedef struct {
    MessageType type;          // Type of message
    int32_t sender_id;         // Unique client ID
    int32_t port;              // Used in HELLO messages to advertise TCP port
    char token[TOKEN_SIZE];    // Token for authentication/authorization
    char payload[BUFFER_SIZE
                 - sizeof(MessageType)
                 - 2 * sizeof(int32_t)
                 - TOKEN_SIZE];  // Additional data
} Message;

// Client information for server/client to manage connections
typedef struct {
    int tcp_sock;                  // TCP socket descriptor
    struct sockaddr_in addr;      // Address of the client
    time_t last_ack;              // Last time KEEP_ALIVE was received
    int sender_id;                // Unique client ID
    char token[TOKEN_SIZE];       // Token assigned by server
} ClientInfo;

#endif // MESSAGE_H

