/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include <bitmap.h>
#include "threads/mmu.h"
#include "threads/vaddr.h"

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

struct bitmap *swap_table;
// The swap area will be also managed at the granularity of PGSIZE
const size_t SECTORS_PER_PAGE = PGSIZE / DISK_SECTOR_SIZE;

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	/* TODO: Set up the swap_disk. */
	// you need to set up the swap disk.
	swap_disk = disk_get(1, 1);

	// need a data structure to manage free and used areas in the swap disk
	size_t swap_bit_cnt = disk_size(swap_disk) / SECTORS_PER_PAGE;
	swap_table = bitmap_create(swap_bit_cnt);
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {	
	/* Set up the handler */
	page->operations = &anon_ops;

	// add some information to the anon_page to support the swapping
	struct anon_page *anon_page = &page->anon;
	anon_page->swap_table_index = -1;
	
	return true;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;
	// The location of the data
	int page_no = anon_page->swap_table_index;
	if (!bitmap_test(swap_table, page_no))
		return false;

	// reading the data contents from the disk to memory.
	for (int i = 0; i < SECTORS_PER_PAGE; i++){
		disk_read(swap_disk, page_no * SECTORS_PER_PAGE + i, kva + DISK_SECTOR_SIZE * i);
	}

	// Remember to update the swap table
	bitmap_set(swap_table, page_no, false);				// 비었음 표시
	return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	// First, find a free swap slot in the disk
	int page_no = bitmap_scan(swap_table, 0, 1, false);

	// no more free slot in the disk, you can panic the kernel.
	if (page_no == BITMAP_ERROR)
		return false;

	// copy the page of data into the slot
	for (int i = 0; i < SECTORS_PER_PAGE; i++){
		disk_write(swap_disk, page_no * SECTORS_PER_PAGE + i, page->va + DISK_SECTOR_SIZE * i);
	}
	
	bitmap_set(swap_table, page_no, true);				// 사용 중 표시
	pml4_clear_page(thread_current()->pml4, page->va);	// 페이지 테이블에서는 지워주기

	// The location of the data should be saved in the page struct
	anon_page->swap_table_index = page_no;

	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;
}
