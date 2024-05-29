#include <assert.h>
#include "kernel_cc.h"
#include "kernel_proc.h"
#include "kernel_streams.h"
#include "kernel_sched.h"


/* 
 The process table and related system calls:
 - Exec
 - Exit
 - WaitPid
 - GetPid
 - GetPPid

 */

/* The process table */
PCB PT[MAX_PROC];
unsigned int process_count;

PCB* get_pcb(Pid_t pid)
{
  return PT[pid].pstate==FREE ? NULL : &PT[pid];
}


Pid_t get_pid(PCB* pcb)
{
  return pcb==NULL ? NOPROC : pcb-PT;
}

/* Initialize a PCB */
static inline void initialize_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->argl = 0;
  pcb->args = NULL;

  for(int i=0;i<MAX_FILEID;i++)
    pcb->FIDT[i] = NULL;

  rlnode_init(& pcb->children_list, NULL);
  rlnode_init(& pcb->exited_list, NULL);
  rlnode_init(& pcb->children_node, pcb);
  rlnode_init(& pcb->exited_node, pcb);
  pcb->child_exit = COND_INIT;
}


static PCB* pcb_freelist;

void initialize_processes()
{
  /* initialize the PCBs */
  for(Pid_t p=0; p<MAX_PROC; p++) {
    initialize_PCB(&PT[p]);
  }

  /* use the parent field to build a free list */
  PCB* pcbiter;
  pcb_freelist = NULL;
  for(pcbiter = PT+MAX_PROC; pcbiter!=PT; ) {
    --pcbiter;
    pcbiter->parent = pcb_freelist;
    pcb_freelist = pcbiter;
  }

  process_count = 0;

  /* Execute a null "idle" process */
  if(Exec(NULL,0,NULL)!=0)
    FATAL("The scheduler process does not have pid==0");
}

PTCB* acquire_PTCB()
{
  PTCB* ptcb = (PTCB*)xmalloc(sizeof(PTCB));
  return ptcb;
}
/*
  Must be called with kernel_mutex held
*/
PCB* acquire_PCB()
{
  PCB* pcb = NULL;

  if(pcb_freelist != NULL) {
    pcb = pcb_freelist;
    pcb->pstate = ALIVE;
    pcb_freelist = pcb_freelist->parent;
    process_count++;
  }

  return pcb;
}

/*
  Must be called with kernel_mutex held
*/
void release_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->parent = pcb_freelist;
  pcb_freelist = pcb;
  process_count--;
}


/*
 *
 * Process creation
 *
 */

/*
  This function is provided as an argument to spawn_thread,
  to execute the main thread of a process.
*/
void start_main_thread()
{
  int exitval;

  Task call =  CURPROC->main_task;
  int argl = CURPROC->argl;
  void* args = CURPROC->args;

  exitval = call(argl,args);
  Exit(exitval);
}

/*
  This function is provided as an argument to spawn_process_thread. 
  This implementation is for multi-threaded processes.
*/
void start_process_thread()
{
  int exitval;
    
  Task call = cur_thread()->ptcb->task;
  int argl = cur_thread()->ptcb->argl; 
  void* args = cur_thread()->ptcb->args;

  exitval = call(argl,args);
  ThreadExit(exitval);  
}

/*
  System call to create a new process.
 */
Pid_t sys_Exec(Task call, int argl, void* args)
{
  PCB *curproc, *newproc;
  
  /* The new process PCB */
  newproc = acquire_PCB();

  if(newproc == NULL) goto finish;  /* We have run out of PIDs! */

  if(get_pid(newproc)<=1) {
    /* Processes with pid<=1 (the scheduler and the init process) 
       are parentless and are treated specially. */
    newproc->parent = NULL;
  }
  else
  {
    /* Inherit parent */
    curproc = CURPROC;

    /* Add new process to the parent's child list */
    newproc->parent = curproc;
    rlist_push_front(& curproc->children_list, & newproc->children_node);

    /* Inherit file streams from parent */
    for(int i=0; i<MAX_FILEID; i++) {
       newproc->FIDT[i] = curproc->FIDT[i];
       if(newproc->FIDT[i])
          FCB_incref(newproc->FIDT[i]);
    }
  }


  /* Set the main thread's function */
  newproc->main_task = call;

  /* Copy the arguments to new storage, owned by the new process */
  newproc->argl = argl;
  if(args!=NULL) {
    newproc->args = malloc(argl);
    memcpy(newproc->args, args, argl);
  }
  else
    newproc->args=NULL;

  /* 
    Create and wake up the thread for the main function. This must be the last thing
    we do, because once we wakeup the new thread it may run! so we need to have finished
    the initialization of the PCB.
   */

  if(call != NULL) { 
    //initializing the counter of threads in this new process
    newproc->thread_count = 0;
    //initializing ptcb list of this new process
    rlnode_init(& newproc->ptcb_list, NULL);

    PTCB* firstptcb =spawn_process_thread(newproc,start_main_thread) ;

    //passingthe arguments of the process to the ptcb
    firstptcb->task = call; 
    firstptcb->argl = argl; 
    firstptcb->args = args;
    
    //put the thread of the first ptcb (of this new process)
    //in the ready queue of the scheduler
    wakeup(firstptcb->tcb); 
  }

finish:
  return get_pid(newproc);
}

/* System call */
Pid_t sys_GetPid()
{
  return get_pid(CURPROC);
}

Pid_t sys_GetPPid()
{
  return get_pid(CURPROC->parent);
}

static void cleanup_zombie(PCB* pcb, int* status)
{
  if(status != NULL)
    *status = pcb->exitval;

  rlist_remove(& pcb->children_node);
  rlist_remove(& pcb->exited_node);

  release_PCB(pcb);
}


static Pid_t wait_for_specific_child(Pid_t cpid, int* status)
{

  /* Legality checks */
  if((cpid<0) || (cpid>=MAX_PROC)) {
    cpid = NOPROC;
    goto finish;
  }

  PCB* parent = CURPROC;
  PCB* child = get_pcb(cpid);
  if( child == NULL || child->parent != parent)
  {
    cpid = NOPROC;
    goto finish;
  }

  /* Ok, child is a legal child of mine. Wait for it to exit. */
  while(child->pstate == ALIVE)
    kernel_wait(& parent->child_exit, SCHED_USER);
  
  cleanup_zombie(child, status);
  
finish:
  return cpid;
}


static Pid_t wait_for_any_child(int* status)
{
  Pid_t cpid;

  PCB* parent = CURPROC;

  /* Make sure I have children! */
  int no_children, has_exited;
  while(1) {
    no_children = is_rlist_empty(& parent->children_list);
    if( no_children ) break;

    has_exited = ! is_rlist_empty(& parent->exited_list);
    if( has_exited ) break;

    kernel_wait(& parent->child_exit, SCHED_USER);    
  }

  if(no_children)
    return NOPROC;

  PCB* child = parent->exited_list.next->pcb;
  assert(child->pstate == ZOMBIE);
  cpid = get_pid(child);
  cleanup_zombie(child, status);

  return cpid;
}


Pid_t sys_WaitChild(Pid_t cpid, int* status)
{
  /* Wait for specific child. */
  if(cpid != NOPROC) {
    return wait_for_specific_child(cpid, status);
  }
  /* Wait for any child */
  else {
    return wait_for_any_child(status);
  }

}


void sys_Exit(int exitval)
{
  PCB* curproc = CURPROC;  

  /* First, store the exit status in the current process*/
  curproc->exitval = exitval;

  /* 
    Here, we must check that we are not the init task. 
    If we are, we must wait until all child processes exit. 
   */
  if(get_pid(curproc)==1) {
    while(sys_WaitChild(NOPROC,NULL)!=NOPROC);
  }
    sys_ThreadExit(exitval); 
}

//dummy function that always returns -1
int dummy() {
  return -1; 
}

//acquires space for a procinfo_cb struct
procinfo_cb* acquire_procinfo_cb() {
  return (procinfo_cb*)xmalloc(sizeof(procinfo_cb));
}

//acquires space for a procinfo struct
procinfo* acquire_procinfo() {
  return (procinfo*)xmalloc(sizeof(procinfo));
}

//read function for procinfo_cb
int procinfo_read(void* procinfo_, char* buf, unsigned int size) {
  procinfo_cb* procinfo = (procinfo_cb*) procinfo_;

  if (procinfo->cursor == NULL)
    return 0; //reached end of PT array.

  //reference for convenience
  PCB* pcb_cursor = procinfo->cursor;

  //making all the necessary initializations for the procinfo struct
  procinfo->info->pid = get_pid(pcb_cursor);
  procinfo->info->ppid = get_pid(pcb_cursor->parent); 
  procinfo->info->alive = (pcb_cursor->pstate == ALIVE); 
  procinfo->info->thread_count = pcb_cursor->thread_count;
  procinfo->info->main_task = pcb_cursor->main_task;
  procinfo->info->argl = pcb_cursor->argl;

  //copy size should never be greater than PROCINFO_MAX_ARGS_SIZE
  unsigned int copy_size = (pcb_cursor->argl < PROCINFO_MAX_ARGS_SIZE) ?
                             pcb_cursor->argl : PROCINFO_MAX_ARGS_SIZE;

  memcpy(procinfo->info->args, (char *)pcb_cursor->args, copy_size);
  
  //we copy the information of the process into the buffer
  memcpy(buf, (char *)procinfo->info, size);

  //we hold the size of the process info
  //because we might change some values
  int infoSize = sizeof(*procinfo);

  //the rest of the code ensures that the with the next call
  //of procinfo_read, the pointer to the pcb (cursor) will
  //always point to an ALIVE/ZOMBIE proc OR NULL(EOF)
  unsigned int index = procinfo->info->pid + 1;
  while(index < MAX_PROC) {
    if (PT[index].pstate != FREE) {
       procinfo->cursor = &PT[index];
       break;
    }
    else {
      index++;
    }
  }

  //EOF (for the next invocation of read)
  if (index == MAX_PROC)
    procinfo->cursor = NULL;

  //in any case, we return the correct size that we stored
  return infoSize; 
}

//close function for procinfo_cb
int procinfo_close(void* procinfo_){
  //error if NULL
  if(procinfo_ == NULL)
    return -1;

  //we must not forget to 
  //free the struct info 
  //within the struct procinfo
  procinfo_cb* procinfo = (procinfo_cb *) procinfo_;
  free(procinfo->info);
  free(procinfo);

  //successfully closed
  return 0;
}

//file operations for procinfo
static file_ops procinfo_file_ops = {
  .Write = dummy,
  .Read = procinfo_read, 
  .Open = NULL,
  .Close = procinfo_close
};

//invoked by SystemInfo
Fid_t sys_OpenInfo()
{
  Fid_t fd;        
  FCB* fcb;

  if(!FCB_reserve(1,&fd,&fcb))
    return NOFILE; 

  //necessary memory allocations
  procinfo_cb* procinfo = acquire_procinfo_cb();
  procinfo->info = acquire_procinfo();

  //first process
  procinfo->cursor = &PT[1];
  procinfo->info->pid = 1;

  //making the necessary connections for the fcb
  fcb->streamobj = procinfo;
  fcb->streamfunc = &procinfo_file_ops;

  //returning the file id
  return fd;
}