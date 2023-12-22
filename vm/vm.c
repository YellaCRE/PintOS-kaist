/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/mmu.h"

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;
	struct page *page = NULL;
	bool (*initializer) = NULL;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		// Create the page
		page = (struct page *)malloc(sizeof(struct page));
		
		// fetch the initialier
		switch (VM_TYPE(type)){
		case VM_UNINIT:
			// uninit이라 initializer가 없다
			break;

		case VM_ANON:
			initializer = anon_initializer;
			break;

		case VM_FILE:
			initializer = file_backed_initializer;
			break;

		case VM_PAGE_CACHE:
			// initializer = page_cache_initializer;
			break;

		default:
			NOT_REACHED();
		}

		// create "uninit" page struct
		uninit_new(page, upage, init, type, aux, initializer);
		page->writable = writable;
		
		/* TODO: Insert the page into the spt. */
		return spt_insert_page(spt, page);
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
	struct page *page = NULL;
	/* TODO: Fill this function. */
	struct list_elem *e;
	for (e=list_begin(&spt->supplemental_page_list); e!=list_end(&spt->supplemental_page_list); e=list_next(e)){
		page = list_entry(e, struct page, sp_elem);
		if (page->va == pg_round_down(va)){
			return page;
		}
	}

	return NULL;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED,
		struct page *page UNUSED) {
	int succ = false;
	/* TODO: Fill this function. */
	struct list_elem *e;
	struct page *parent_page = NULL;

	// check valid
	for (e=list_begin(&spt->supplemental_page_list); e!=list_end(&spt->supplemental_page_list); e=list_next(e)){
		parent_page = list_entry(e, struct page, sp_elem);
		if (parent_page->va == page->va){
			return succ;
		}
	}

	// insert supplemental_page	
	list_push_back(&spt->supplemental_page_list, &page->sp_elem);
	return true;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	vm_dealloc_page (page);
	return true;
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */

	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame = NULL;
	/* TODO: Fill this function. */
	// Gets a new physical page
	void *physical_page = palloc_get_page(PAL_USER);
	if (!physical_page){
		palloc_free_page(physical_page);
		PANIC("todo");
	}

	// also allocate a frame
	frame = (struct frame *)malloc(sizeof(struct frame));
	if (!frame){
		free(frame);
		PANIC("todo");
	}

	// initialize its members
	frame->kva = physical_page;
	frame->page = NULL;

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static void
vm_stack_growth (void *addr UNUSED) {
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
	struct page *page = NULL;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */

	if(is_kernel_vaddr(addr))
		return false;
	
	page = spt_find_page(spt, addr);
	
	if (!page)
		return false;

	return vm_do_claim_page (page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va UNUSED) {
	struct page *page = NULL;
	/* TODO: Fill this function */
	page = spt_find_page(&thread_current()->spt, va);

	if (!page)
		return false;

	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();

	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	if (!pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable))
		return false;

	return swap_in (page, frame->kva);
}

/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	list_init(&spt->supplemental_page_list);
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
	
	struct list_elem *e;
	struct page *parent_page	= NULL;
	struct page *child_page		= NULL;

	enum vm_type type;
	void *upage;
	bool writable;
	vm_initializer *init;
	void *aux;

	// Iterate through each page in the src's supplemental page table
	for (e=list_begin(&src->supplemental_page_list); e!=list_end(&src->supplemental_page_list); e=list_next(e)){
		parent_page = list_entry(e, struct page, sp_elem);
		
		// 변수 지정
		type 	 = page_get_type(parent_page);
		upage 	 = parent_page->va;
		writable = parent_page->writable;
		init 	 = parent_page->uninit.init;
		aux 	 = parent_page->uninit.aux;
		
		// allocate uninit page
		// VM_UNINIT
		if (parent_page->operations->type == VM_UNINIT){
			if(!vm_alloc_page_with_initializer(type, upage, writable, init, aux))
                return false;
		}
		// VM_ANON or VM_FILE
		else{
			if (!vm_alloc_page(type, upage, writable))
				return false;
			
			// claim them immediately
			if (!vm_claim_page(upage))
				return false;

			// make a exact copy of the entry in the dst's supplemental page table
			child_page = spt_find_page(dst, upage);
			memcpy(child_page->frame->kva, parent_page->frame->kva, PGSIZE);
		}
	}

	return true;
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	struct list_elem *e;
	struct page *e_page;
	
	// iterate through the page entries
	for (e=list_begin(&spt->supplemental_page_list); e!=list_end(&spt->supplemental_page_list); e=list_next(e)){
		e_page = list_entry(e, struct page, sp_elem);
		// call destroy(page) for the pages in the table
		list_remove(e);
		destroy(e_page);
	}
}
