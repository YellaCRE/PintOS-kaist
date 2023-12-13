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
#include "devices/input.h"
#include <string.h>

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define OPEN_MAX 64

void syscall_entry (void);
void syscall_handler (struct intr_frame * UNUSED);

void check_file_valid(void *ptr);
void check_fd_valid(int fd);

void _halt (void);
void _exit (int status);

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
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.

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
	}
}

/* ========== kernel-level function ==========*/

void
check_file_valid(void *ptr){
	if (!ptr || is_kernel_vaddr(ptr) || !(pml4_get_page(thread_current()->pml4, ptr)))
		_exit(-1);
}

void
check_fd_valid(int fd){
	if (fd < 2 || fd >= OPEN_MAX)
		_exit(-1);
}

void
_halt (void) {
	power_off();
}

void
_exit (int status) {
	struct thread *curr = thread_current ();
	curr->exit_code = status;
	printf ("%s: exit(%d)\n", curr->name, curr->exit_code);

	for (int i=3; i<OPEN_MAX; i++){
		_close(i);
	}

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

	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return -1;
	
	strlcpy (fn_copy, cmd_line, PGSIZE);

	if ((process_exec(fn_copy)) == -1)
		_exit(-1);
	NOT_REACHED();
}

int
_wait (pid_t pid) {
	return process_wait(pid);
}

bool
_create (const char *file, unsigned initial_size) {
	check_file_valid((void *)file);
	return filesys_create(file, initial_size);
}

bool
_remove (const char *file) {
	check_file_valid((void *)file);
	return filesys_remove(file);
}

int
_open (const char *file) {
	check_file_valid((void *)file);
	struct file *target;
	struct thread *curr = thread_current ();

	target = filesys_open(file);
	if (!target)
		return -1;

	for (int i=3; i<OPEN_MAX; i++){
		if (curr->fd_table[i] == NULL){
			curr->fd_table[i] = target;
			return i;
		}
	}

	// 만약 fd_table에 빈 공간이 없다면 닫는다.
	file_close(target);
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
	check_file_valid((void *) buffer);
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
			return -1;
		}
		read_len = file_read(target, buffer, size);
	}

	if (read_len != size)
		return -1;
	return read_len;
}

int
_write (int fd, const void *buffer, unsigned size) {
	check_file_valid((void *) buffer);
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
			return -1;
		}
		write_len = file_write(target, (void *)buffer, size);
	}
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
}
