#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#ifdef VM
#include "vm/vm.h"
#endif
#include "threads/synch.h"	// lock 자료구조 사용

#define F (1 << 14)
// x, y는 fixed-point, n은 integer


/* States in a thread's life cycle. */
enum thread_status {
	THREAD_RUNNING,     /* Running thread. */
	THREAD_READY,       /* Not running but ready to run. */
	THREAD_BLOCKED,     /* Waiting for an event to trigger. */
	THREAD_DYING        /* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

#define INITIAL_LOAD_AVG 0				// define INTIAL_LOAD_AVG
#define INITIAL_RECENT_CPU 0			// define INTIAL_RECENT_CPU
#define INITIAL_NICE 0					// define INITIAL_NICE

/* A kernel thread or user process.
 *
 * Each thread structure is stored in its own 4 kB page.  The
 * thread structure itself sits at the very bottom of the page
 * (at offset 0).  The rest of the page is reserved for the
 * thread's kernel stack, which grows downward from the top of
 * the page (at offset 4 kB).  Here's an illustration:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * The upshot of this is twofold:
 *
 *    1. First, `struct thread' must not be allowed to grow too
 *       big.  If it does, then there will not be enough room for
 *       the kernel stack.  Our base `struct thread' is only a
 *       few bytes in size.  It probably should stay well under 1
 *       kB.
 *
 *    2. Second, kernel stacks must not be allowed to grow too
 *       large.  If a stack overflows, it will corrupt the thread
 *       state.  Thus, kernel functions should not allocate large
 *       structures or arrays as non-static local variables.  Use
 *       dynamic allocation with malloc() or palloc_get_page()
 *       instead.
 *
 * The first symptom of either of these problems will probably be
 * an assertion failure in thread_current(), which checks that
 * the `magic' member of the running thread's `struct thread' is
 * set to THREAD_MAGIC.  Stack overflow will normally change this
 * value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
 * the run queue (thread.c), or it can be an element in a
 * semaphore wait list (synch.c).  It can be used these two ways
 * only because they are mutually exclusive: only a thread in the
 * ready state is on the run queue, whereas only a thread in the
 * blocked state is on a semaphore wait list. */
struct thread {
	/* Owned by thread.c. */
	tid_t tid;                          /* Thread identifier. */
	enum thread_status status;          /* Thread state. */
	char name[16];                      /* Name (for debugging purposes). */
	int priority;                       /* Priority. */

	// timer.c
	int64_t local_ticks;				// local ticks 추가

	// donate
	int original_priority;				// original_priority
	struct lock *wait_on_lock;			// 기다리고 있는 lock
	struct list donors;					// waiters와 같다
	struct list_elem d_elem;			// elem과 같다

	// mlfqs
	struct list_elem m_elem;			// mlfqs_list의 elem
	struct list_elem all_elem;			// all_list의 elem
	int nice;
	int recent_cpu;

	/* Shared between thread.c and synch.c. */
	struct list_elem elem;              /* List element. */
	struct intr_frame *intr_frame_ptr;
#ifdef USERPROG
	/* Owned by userprog/process.c. */
	uint64_t *pml4;                     /* Page map level 4 */

	// syscall
	int exit_code;

	// fork
	struct thread *parent_thread;

	// wait
	struct list_elem c_elem;
	struct list child_list;
	struct list_elem k_elem;
	struct list killed_list;
	struct semaphore process_sema;
	struct semaphore fork_sema;

	// file descriptor table
	struct file **fd_table;	
#endif
#ifdef VM
	/* Table for whole virtual memory owned by thread. */
	struct supplemental_page_table spt;
#endif

	/* Owned by thread.c. */
	struct intr_frame tf;               /* Information for switching */
	unsigned magic;                     /* Detects stack overflow. */
};

struct multiple_ready_queue {
	struct list ready_queue;
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void do_iret (struct intr_frame *tf);

void thread_sleep (int64_t ticks);
void thread_wakeup (int64_t ticks);

bool cmp_priority(const struct list_elem *curr_elem, const struct list_elem *e, void *aux);	// compare priority
void thread_preempt(void);

int int_to_fp (int n);
int fp_to_int_zero (int x);
int fp_to_int_near (int x);
int fp_add_both (int x, int y);
int fp_sub_both (int x, int y);
int fp_add_int (int x, int n);
int fp_sub_int (int x, int n);
int fp_mul_both (int x, int y);
int fp_mul_int (int x, int n);
int fp_div_both (int x, int y);
int fp_div_int (int x, int n);

void update_priority(void);
void update_load_avg(void);
void plus_recent_cpu(void);
void update_recent_cpu(void);

#endif /* threads/thread.h */
