#include "tinyos.h"
#include "kernel_socket.h"
#include "kernel_pipe.h"
#include "kernel_cc.h"


/*the following function is used to allocate memory 
  for a socket_cb structure */
socket_cb* acquire_socket_cb(){
	return (socket_cb*)xmalloc(sizeof(socket_cb));
}



/*the following function is used to allocate memory 
  for a connection_request structure */
connection_request* acquire_request(){
	return (connection_request*)xmalloc(sizeof(connection_request));
}


/*function to decrease refcount of the given @socket_CB socket. 
  if refcount drops below 0 the socket "is not usefull" anymore, 
  so we free the socket.*/
void decref(socket_cb* socket_CB){
    socket_CB->refcount--;
    if(socket_CB->refcount < 0)
        free(socket_CB);
}



/*using @fid we find the corresponding FCB and return its stream socket.
  This function basically returns a reference to the desired socket_cb*/
socket_cb* get_socketcb(Fid_t fid) {
	FCB* fcb = get_fcb(fid); 
	if(fcb == NULL) {
		return NULL;
	}
	return fcb->streamobj;
}



/* This function implements the read operation for a socket.
   This function will return error if the @sock is not marked as a peer socket.
   (invoking this method for a non-peer socket has no meaning)
   The @sock socket is the "reader socket" 
*/
int socket_read(void* sock, char *buf, unsigned int size) {
    socket_cb * socket = (socket_cb *) sock;
    
    if (sock == NULL){return -1;} 
    
    if(socket->type != SOCKET_PEER){return -1;} 
    
    if(socket->peer_s.read_pipe == NULL){return -1;}
  
    pipe_cb* pipe_cb = socket->peer_s.read_pipe;

    return pipe_read(pipe_cb, buf, size);
}



/* This function implements the write operation for a socket.
   This function will return error if the @sock is not marked as a peer socket.
   The @sock socket is the "writer socket" 
*/
int socket_write(void* socket_t, const char *buf, unsigned int size) {
    socket_cb * scb = (socket_cb *) socket_t;
    
    if (socket_t == NULL){return -1;}

    if( scb->type != SOCKET_PEER) {return -1;}

    if(scb->peer_s.write_pipe == NULL){return -1;}

    pipe_cb* pipe_cb = scb->peer_s.write_pipe;

    return pipe_write(pipe_cb, buf, size);
}


/* Function to close a socket after we are done with it.
*/
int socket_close(void* socket){
	if(socket == NULL)
        return -1;

    //doing the needed casting to get a pointer to socket_cb struct
    socket_cb* socket_t = (socket_cb *) socket;

    switch (socket_t->type){
        case SOCKET_PEER:
            pipe_writer_close(socket_t->peer_s.write_pipe);
            pipe_reader_close(socket_t->peer_s.read_pipe);
            break;
        case SOCKET_LISTENER:
            while(!is_rlist_empty(&(socket_t->listener_s.queue))){
                rlnode_ptr node = rlist_pop_front(&(socket_t->listener_s.queue));
                free(node);
            }
            kernel_signal(&(socket_t->listener_s.req_available));//could be signal
            port_map[socket_t->port] = NULL;
            break;
        case SOCKET_UNBOUND:
            break;  
    }
	decref(socket_t);
    
    return 0;
}


/* File operations of a socket streamobj.*/
static file_ops socket_file_ops={
	.Write = socket_write,
	.Read = socket_read, 
	.Open = NULL,   
	.Close = socket_close
};



/*
This function returns a file descriptor for a new
socket object bound on a port, or NOFILE on error.*/
Fid_t sys_Socket(port_t port)
{
	//Checks if the port number is valid 
	if(port > MAX_PORT || port < 0)
		return NOFILE;

	Fid_t fd;            
	FCB* fcb;

	if(!FCB_reserve(1,&fd,&fcb))
		return NOFILE;
	
	socket_cb* socket_t = acquire_socket_cb(); 

	//Initializes the socket control block attributes
	fcb->streamobj = socket_t;
	fcb->streamfunc = &socket_file_ops;
	socket_t->fcb = fcb;
	socket_t->type = SOCKET_UNBOUND;
	socket_t->port = port; 
	socket_t->refcount = 0;

	return fd; 
}


int sys_Listen(Fid_t sock)
{
	/*getting the socket control block*/
	socket_cb* socketcb_t = get_socketcb(sock);

	//check if the socket is valid
	if(socketcb_t == NULL)
		return -1; 

	//getting sockets port and checking if its bound to a port.
	port_t port = socketcb_t->port;

	if(port == NOPORT)
		return -1;


	if(port_map[port] != NULL)
		return -1; 

	//check if its peer or listener 
	//(only an unbound socket can be marked as listener later on)
	if(socketcb_t->type != SOCKET_UNBOUND)
		return -1;


	//installing the socket to the port_map 
	port_map[port] = socketcb_t;

	//marking it as SOCKET_LISTENER 
	socketcb_t->type = SOCKET_LISTENER;
	socketcb_t->listener_s.req_available = COND_INIT;

  	rlnode_init(&socketcb_t->listener_s.queue, NULL);
	
	return 0;
}



Fid_t sys_Accept(Fid_t lsock)
{
	/*getting the socket control block*/
	socket_cb* listener_socket = get_socketcb(lsock);

	//doing the necessary checks that are discribed in the documentation 
	//(see tinyos.h file)
	if(listener_socket == NULL) {return NOFILE;}

	if(listener_socket->type != SOCKET_LISTENER) {return NOFILE;}

	if(port_map[listener_socket->port] == NULL) {return NOFILE;}

	listener_socket->refcount++;

	while (is_rlist_empty(&listener_socket->listener_s.queue) 
			&& port_map[listener_socket->port] != NULL)
	{
		kernel_wait(&listener_socket->listener_s.req_available, SCHED_IO);
	}

	if(port_map[listener_socket->port] == NULL) {
		decref(listener_socket);
		return NOFILE;
	}

	//get the request from the listener_socket's request queue
	//like this we can find the client_peer of the listener socket.
	connection_request* req = rlist_pop_front(&listener_socket->listener_s.queue)->cr;

	//get the client_peer socket (exist in req)
    socket_cb* client_peer = req->peer;

    //server socket's Fid_t (this server socket is peer to the client socket)
	Fid_t server_fid = sys_Socket(listener_socket->port);

	if(server_fid == NOFILE) {		
		listener_socket->refcount--; 
		kernel_signal(&(req->connected_cv));
		if (listener_socket->refcount < 0) {
        	free(listener_socket);
		}
		return NOFILE;
	}

    socket_cb* server_peer = get_socketcb(server_fid); 

    //constructing the pipes used for communication
	pipe_cb* pipe1 = acquire_pipe_cb();
	pipe_cb* pipe2 = acquire_pipe_cb(); 

	//needed initializations for the pipes: 	
    pipe1->w_position = 0;
	pipe1->r_position = 0;

	pipe1->has_space = COND_INIT;
	pipe1->has_data = COND_INIT;

	pipe2->w_position = 0;
	pipe2->r_position = 0;
	
	pipe2->has_space = COND_INIT;
	pipe2->has_data = COND_INIT;

	//mark pipe1's reader and writer FCBs 
	pipe1->reader = client_peer->fcb; //pipe1 reads from client's fcb
	pipe1->writer = server_peer->fcb; //pipe1 writes to server's fcb

	//mark pipe2's reader and writer FCBs 
	pipe2->reader = server_peer->fcb; //pipe2 reads from server's fcb
	pipe2->writer = client_peer->fcb; //pipe2 writes to client's fcb

	//"setup" the server sand client sockets.
    server_peer->type = SOCKET_PEER;
	server_peer->peer_s.write_pipe = pipe1;
	server_peer->peer_s.read_pipe = pipe2;
	server_peer->peer_s.peer = client_peer;
	
	client_peer->type = SOCKET_PEER;
	client_peer->peer_s.write_pipe = pipe2;
	client_peer->peer_s.read_pipe = pipe1;
	client_peer->peer_s.peer = server_peer;
	
	//mark req as admitted (set admitted "flag" equal to 1)
	req->admitted = 1; 

	decref(listener_socket);
	kernel_signal(&req->connected_cv);

	return server_fid;
}


/* Function to create a conection to a listener at a specific @port 
*/
int sys_Connect(Fid_t sock, port_t port, timeout_t timeout)
{
	//do the checks that are discribed in the tinyos.h documentation
	socket_cb* socketcb_t = get_socketcb(sock);

	if(socketcb_t == NULL) 
		return -1;

   	if(socketcb_t->type != SOCKET_UNBOUND)
  		return -1;

	if(port > MAX_PORT || port < 1)
		return -1; 

	if(socketcb_t == NULL)
		return -1; 

   	socket_cb* server_sock = port_map[port];

	if(server_sock == NULL)
		return -1; 

	if(server_sock->type != SOCKET_LISTENER)
		return -1; 

	connection_request* request = acquire_request();

	//mark it as "not admitted" (=0)
	request->admitted = 0; 

    rlnode_init(&request->queue_node, request);
    request->connected_cv = COND_INIT; 
    request->peer = socketcb_t; 

    //add the request to the listener's request queue and signal listener
    rlist_push_back(&server_sock->listener_s.queue, &request->queue_node);
    kernel_signal(&server_sock->listener_s.req_available);
  
	server_sock->refcount++;
    
    //while request is not admitted block the connect call 
	kernel_timedwait(&(request->connected_cv), SCHED_IO, timeout);    
 	
 	decref(server_sock);

 	//return -1 (error) if request is not admitted (=0)
 	//return 0 if request is admitted (=1)
    int retval = request->admitted - 1; 	

    rlist_remove(&(request->queue_node));
    free(request);

    return retval;
}


/* Function to shut down one or both directions of communication depending on the @how shutdown_mode
*/
int sys_ShutDown(Fid_t sock, shutdown_mode how) {
	socket_cb* socket = get_socketcb(sock);

	if(socket == NULL)
		return -1; 

	if(socket->type != SOCKET_PEER)
		return -1;

	switch(how) {
			case SHUTDOWN_READ:
			pipe_reader_close(socket->peer_s.read_pipe);
			socket->peer_s.read_pipe = NULL;
			break;
		case SHUTDOWN_WRITE:
	    	pipe_writer_close(socket->peer_s.write_pipe);
	    	socket->peer_s.write_pipe = NULL;
			break;
		case SHUTDOWN_BOTH:
			pipe_reader_close(socket->peer_s.read_pipe);
			pipe_writer_close(socket->peer_s.write_pipe);
			socket->peer_s.read_pipe = NULL;
	    	socket->peer_s.write_pipe = NULL;		
			break;
		default:
			return -1;
	}

	return 0; 
}
