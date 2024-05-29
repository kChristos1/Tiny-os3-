
#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_cc.h"
#include "kernel_streams.h"

/** 
  @brief Create a new thread in the current process. Also returns its Tid_t 
  */
Tid_t sys_CreateThread(Task task, int argl, void* args)
{ 
  if (task == NULL){
      return NOTHREAD;
  }

  PCB* curproc = CURPROC; 
 
  PTCB* newptcb =spawn_process_thread(curproc, start_process_thread);  

  /*passing arguments*/
  newptcb->task = task; 
  newptcb->args = args; 
  newptcb->argl = argl; 

  assert(newptcb->task == task);

  wakeup(newptcb->tcb);

  return (Tid_t) newptcb;
}

/**
  @brief Return the Tid of the current thread.
 */
Tid_t sys_ThreadSelf()
{
	return (Tid_t) cur_thread()->ptcb;
}

/**
  @brief Join the given thread.
  */
int sys_ThreadJoin(Tid_t tid, int* exitval)
{
  PCB* curproc = CURPROC; 
  PTCB* ptcb = (PTCB*)tid; 

  if(ptcb==NULL){
    return -1;
  }

  //check if the ptcb of the wanted thread exists in the ptcb list of the CURRENT process.
  //2 threads that belong in a different processes can't be joined! 
  rlnode* result_node = rlist_find(&curproc->ptcb_list, ptcb , NULL); 

  if(result_node == NULL){
    return -1;
  }

  //check if the ptcb (of the tid thread) is detached.
  if(ptcb->detached==1){
    return -1;
  }
 
  //check if the thread is trying to join itself (curthread).
  if(tid == sys_ThreadSelf()){
    return -1;
  }

  //in any other case, threads can be joined, so we increase ref_count by 1
  ptcb->ref_count ++;

 //wait while it's not exited, or detached 
  while(!ptcb->exited && !ptcb->detached){
    kernel_wait(& ptcb->exit_cv, SCHED_USER);
 }
  //else we are not waiting now, so the refcount is decreased by 1
  ptcb->ref_count --;

  //there is a chance we stopped waiting because the thread became detached
  //in this case we must return -1 (error)
  if (ptcb->detached){
    return -1;
  }

  //set exitval
  if (exitval){
      *exitval = ptcb->exitval;
  }

  //if we dont need the ptcb (ref_count==0), clear it from the memory.
  if(ptcb->ref_count == 0){
    rlist_remove(& ptcb->ptcb_list_node);
     free(ptcb);
  }
 
 return 0;

}

/**
  @brief Detach the given thread.

  returns -1 on failure (thread @c tid cannot be detached)
  returs 0 on success (thread @c tid can be detached)
  */
int sys_ThreadDetach(Tid_t tid)
{
  //"ptcb" is the corresponding ptcb
  PCB* curproc = CURPROC;
  PTCB* ptcb = (PTCB*)tid; 

  //check if the ptcb of the wanted thread exists in the ptcb list of the CURRENT process.
  rlnode* result_node = rlist_find(&curproc->ptcb_list, ptcb , NULL); 

  //failure because the wanted thread does not exist in a ptcb of the CURRENT process
  if(result_node == NULL){
    return -1; 
  }
  //failure because the wanted thread exists in list but it's exited.
  if(ptcb->exited ==1){
    return -1; 
  }

  //else , thread can be detached (it exists AND its not exited)
  ptcb->detached=1; 

  kernel_broadcast(&ptcb->exit_cv);

  return 0; 
}


/*
  Terminate the current thread
*/
void sys_ThreadExit(int exitval)
{
  PTCB* curptcb = (PTCB*) sys_ThreadSelf(); 
  curptcb->exitval = exitval;  //stores exit val in the ptcb 
  curptcb->exited = 1;         //sets exited flag 
 
  PCB* curproc = CURPROC; 
  curproc->thread_count --;

  //All threads are informed of the exit
  //Wakes up the threads that have joined the exiting (current) thread .
  kernel_broadcast(&curptcb->exit_cv);

  //if its the last thread in the current process 
  if(curproc->thread_count == 0){
    //if it's not the init process, we have to reparent the children    
    if (get_pid(curproc) != 1) {

      //reparenting children of the current process as needed  
      PCB* initpcb = get_pcb(1);
      while(!is_rlist_empty(& curproc->children_list)) {
        //remove the head of the list and return the head of the list in "child" variable
        rlnode* child = rlist_pop_front(& curproc->children_list);
        
        child->pcb->parent = initpcb;
        
        rlist_push_front(& initpcb->children_list, child);
      }
      //add exited children to the initial process's exited list 
      //and signal the initial process , if the curproc's exited list is empty, 
      //there is nothing to append to the initial proc's exited list.
      if(!is_rlist_empty(& curproc->exited_list)) {
        rlist_append(& initpcb->exited_list, &curproc->exited_list);
        kernel_broadcast(& initpcb->child_exit);
      }
      //inserts exiting node in the parent's exited list
      rlist_push_front(& curproc->parent->exited_list, &curproc->exited_node);
      kernel_broadcast(& curproc->parent->child_exit);
    }

    assert(is_rlist_empty(& curproc->children_list));
    assert(is_rlist_empty(& curproc->exited_list));

    CleanUp(curproc);
  }
  //Bye-bye cruel world 
  kernel_sleep(EXITED, SCHED_USER); 
}

 /*
     Do all the other cleanup we want here, close files etc    */
void CleanUp(PCB* curproc)
{
/* Release the args data */
  if(curproc->args) {
    free(curproc->args);
    curproc->args = NULL;
  }

  /* Clean up FIDT */
  for(int i=0;i<MAX_FILEID;i++) {
    if(curproc->FIDT[i] != NULL) { 
      FCB_decref(curproc->FIDT[i]);     
      curproc->FIDT[i] = NULL;
    }
  }
  
  //free the ptcbs from the memory
  while(!is_rlist_empty(&CURPROC->ptcb_list)) {
    PTCB* ptcb = rlist_pop_front(&curproc->ptcb_list)->ptcb;
    free(ptcb);
  }
   //disconnect main_thread 
  curproc->main_thread = NULL;

  //mark the process as exited.
  curproc->pstate = ZOMBIE;
}

