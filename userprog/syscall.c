#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "filesys/filesys.h"

#define STDIN_FILENO 0
#define STDOUT_FILENO 1

void syscall_entry (void);
void syscall_handler (struct intr_frame * UNUSED);

void check_valid(void *ptr);

void halt (void);
void exit (int status);

pid_t fork (const char *thread_name);
int exec (const char *cmd_line);
int wait (pid_t pid);

bool create (const char *file, unsigned initial_size);
bool remove (const char *file);
int open (const char *file);
int filesize (int fd);
int read (int fd, void *buffer, unsigned size);
int write (int fd, const void *buffer, unsigned size);
void seek (int fd, unsigned position);
unsigned tell (int fd);
void close (int fd);

/*
pid_t fork (const char *thread_name);
int exec (const char *file);
int wait (pid_t pid);
*/

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
	// hex_dump(f->rsp, f->rsp, USER_STACK - f->rsp, true);

	switch (f->R.rax){
		case SYS_HALT:
			halt();
			break;
		
		case SYS_EXIT:
			exit(f->R.rdi);
			break;

		case SYS_FORK:
			f->R.rax = fork(f->R.rdi);
			break;

		case SYS_EXEC:
			f->R.rax = exec(f->R.rdi);
			break;

		case SYS_WAIT:
			f->R.rax = wait(f->R.rdi);
			break;

		case SYS_CREATE:
			f->R.rax = create(f->R.rdi, f->R.rsi);
			break;

		case SYS_REMOVE:
			f->R.rax = remove(f->R.rdi);
			break;

		case SYS_OPEN:
			f->R.rax = open(f->R.rdi);
			break;

		case SYS_FILESIZE:
			f->R.rax = filesize(f->R.rdi);
			break;
		
		case SYS_READ:
			f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
			break;

		case SYS_WRITE:
			f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
			break;

		case SYS_SEEK:
			seek(f->R.rdi, f->R.rsi);
			break;

		case SYS_TELL:
			f->R.rax = tell(f->R.rdi);
			break;

		case SYS_CLOSE:
			close(f->R.rdi);
			break;
	}
}

/* ========== kernel-level function ==========*/

void
check_valid(void *ptr){
	if (!ptr || is_kernel_vaddr(ptr) || !(pml4_get_page(thread_current()->pml4, ptr)))
		exit(-1);
}

void
halt (void) {
	power_off();
}

void
exit (int status) {
	struct thread *curr = thread_current ();
	curr->exit_code = status;
	printf ("%s: exit(%d)\n", curr->name, curr->exit_code);
	thread_exit ();
}

pid_t
fork (const char *thread_name){
	int child_pid = process_fork(thread_name);
	return child_pid;
}

int
exec (const char *cmd_line) {
	if (process_exec(cmd_line) == -1)
		return -1;
	NOT_REACHED();
}

int
wait (pid_t pid) {
	if (process_wait(pid) == -1)
		return -1;
}

bool
create (const char *file, unsigned initial_size) {
	check_valid(file);
	return filesys_create(file, initial_size);
}

bool
remove (const char *file) {
	check_valid(file);
	return filesys_remove(file);
}

int
open (const char *file) {
	check_valid(file);
	struct file *target = filesys_open(file);
	if (!target)
		return -1;

	struct thread *curr = thread_current ();
	for (int i=3; i<64; i++){
		if (curr->fd_table[i] == NULL)
			curr->fd_table[i] = target;
			return i;
	}

	// 만약 fd_table에 빈 공간이 없다면 열지 않는다.
	file_close(target);
	return -1;
}

int
filesize (int fd) {
	struct thread *curr = thread_current();
	struct file *target = curr->fd_table[fd];
	if (!target)
		return -1;
	return file_length(target);
}

int
read (int fd, void *buffer, unsigned size) {
	struct thread *curr;
	unsigned read_len;
	uint8_t key;
	
	if (fd == STDIN_FILENO){
		key = input_getc();
		read_len = file_read(key, buffer, size);
	}
	else{
		curr = thread_current ();
		read_len = file_read(curr->fd_table[fd], buffer, size);
	}

	if (read_len != size)
		return -1;
	return read_len;
}

int
write (int fd, const void *buffer, unsigned size) {
	struct thread *curr;
	unsigned write_len;

	if (fd == STDOUT_FILENO){
		putbuf(buffer, size);
		write_len = size;
	}
	else{
		curr = thread_current ();
		write_len = file_read(curr->fd_table[fd], buffer, size);
	}

	if (write_len != size)
		return -1;
	return write_len;
}

void
seek (int fd, unsigned position) {
	file_seek(thread_current()->fd_table[fd], position);
}

unsigned
tell (int fd) {
	return file_tell(thread_current()->fd_table[fd]);
}

void
close (int fd) {
	if (fd<2 | fd>63)
		return;

	struct thread *curr = thread_current ();
	struct file *target = curr->fd_table[fd];
	if (!target)
		return;
	
	curr->fd_table[fd] = NULL;
	file_close(target);	// 이거 안된다
}

/*
int
dup2 (int oldfd, int newfd){
	return syscall2 (SYS_DUP2, oldfd, newfd);
}

void *
mmap (void *addr, size_t length, int writable, int fd, off_t offset) {
	return (void *) syscall5 (SYS_MMAP, addr, length, writable, fd, offset);
}

void
munmap (void *addr) {
	syscall1 (SYS_MUNMAP, addr);
}

bool
chdir (const char *dir) {
	return syscall1 (SYS_CHDIR, dir);
}

bool
mkdir (const char *dir) {
	return syscall1 (SYS_MKDIR, dir);
}

bool
readdir (int fd, char name[READDIR_MAX_LEN + 1]) {
	return syscall2 (SYS_READDIR, fd, name);
}

bool
isdir (int fd) {
	return syscall1 (SYS_ISDIR, fd);
}

int
inumber (int fd) {
	return syscall1 (SYS_INUMBER, fd);
}

int
symlink (const char* target, const char* linkpath) {
	return syscall2 (SYS_SYMLINK, target, linkpath);
}

int
mount (const char *path, int chan_no, int dev_no) {
	return syscall3 (SYS_MOUNT, path, chan_no, dev_no);
}

int
umount (const char *path) {
	return syscall1 (SYS_UMOUNT, path);
}
*/
