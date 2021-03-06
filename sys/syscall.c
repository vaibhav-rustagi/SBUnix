#include "sys/defs.h"
#include "sys/syscall.h"
#include "sys/kprintf.h"
#include "sys/process_manager.h"
#include "sys/pcb.h"
#include "sys/vfs.h"
#include "sys/elf64.h"
#include "sys/utility.h"
#include "sys/userPageTable.h"
#include "sys/freelist.h"

#define PAGE_SIZE 4096

static volatile int sleep_count = 0;

extern void sysCallHandler();
extern void loadNextProcess();    

extern void getCharacters(uint64_t data, uint64_t len);

uint64_t* function_ptr = NULL;
extern struct PCB* ready_proc_list;
extern struct PCB* sleep_proc_list;
extern struct PCB* current_proc;
extern struct PCB* proc_table[100];
void* systemCallHandlerTable[128];
// = {systemRead, systemWrite, systemExit, systemYield, systemFork};

void initSyscalls() {
	systemCallHandlerTable[__NR_read] = sys_read;
	systemCallHandlerTable[__NR_write] = sys_write;
	systemCallHandlerTable[__NR_open] = sys_open;
	systemCallHandlerTable[__NR_close] = sys_close;
	systemCallHandlerTable[__NR_yield] = systemYield;
	systemCallHandlerTable[__NR_sleep] = systemSleep;
	systemCallHandlerTable[__NR_fork] = systemFork;
	systemCallHandlerTable[__NR_execve] = systemExecvpe;
	systemCallHandlerTable[__NR_exit] = systemExit;
	systemCallHandlerTable[__NR_wait4] = systemWaitPid;
	systemCallHandlerTable[__NR_getdents] = sys_getdents;
	systemCallHandlerTable[__NR_getcwd] = sys_getcwd;
	systemCallHandlerTable[__NR_chdir] = sys_chdir;
	systemCallHandlerTable[__NR_mmap] = systemMMap;
	systemCallHandlerTable[__NR_munmap] = systemMunmap;
	systemCallHandlerTable[__NR_kill] = systemKill;	
}
 
void systemCallHandler()
{
    uint64_t sysNum;
	__asm__ volatile
	(
		"pushq %rdx;"
	);
	__asm__ volatile
	(
		"movq %%rax, %0;"
		: "=r"(sysNum)
		:
		: "cc", "memory"
	);

	function_ptr = systemCallHandlerTable[sysNum];
	__asm__ volatile
	(
		"popq %%rdx;"
		"callq %0;"
		:
		: "r" (function_ptr)
		: "cc", "rcx", "memory"	
	);

    __asm__ volatile
    (
        "iretq;"
        :
        :
        :"cc", "memory"
    );
}

void systemMunmap(uint64_t ptr)
{
	if(ptr < H_BASE || ptr > (H_BASE + H_END))	
	{
		kprintf("No malloc memory associated with address: %x. Please check.\n", ptr);
		return;
	}
	struct PCBMemList* temp1 = current_proc->usedHeapList;
	struct PCBMemList* temp2 = NULL;
	if(temp1 == NULL)
	{
		kprintf("No memory associated with address: %x. Please check.\n", ptr);
		return;
	}
	if(ptr == temp1->baseAddress)
	{
		temp2 = temp1;
		current_proc->usedHeapList = temp1->next;
	}
	else
	{
		while(temp1 && temp1->next != NULL)
		{
			temp2 = temp1->next;
			if(temp2->baseAddress == ptr)
			{
				temp1->next = temp2->next;
				break;
			}
			temp1 = temp1->next;
		}
		if(temp1->next == NULL)
			temp2 = NULL;
	}
	if(temp2 == NULL)
	{
		kprintf("No memory associated with address: %x. Please check.\n", ptr);
		return;
	}
	//for(uint64_t i = temp2->baseAddress; i < (temp2->baseAddress + temp2->size); i += 0x1000)
	//	i = 0;
	for(uint64_t i = temp2->baseAddress; i < (temp2->baseAddress + temp2->size); i += 0x1000)
	{
		uint64_t* ptEntry = getPTTableEntry(current_proc->pml4, i);
		uint64_t ptentry = (uint64_t)(*ptEntry);
		ptentry = ptentry & GET_40_BITS;
		struct PAGE* p = getPageStruct(ptentry);
		freePage(p);
		*ptEntry = 0;
	}
	temp2->next = current_proc->freeHeapList;
	current_proc->freeHeapList = temp2;
	flushTLB();
}

uint64_t systemMMap(uint64_t size)
{
	struct PCBMemList* freeList = current_proc->freeHeapList;
	if(size % (PAGE_SIZE))
	{
		int8_t q = size/(PAGE_SIZE);
		size = (PAGE_SIZE)*(q+1);
	}
	while(freeList != NULL)
	{
		if(freeList->size >= size)
		{
			break;
		}
		else
		{
			freeList = freeList->next;
		}
	}
	if(freeList == NULL)
	{
		ERROR("Process run out of dynamic memory. Terminating the process\n");
	}
	if(current_proc->freeHeapList == freeList)
	{
		current_proc->freeHeapList = current_proc->freeHeapList->next;
		freeList->next = NULL;
	}
	else
	{
		struct PCBMemList* temp = current_proc->freeHeapList;
		while(temp->next != freeList)
			temp = temp->next;
		if(temp)
		{
			temp->next = freeList->next;
			freeList->next = NULL;
		}
	}
	if(freeList->size > size)
	{
		struct PCBMemList* t = (struct PCBMemList*) kmalloc(sizeof(struct PCBMemList));
		t->size = freeList->size - size;
		t->baseAddress = freeList->baseAddress + size;
		freeList->size = size;
		t->next = current_proc->freeHeapList;
		current_proc->freeHeapList = t;
	}
	for(uint64_t i = freeList->baseAddress; i < freeList->baseAddress + freeList->size; i += 0x1000)
	{
		walkUserPageTables(current_proc->pml4, i, 0);
	}
	if(current_proc->usedHeapList == NULL)
	{
		current_proc->usedHeapList = freeList;
		freeList->next = NULL;
	}
	else
	{
		freeList->next = current_proc->usedHeapList;
		current_proc->usedHeapList = freeList;
	}
	//uint64_t check = freeList->baseAddress;
	//kprintf("Value of check is: %x\n", check);
	return (freeList->baseAddress);
} 

void incrementSleepCount() {
	sleep_count++;
	//kprintf("%d\n", sleep_count);
}

uint64_t systemSleep(uint64_t seconds) {
	current_proc->state = SLEEPING;
	systemYield();
	__asm__("sti");
	while(sleep_count < seconds) {
	//	kprintf("in sleep %d\n", sleep_count);
	}
	sleep_count = 0;
	current_proc->state = READY;
	return 0;
}

void systemWrite(uint64_t fd, uint64_t data, uint64_t len)
{
	char* d = (char*)data;
	kprintf("Data is: %s\n", d);
	/*for(uint64_t i = 0; i < len; i++)
	{
		kprintf("%c", d++);
	}*/
}

void systemRead(uint64_t fileDescriptor, uint64_t data, uint64_t len)
{
    if(fileDescriptor==0)
    {
        if(len==0)
            return;
        getCharacters(data, len);
    }
}

pid_t systemFork()
{
    struct PCB *parent = current_proc; 
    struct PCB *child = copyProcess(parent); 

    initializeProc(child, parent->kstack[KSTACK_SIZE-7], parent->kstack[KSTACK_SIZE-4]);
    child->kstack[KSTACK_SIZE-2] = 0UL;

    printReadyList();
    return child->pid;
}

void systemExit(uint64_t status)
{
    //kprintf("Process exit with status: %d\n", status);
    for(int i=0;i<100;i++) {
        if(current_proc->child_list[i] == 1) {
            proc_table[i]->ppid = 1;
        }
    }

    flushPageTable(current_proc->pml4);
    current_proc->state = EXIT;
    loadNextProcess();
}

void systemKill(uint64_t killpid)
{
	int i;
	int flag = 0;
	for(i = 0; i < 100; i++)
	{
		if(proc_table[i] != NULL && proc_table[i]->pid == killpid)
		{
			struct PCB* temp = proc_table[i];
			if(current_proc->pid == temp->pid)
			{
				flag = 1;
			}
			for(int j = 0; j < 100; j++)
			{
				if(temp->child_list[j] == 1)
				{
					proc_table[j]->ppid = 1;
				}
			}
			struct PCB* currentProcess = current_proc;
			current_proc = temp;
		        loadCR3(temp->pml4);
			flushPageTable(current_proc->pml4);
			proc_table[i] = NULL; 
			current_proc->state = EXIT;
			if(flag)
			{
			   loadNextProcess();
			}
			else
			{
			   struct PCB* iterate = ready_proc_list;
			   if(iterate != NULL && iterate->pid == killpid)
			   {
			       ready_proc_list = iterate->next;
			       iterate->next = NULL;
			   }
			   else
			   {
			       while(iterate && iterate->next != NULL && iterate->next->pid != killpid)
				   iterate = iterate->next;
			       if(iterate && iterate->next != NULL)
			       {
				   struct PCB* t = iterate->next;
                                   iterate->next = t->next;
				   t->next = NULL;
                               }
			       else
			       {
                                   iterate = sleep_proc_list;
				   if(iterate && iterate->pid == killpid)
				   {
					sleep_proc_list = iterate->next;
				        iterate->next = NULL;
				   }
				   else
				   {
					while(iterate && iterate->next != NULL && iterate->next->pid != killpid)
					{
						iterate = iterate->next;
					}
					if(iterate && iterate->next != NULL)
					{
						struct PCB* t1 = iterate->next;
						iterate->next = t1->next;
						t1->next = NULL;
					}
				   }
                               }
				
			   }
			   current_proc =  currentProcess;
			   loadCR3(current_proc->pml4);
			}
			break;
		}
	}
	if(i==100)
	{
		kprintf("Given pid by user not found. Please check.\n");
	}
}

void systemYield()
{
    // kprintf("Inside Yield\n");
    current_proc->state = READY;
    loadNextProcess();    
}


//TODO copy siblings and child
uint64_t systemExecvpe(char *file_path, char *argv[], char *envp[])
{

    struct PCB *exec = read_file(file_path, argv , envp);
    // kprintf("%s\n", "Inside Execvpe");

    if (exec != NULL) {
        // struct PCB *temp = current_proc;

        // Exec process has same pid, ppid and parent
        set_pid(exec->pid);
        exec->pid  = current_proc->pid;
        exec->ppid = current_proc->ppid;
        exec->parent = current_proc->parent;
	exec->cwd = current_proc->cwd;
        //memcpy((void*)new_task->file_descp, (void*)cur_task->file_descp, MAXFD*8);

        // // Replace current child with new exec process
        // replace_child_task(cur_task, new_task);

        // Exit from the current process
        // empty_task_struct(cur_task);
        // schedule_next_process()
        // addToFrontReady(exec);
        systemExit(0);
        //switch_to_ring3_from_kernel();
 
    }
    // execvpe failed; so return -1
    return 1;
}

//TODO check if valid pid
uint64_t systemWaitPid(uint64_t pid, uint64_t status, uint64_t options)
{
    // int *status_p = (int*) fstatus;
    // if (current_proc->child_cnt == 0) {
    //     //if (status_p) *status_p = -1;
    //     return -1;
    // }

    // if (pid > 0) {
    //     current_proc->wait_on_child_pid = pid;
    // } else {
    //     current_proc->wait_on_child_pid = 0;
    // }

    // current_proc->state = WAIT;

    // // if (status_p) *status_p = 0;
    // return (uint64_t)current_proc->wait_on_child_pid;
    /*struct PCB* proc = ready_proc_list;
    while(proc) {
        if(proc->pid == pid) {
            // systemYield();
            return pid;
        }
        proc = proc->next;
    }
    
    return 0;*/
    if(pid > 100) {
        return 0;
    }
    /*if(proc_table[pid]->state == EXIT) {
        proc_table[pid] = NULL;
        current_proc->child_list[pid] = 0;
        return 0;
    } else {*/
        struct PCB* proc = ready_proc_list;
        while(proc) {
            if(proc->pid == pid) {
                // systemYield();
                return pid;
            }
            proc = proc->next;
        }
   // }

    return 0;    
}
