/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "threads/mmu.h"
#include "userprog/process.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void
vm_file_init (void) {
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &file_ops;

	struct file_page *file_page = &page->file;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}

/* Do the mmap */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
	// virtual pages starting at addr
	void * mapped_va = addr;

	// Set these bytes to zero
    size_t read_bytes = length > file_length(file) ? file_length(file) : length;
    size_t zero_bytes = PGSIZE - read_bytes % PGSIZE;
	
	// obtain a separate and independent reference
	struct file *mapping_file = file_reopen(file);

	// starting from offset byte
	while (read_bytes > 0 || zero_bytes > 0) {
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		struct load_info *aux;
		aux = (struct load_info *)malloc(sizeof(struct load_info));

		aux->file = mapping_file;
		aux->ofs = offset;
		aux->page_read_bytes = page_read_bytes;

		// return NULL which is not a valid address to map a file
		if (!vm_alloc_page_with_initializer (VM_FILE, addr,
					writable, lazy_load_segment, aux)){
			free(aux);
			return NULL;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		offset += page_read_bytes;
		addr += PGSIZE;
	}
	// returns the virtual address where the file is mapped
	return mapped_va;
}

/* Do the munmap */
void
do_munmap (void *addr) {
	// the specified address range addr
	struct thread *curr = thread_current();
	struct page *page;
	struct load_info * aux;

	// 파일이 끝날 때까지 반복
	while (true){
		// 파일 찾기
		page = spt_find_page(&curr->spt, addr);
        // 파일의 끝인지 확인
		if (!page || page_get_type(page) != VM_FILE)
            break;
		
		// written back to the file
		if (pml4_is_dirty(curr->pml4, page->va)){
			aux = (struct load_info *) page->uninit.aux;
			file_write_at(aux->file, addr, aux->page_read_bytes, aux->ofs);
            pml4_set_dirty (curr->pml4, page->va, 0);
		}

		// unmap
		pml4_clear_page(curr->pml4, page->va);
		addr += PGSIZE;
	}
}
