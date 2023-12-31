#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#include "userprog/syscall.h"
#endif
// 구분자를 공백으로 설정
#define DELIM_CHARS	" "

extern struct lock global_sys_lock;

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);
struct thread * find_child(tid_t child_tid);

/* General process initializer for initd and other process. */
static void
process_init (void) {
	struct thread *current UNUSED = thread_current ();
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

	char *ptr, *next_ptr;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

	// 파일 이름 제대로 만들기
	ptr = strtok_r((char *)file_name, DELIM_CHARS, &next_ptr);

	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (ptr, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
		palloc_free_page (fn_copy);
	return tid;
}

/* A thread function that launches first user process. */
static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();

	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t
process_fork (const char *name, struct intr_frame *if_ UNUSED) {
	/* Clone current thread to new thread.*/
	tid_t tid;
	struct thread *curr = thread_current();
	struct thread *child_thread;

	curr->intr_frame_ptr = if_;
	tid = thread_create (name, PRI_DEFAULT, __do_fork, curr);
	
	if (tid == TID_ERROR)
		return tid;
	
	child_thread = find_child(tid);
	sema_down(&child_thread->fork_sema);
	
	// sema down 이후에 바뀐 exit code 검사
	if (child_thread->exit_code == -1)
		return TID_ERROR;
	
	return tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;
	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	if (is_kern_pte(pte))
		return true;
	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page (parent->pml4, va);
	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	newpage = palloc_get_page (PAL_USER);
	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	memcpy(newpage, parent_page, PGSIZE);
	writable = is_writable(pte);
	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: if fail to insert page, do error handling. */
		palloc_free_page(newpage);
	}
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork (void *aux) {
	struct intr_frame if_;
	struct thread *parent = aux;
	struct thread *current = thread_current ();
	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
	struct intr_frame *parent_if = parent->intr_frame_ptr;
	bool succ = true;
	/* 1. Read the cpu context to local stack. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));
	
	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);
#ifdef VM
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/

	for (int i=3; i<OPEN_MAX; i++){
		struct file *tmp = parent->fd_table[i];
		if (tmp != NULL){
			current->fd_table[i] = file_duplicate(tmp);
		}
	}
	process_init ();
	
	if_.R.rax = 0;
	sema_up(&current->fork_sema);	// 이제 대기하고 있던 부모를 깨워준다

	/* Finally, switch to the newly created process. */
	if (succ){
		do_iret (&if_);
	}
error:
	succ = false;
	sema_up(&current->fork_sema);
	_exit(-1);
	// thread_exit ();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* We first kill the current context */
	process_cleanup ();

#ifdef VM
	supplemental_page_table_init(&thread_current()->spt);
#endif

	/* And then load the binary */
	lock_acquire(&global_sys_lock);
	success = load (file_name, &_if);
	lock_release(&global_sys_lock);

	palloc_free_page (file_name);		// 여기서 fn_copy를 free 해준다

	/* If load failed, quit. */
	if (!success)
		return -1;

	// user memory access
	if (!is_user_vaddr(_if.rsp))
		return -1;

	/* Start switched process. */
	do_iret (&_if);
	NOT_REACHED ();
}


/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int
process_wait (tid_t child_tid UNUSED) {
	struct thread *parent_thread = thread_current();
	struct thread *child_thread;
	int exit_code;

	// invalid child_tid
	if (child_tid == TID_ERROR)
		return -1;

	// 부모의 자식이 있으면
	if (list_empty(&parent_thread->child_list))
		return -1;

	// 만약 내 자식이면
	if ((child_thread = find_child(child_tid)) == NULL)
		return -1;
	
	// // 이미 기다린 자식이 아니면
	// struct list_elem *e;
	// struct thread *e_thread;
	// for (e=list_begin(&parent_thread->killed_list); e!=list_end(&parent_thread->killed_list); e = list_next(e)){
	// 	e_thread = list_entry(e, struct thread, k_elem);
	// 	if (e_thread->tid == child_tid){
	// 		return -1;
	// 	}
	// }
	// // 기다린 목록에 추가
	// list_push_back(&parent_thread->killed_list, &child_thread->k_elem);

	// 기다린다
	sema_down(&child_thread->wait_sema);
	
	exit_code = find_exit_code(child_tid);
	return exit_code;
}

struct thread *
find_child(tid_t child_tid){
	struct list_elem *e;
	struct thread *e_thread;
	struct thread *parent_thread = thread_current();
	struct thread *child_thread;

	child_thread = NULL;
	for (e=list_begin(&parent_thread->child_list); e!=list_end(&parent_thread->child_list); e = list_next(e)){
		e_thread = list_entry(e, struct thread, c_elem);
		if (e_thread->tid == child_tid){
			child_thread = e_thread;
			break;
		}
	}
	return child_thread;
}

int
find_exit_code(tid_t child_tid){
	struct list_elem *e;
	struct exit_info *e_info;
	struct thread *parent_thread = thread_current();
	int exit_code;

	for (e=list_begin(&parent_thread->exit_code_list); e!=list_end(&parent_thread->exit_code_list); e = list_next(e)){
		e_info = list_entry(e, struct exit_info, e_elem);
		if (e_info->tid == child_tid){
			exit_code = e_info->exit_code;
			list_remove(e);								// 이미 죽은 자식 제거
			free(e_info);
			return exit_code;
		}
	}
	return -1;
}

#ifdef VM
void 
mmap_destroy(struct hash_elem *hash_elem, void *aux UNUSED){
	struct page *page = hash_entry(hash_elem, struct page, hash_elem);

	if (page && page_get_type(page) == VM_FILE){
		_munmap(page->va);
	}
}
#endif

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	// All mappings are implicitly unmapped when a process exits
	hash_apply(&curr->spt.supplemental_page_hash, mmap_destroy);
#endif

	/* TODO: Your code goes here.
	 * TODO: Implement process termination message (see
	 * TODO: project2/process_termination.html).
	 * TODO: We recommend you to implement process resource cleanup here. */
	
	palloc_free_page((void *)curr->fd_table);	// fd_table 메모리 정리

	if (curr->file_in_use != NULL){
		file_close(curr->file_in_use);			// 사용 중인 파일도 닫기
		curr->file_in_use = NULL;
	}

	if (curr->parent_thread != NULL){
		if (!list_empty(&curr->parent_thread->child_list)){
			list_remove(&curr->c_elem);
			curr->parent_thread = NULL;
		}

		// if (!list_empty(&curr->parent_thread->killed_list)){
		// 	list_remove(&curr->k_elem);
		// }
	}
	process_cleanup ();							// pml4도 메모리 정리
	sema_up(&curr->wait_sema);					// 자식 종료한다고 부모에게 알려주기
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);
void load_stack(struct intr_frame *if_, char **arg_value, int arg_count);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;
	
	// parse file_name
	char *fn_copy = (char*)file_name;
	char *argv[64];
	char *ptr, *next_ptr;
	int argc = 0;
 
	ptr = strtok_r(fn_copy, DELIM_CHARS, &next_ptr);
	argv[argc] = ptr;
	while (ptr) {
		ptr = strtok_r(NULL, DELIM_CHARS, &next_ptr);
		argc += 1;
		argv[argc] = ptr;
	}

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	/* Open executable file. */
	file = filesys_open (argv[0]);
	if (file == NULL) {
		printf ("load: %s: open failed\n", argv[0]);
		goto done;
	}

	// 실행 중인 파일에 대해서 deny_write 설정
	file_deny_write(file);
	t->file_in_use = file;

	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", argv[0]);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* Set up stack. */
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */

	load_stack(if_, argv, argc);
	success = true;

done:
	/* We arrive here whether the load is successful or not. */

	// file_close (file);
	return success;
}


/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

void
load_stack(struct intr_frame *if_, char **arg_value, int arg_count){
	char *arg_value_addr[64];

	// printf("USER_STACK: %p\n", if_->rsp);

	// argv[i] 쌓기
	for (int i=arg_count-1; i>=0; i--){
		if_->rsp -= strlen(arg_value[i])+1;
		memcpy((void *)if_->rsp, arg_value[i], strlen(arg_value[i])+1);
		// printf("argv[%d][]: %p, %s\n", i, if_->rsp, arg_value[i]);
		arg_value_addr[i] = (char *)if_->rsp;
	}
	
	// word_align
	uint8_t word_align[8 - (USER_STACK - if_->rsp) % 8];	// 성능을 위해 8의 배수로 맞춰준다
	if_->rsp -= sizeof(word_align);
	memcpy((void *)if_->rsp, word_align, sizeof(word_align));
	// printf("word_align: %p\n", if_->rsp);

	// null pointer
	if_->rsp -= sizeof(char **);
	memset((void *)if_->rsp, 0, sizeof(char **));
	// printf("argv[%d]: %p, null pointer\n", arg_count, if_->rsp);

	// argv userstack에 쌓기
	// memcpy(if_->rsp, arg_value, sizeof(arg_value));
	for (int i=arg_count-1; i>=0; i--){
		if_->rsp -= sizeof(char **);
		memcpy((void *)if_->rsp, &arg_value_addr[i], sizeof(char *));
		// printf("argv[%d]: %p\n",i ,if_->rsp);
	}

	// RDI: argc | RSI: &argv[0]
	(&if_->R)->rdi = arg_count;
	(&if_->R)->rsi = if_->rsp;

	// fake "return address"
	if_->rsp -= sizeof(void *);
	memset((void *)if_->rsp, 0, sizeof(void *));
	// printf("fake return address: %p\n", if_->rsp);
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}

#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
	struct load_info *aux_load_info = (struct load_info *) aux;

	struct file *file 		= aux_load_info->file;
	off_t ofs				= aux_load_info->ofs;
	size_t page_read_bytes 	= aux_load_info->page_read_bytes;
	size_t page_zero_bytes 	= PGSIZE - page_read_bytes;

	file_seek (file, ofs);
	/* Get a page of memory. */
	void *kva = page->frame->kva;

	/* Load the segment. */
	if (file_read (file, kva, page_read_bytes) != (int) page_read_bytes) {
		palloc_free_page(kva);
		free(aux_load_info);
		return false;
	}
	memset (kva + page_read_bytes, 0, page_zero_bytes);
	return true;
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		struct load_info *aux;
		aux = (struct load_info *)malloc(sizeof(struct load_info));

		aux->file = file;
		aux->ofs = ofs;
		aux->page_read_bytes = page_read_bytes;

		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux)){
			free(aux);
			return false;
		}


		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		ofs += page_read_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	// Map the stack on stack_bottom
	if (vm_alloc_page(VM_ANON | VM_MARKER_0, stack_bottom, true)){
		// claim the page
		success = vm_claim_page(stack_bottom);

		// If success, set the rsp and mark the page
		if (success) {
			if_->rsp = USER_STACK;
            thread_current()->stack_bottom = stack_bottom;
		}
	}
	return success;
}
#endif /* VM */
