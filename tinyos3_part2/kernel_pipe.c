#include "kernel_pipe.h"
#include "tinyos.h"
#include "kernel_streams.h"
#include "kernel_dev.h"
#include "kernel_sched.h"
#include "kernel_cc.h"


//Used to return -1 in specific file operation functions
static int dummy()
{
	return -1; 
}

static file_ops writer_file_ops=
{
	.Write = pipe_write,
	.Read = dummy, 
	.Open = NULL,   
	.Close = pipe_writer_close
};

static file_ops reader_file_ops=
{
	.Read = pipe_read,
	.Write = dummy,
	.Open = NULL,
	.Close = pipe_reader_close
};

//Allocates memory for a pipe control block.
pipe_cb* acquire_pipe_cb()
{
  pipe_cb* pipecb_t = (pipe_cb*)xmalloc(sizeof(pipe_cb));
  return pipecb_t;
}

//Returns 1 if the buffer is full and zero if not. Makes that conclusion based on the reading and writing position of the buffer.
int isBuffFull(int r_pos,int w_pos){
	if((w_pos == r_pos - 1)||(r_pos== 0 && w_pos == PIPE_BUFFER_SIZE-1))
		return 1; //is Full
	return 0; //is not Full
}

//Returns 1 if the buffer is empty and zero if not. Makes that conclusion based on the reading and writing position of the buffer.
int isBuffEmpty(int r_pos,int w_pos){
	if(r_pos==w_pos)
		return 1; //is Empty
	return 0; //is not Empty
}

int pipe_write(void* pipecb_t, const char *buf, unsigned int n) 
{
	pipe_cb* pipe = (pipe_cb*) pipecb_t;
	int written_counter = 0;

//
	if(pipe == NULL || buf == NULL){
		return -1; 
	}

	if(pipe->reader == NULL || pipe->writer == NULL){
		return -1; 
	}

	while((pipe->reader!=NULL) && (isBuffFull(pipe->r_position,pipe->w_position)) ){
		kernel_wait(&pipe->has_space,SCHED_PIPE);
	}

	if(pipe->reader == NULL){
		return -1; 
	}
	
	//copy the data.
	while (written_counter < n && (!isBuffFull(pipe->r_position,pipe->w_position))) {
	  pipe->BUFFER[pipe->w_position] = buf[written_counter];
		written_counter++;
		pipe->w_position=(pipe->w_position+1)%PIPE_BUFFER_SIZE;  /*! prosoxi an xanetai thesi kai pos epireazei tin ilopoiisi*/ 
		
	}

	kernel_broadcast(&pipe->has_data);

	return written_counter;
}


int pipe_read(void* pipecb_t, char *buf, unsigned int n) {
	int reader_counter = 0;

	pipe_cb* pipe = (pipe_cb*) pipecb_t;

	if(pipe == NULL || buf == NULL){
		return -1; 
	}

	if(pipe->reader == NULL){
		return -1; 
	}

	while(pipe->writer!=NULL && isBuffEmpty(pipe->r_position,pipe->w_position)){
		kernel_wait(&pipe->has_data,SCHED_PIPE);
	}

	if(pipe->reader == NULL){
		return -1; 
	}
	
	//copies the data.
	while (reader_counter < n && (!isBuffEmpty(pipe->r_position,pipe->w_position))) {
	  buf[reader_counter] = pipe->BUFFER[pipe->r_position];
		reader_counter++;
		pipe->r_position=(pipe->r_position+1)%PIPE_BUFFER_SIZE;  /*! prosoxi an xanetai thesi kai pos epireazei tin ilopoiisi*/ 
	}

	kernel_broadcast(&pipe->has_space);

	return reader_counter;
}
	

int pipe_writer_close(void* _pipecb)
{
	pipe_cb* pipe = (pipe_cb*) _pipecb;

	//If pipe is null returns error message
	if(pipe==NULL)
		return -1;

	//if closed from before returns error message
	if(pipe->writer == NULL)//closed from before
		return -1;

	//Close it by making pointer equal to null (no reference)
	pipe->writer = NULL; 

	//Now if both reader AND writer are null, free pipe control block
	if(pipe->reader == NULL) 
		free(pipe);

	//Notify
	kernel_broadcast(&pipe->has_data); 
	return 0;
}

int pipe_reader_close(void* _pipecb)
{
	pipe_cb* pipe = (pipe_cb*) _pipecb;

	//if pipe is null returns error message
	if(pipe==NULL)
		return -1;
	
	//if closed from before returns error message
	if(pipe->reader == NULL)
		return -1;

	//Close it by making pointer equal to null (no reference)
	pipe->reader = NULL; 

	//Now if both reader AND writer are null, free pipe control block
	if(pipe->writer == NULL) 
		free(pipe);
	
	//Notify
	kernel_broadcast(&pipe->has_space); 

	return 0;	
}

/*Creates a pipe and provides a one-way (undirectional flow of data). 
Returns 0 when successful and -1 when failure.
reserve 2 fcbs in the current process*/ 
int sys_Pipe(pipe_t* pipe)
{
	Fid_t fd[2];            
	FCB* fcb[2];

//fcb reserve for two fids.
	if(!FCB_reserve(2,fd,fcb))
		return -1;
	
	//indexes of the FIDT array (SIZE=16) in the PCB.
	//In each position we have pointer to FCB object of FT array (MAXFILES size) 
	pipe->read = fd[0];//file descriptor for reading
	pipe->write = fd[1];//file descriptor for writing

	pipe_cb* pipecb_t = acquire_pipe_cb();

	pipecb_t->reader = fcb[0];
	pipecb_t->writer = fcb[1];

	//initializing pipe control block variables
	pipecb_t->w_position = 0;
	pipecb_t->r_position = 0;

	pipecb_t->has_space = COND_INIT;
	pipecb_t->has_data = COND_INIT;

	//both FCBs modify the same pipe 
	fcb[0]->streamobj = pipecb_t;
	fcb[1]->streamobj = pipecb_t;

  //assigning the pointers to the functions.
	fcb[0]->streamfunc = &reader_file_ops;
	fcb[1]->streamfunc = &writer_file_ops;

	return 0;
}
