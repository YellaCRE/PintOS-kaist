#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

#include "filesys/filesys.h"	// 이걸 해야 pintos 64bit 함수들을 사용할 수 있다
#include "filesys/file.h"
#include "userprog/process.h"
#include "threads/init.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "devices/input.h"
#include <string.h>
#ifdef VM
#include "vm/vm.h"
#endif
#define STDIN_FILENO 0
#define STDOUT_FILENO 1

void syscall_entry (void);
void syscall_handler (struct intr_frame * UNUSED);

void check_file_valid(void *ptr);
void check_fd_valid(int fd);

void _halt (void);

pid_t _fork (const char *thread_name, struct intr_frame *f UNUSED);
int _exec (const char *cmd_line);
int _wait (pid_t pid);

bool _create (const char *file, unsigned initial_size);
bool _remove (const char *file);
int _open (const char *file);
int _filesize (int fd);
int _read (int fd, void *buffer, unsigned size);
int _write (int fd, const void *buffer, unsigned size);
void _seek (int fd, unsigned position);
unsigned _tell (int fd);
void _close (int fd);
void *_mmap(void *addr, size_t length, int writable, int fd, off_t offset);
void _munmap(void *addr);

struct lock global_sys_lock;

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
	
	lock_init(&global_sys_lock);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.
	#ifdef VM
	thread_current()->rsp_stack = (void *)f->rsp;
	#endif
	
	switch (f->R.rax){
		case SYS_HALT:
			_halt();
			break;
		
		case SYS_EXIT:
			_exit((int)f->R.rdi);
			break;

		case SYS_FORK:
			f->R.rax = _fork((const char *)f->R.rdi, f);
			break;

		case SYS_EXEC:
			f->R.rax = _exec((const char *)f->R.rdi);
			break;

		case SYS_WAIT:
			f->R.rax = _wait((pid_t)f->R.rdi);
			break;

		case SYS_CREATE:
			f->R.rax = _create((const char *)f->R.rdi, (unsigned int)f->R.rsi);
			break;

		case SYS_REMOVE:
			f->R.rax = _remove((const char *)f->R.rdi);
			break;

		case SYS_OPEN:
			f->R.rax = _open((const char *)f->R.rdi);
			break;

		case SYS_FILESIZE:
			f->R.rax = _filesize((int)f->R.rdi);
			break;
		
		case SYS_READ:
			f->R.rax = _read((int)f->R.rdi, (void *)f->R.rsi, (unsigned int)f->R.rdx);
			break;

		case SYS_WRITE:
			f->R.rax = _write((int)f->R.rdi, (const void *)f->R.rsi, (unsigned int)f->R.rdx);
			break;

		case SYS_SEEK:
			_seek((int)f->R.rdi, (unsigned int)f->R.rsi);
			break;

		case SYS_TELL:
			f->R.rax = _tell((int)f->R.rdi);
			break;

		case SYS_CLOSE:
			_close((int)f->R.rdi);
			break;
		
		case SYS_MMAP:
			f->R.rax = _mmap((void *)f->R.rdi, (size_t)f->R.rsi, (int)f->R.rdx, (int)f->R.r10, (off_t)f->R.r8);
			break;
		
		case SYS_MUNMAP:
			_munmap((void *)f->R.rdi);
			break;
	}
}

/* ========== kernel-level function ==========*/

void
check_file_valid(void *ptr){
	if (!ptr || is_kernel_vaddr(ptr) || !(pml4_get_page(thread_current()->pml4, ptr)))
		_exit(-1);
}

#ifdef VM
// project 3 이후에 만들어야 하는 코드
void
check_buffer_valid(void *ptr, unsigned size UNUSED, bool to_write){
	if (!ptr || is_kernel_vaddr(ptr))
		_exit(-1);
	
	struct page* page;
	page = spt_find_page(&thread_current()->spt, ptr);
	if(!page)
		_exit(-1);
	if(to_write == true && page->writable == false)
		_exit(-1);
}
#endif

void
check_fd_valid(int fd){
	if (fd < 3 || fd >= OPEN_MAX)
		_exit(-1);
}

void
_halt (void) {
	power_off();
}

void
_exit (int status) {
	struct thread *curr = thread_current ();
	struct exit_info *exit_code_info;

	// exit code 설정
	curr->exit_code = status;
	printf ("%s: exit(%d)\n", curr->name, curr->exit_code);

	// 종료 전 exit code 저장하기
	if (curr->parent_thread != NULL){
		exit_code_info = (struct exit_info *)malloc(sizeof(struct exit_info));
		exit_code_info->tid = curr->tid;
		exit_code_info->exit_code = status;
		list_push_back(&curr->parent_thread->exit_code_list, &exit_code_info->e_elem);
	}

	// 명시적으로 fd_table 닫아주기
	for (int i=3; i<OPEN_MAX; i++){
		_close(i);
	}

	// process_exit 호출
	thread_exit ();
}

pid_t
_fork (const char *thread_name, struct intr_frame *f UNUSED){
	return process_fork((const char *)thread_name, f);
}

int
_exec (const char *cmd_line) {
	check_file_valid((void *)cmd_line);
	char *fn_copy;

	fn_copy = palloc_get_page (PAL_ZERO);
	if (fn_copy == NULL){
		palloc_free_page(fn_copy);
		return -1;
	}
	
	strlcpy (fn_copy, cmd_line, PGSIZE);

	if ((process_exec(fn_copy)) == -1)
		_exit(-1);
	NOT_REACHED();
}

int
_wait (pid_t pid) {
	int child_exit_code;
	if ((child_exit_code = find_exit_code(pid)) != -1)
		return child_exit_code;
	return process_wait(pid);
}

bool
_create (const char *file, unsigned initial_size) {
	check_file_valid((void *)file);

	lock_acquire(&global_sys_lock);
	bool success = filesys_create(file, initial_size);
	lock_release(&global_sys_lock);

	return success;
}

bool
_remove (const char *file) {
	check_file_valid((void *)file);
	return filesys_remove(file);
}

int
_open (const char *file) {
	check_file_valid((void *)file);
	lock_acquire(&global_sys_lock);

	struct file *target;
	struct thread *curr = thread_current ();

	target = filesys_open(file);
	if (!target){
		lock_release(&global_sys_lock);
		return -1;
	}
	for (int i=3; i<OPEN_MAX; i++){
		if (curr->fd_table[i] == NULL){
			curr->fd_table[i] = target;
			lock_release(&global_sys_lock);
			return i;
		}
	}

	// 만약 fd_table에 빈 공간이 없다면 닫는다.
	file_close(target);
	lock_release(&global_sys_lock);
	return -1;
}

int
_filesize (int fd) {
	struct file *target;

	if (!(target = thread_current()->fd_table[fd]))
		return -1;
	return file_length(target);
}

int
_read (int fd, void *buffer, unsigned size) {
	#ifdef VM
		check_buffer_valid((void *) buffer, size, true);
	#else
		check_file_valid((void *) buffer);
	#endif
	lock_acquire(&global_sys_lock);

	struct thread *curr = thread_current ();
	struct file *target;
	unsigned read_len;
	
	if (fd == STDIN_FILENO){
		*(char *)buffer = input_getc();	// buffer에 그대로 넣는다
		read_len = size;
	}
	else{
		check_fd_valid(fd);
		if (!(target = curr->fd_table[fd])){
			lock_release(&global_sys_lock);
			return -1;
		}
		read_len = file_read(target, buffer, size);
	}
	lock_release(&global_sys_lock);
	return read_len;
}

int
_write (int fd, const void *buffer, unsigned size) {
	#ifdef VM
		check_buffer_valid((void *) buffer, size, false);
	#else
		check_file_valid((void *) buffer);
	#endif
	lock_acquire(&global_sys_lock);

	struct thread *curr = thread_current ();
	struct file *target;
	unsigned write_len;

	if (fd == STDOUT_FILENO){
		putbuf(buffer, size);
		write_len = size;
	}
	else{
		check_fd_valid(fd);
		if (!(target = curr->fd_table[fd])){
			lock_release(&global_sys_lock);
			return -1;
		}
		write_len = file_write(target, (void *)buffer, size);
	}
	lock_release(&global_sys_lock);
	return write_len;
}

void
_seek (int fd, unsigned position) {
	check_fd_valid(fd);
	file_seek(thread_current()->fd_table[fd], position);
}

unsigned
_tell (int fd) {
	check_fd_valid(fd);
	return file_tell(thread_current()->fd_table[fd]);
}

void
_close (int fd) {
	check_fd_valid(fd);
	
	struct thread *curr;
	struct file *target;

	curr = thread_current ();
	if (!(target = curr->fd_table[fd]))
		return;

	curr->fd_table[fd] = NULL;
	file_close(target);
}

void *
_mmap(void *addr, size_t length, int writable, int fd, off_t offset){
	#ifdef VM
	struct thread *curr = thread_current();
	struct file *file;

	// may fail if the file opened as fd has a length of zero bytes
	if ((long long)length <= 0)
		return NULL;

	// must fail if addr is not page-aligned
	if (pg_round_down(addr) != addr)
		return NULL;

	// Tries to mmap an invalid offset
	if (offset > (off_t)length)
		return NULL;

	if (offset % PGSIZE != 0)
		return NULL;

	// must fail if overlaps any existing set of mapped pages
	if (spt_find_page(&curr->spt, addr))
		return NULL;

	// if addr is 0, it must fail
	if (addr == NULL)
		return NULL;
	
	// try to mmap over kernel
	// addr이 커널 시작 주소인지 || 64비트 이상으로 사용하는지
	if ((long long)addr == KERN_BASE || is_kernel_vaddr(addr))
		return NULL;

	// the fd representing console input and output are not mappable.
	if (fd == 0 || fd == 1)
		return NULL;

	// file open as fd
	if (!(file = curr->fd_table[fd]))
		return NULL;

	return do_mmap(addr, length, writable, file, offset);
	#endif
}

void
_munmap(void *addr){
	#ifdef VM
	do_munmap(addr);
	#endif
}