#include "kernel_streams.h"
#include "util.h"
#include "tinyos.h"
#include "kernel_pipe.h"

typedef struct socket_control_block socket_cb;

typedef enum socket_type_enum{ 
    SOCKET_LISTENER,
    SOCKET_UNBOUND,
    SOCKET_PEER
}socket_type;

socket_cb* port_map[MAX_PORT + 1]; //MAX_PORT is the maximum legal port

typedef struct unbound_socket_s {

    rlnode unbound_s;

}unbound_socket;


typedef struct listener_socket_s{

    rlnode queue;
    CondVar req_available;
    
}listener_socket;


typedef struct peer_socket_s {
    
    socket_cb* peer;
    pipe_cb* write_pipe;
    pipe_cb* read_pipe;

}peer_socket;


typedef struct socket_control_block
{
    uint refcount;
    FCB* fcb;
    socket_type type;
    port_t port;
    union{
        listener_socket listener_s;
        unbound_socket unbound_s;
        peer_socket peer_s;
    };
}socket_cb;

typedef struct connection_request{
    int admitted;
    socket_cb* peer;

    CondVar connected_cv;
    rlnode queue_node;
} connection_request;


