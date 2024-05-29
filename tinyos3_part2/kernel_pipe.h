#include "tinyos.h"
#include "kernel_streams.h"
#include "kernel_dev.h"
#include "kernel_sched.h"
#include "kernel_cc.h"


pipe_cb* acquire_pipe_cb();

int isBuffFull(int r_pos,int w_pos);

int isBuffEmpty(int r_pos,int w_pos);

int pipe_write(void* pipecb_t, const char *buf, unsigned int n);

int pipe_read(void* pipecb_t, char *buf, unsigned int n);

int pipe_writer_close(void* _pipecb);

int pipe_reader_close(void* _pipecb);

int sys_Pipe(pipe_t* pipe);
