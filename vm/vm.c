/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/mmu.h"
#include <string.h>

struct list frame_table;
struct lock frame_table_lock;
struct list_elem *now;

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
    list_init(&frame_table);
	lock_init(&frame_table_lock);
	now = list_begin(&frame_table);
}

// hash helper
static unsigned
page_hash (const struct hash_elem *page_elem, void *aux UNUSED) {
	const struct page *page = hash_entry (page_elem, struct page, hash_elem);
	
	return hash_bytes(&page->va, sizeof page->va);
}

static bool
page_less (const struct hash_elem *page_elem_a, const struct hash_elem *page_elem_b, void *aux UNUSED) {
	const struct page *page_a = hash_entry(page_elem_a, struct page, hash_elem);
	const struct page *page_b = hash_entry(page_elem_b, struct page, hash_elem);
	
	return page_a->va < page_b->va;
}

static struct page *
page_lookup (const void *va) {
	struct page page;
	struct hash_elem *page_elem;
	
	page.va = pg_round_down(va);
	page_elem = hash_find(&thread_current()->spt.supplemental_page_hash, &page.hash_elem);
	
	return page_elem != NULL ? hash_entry(page_elem, struct page, hash_elem) : NULL;
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

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		// Create the page
		page = (struct page *)malloc(sizeof(struct page));
		
		// fetch the initialier
		switch (VM_TYPE(type)){
		case VM_ANON:
			uninit_new(page, upage, init, type, aux, anon_initializer);
			break;

		case VM_FILE:
			uninit_new(page, upage, init, type, aux, file_backed_initializer);
			break;

		case VM_PAGE_CACHE:
			// initializer = page_cache_initializer;
			break;

		default:
			NOT_REACHED();
		}

		// create "uninit" page struct
		
		page->writable = writable;
		
		/* TODO: Insert the page into the spt. */
		return spt_insert_page(spt, page);
	}

	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
	struct page *page = NULL;
	/* TODO: Fill this function. */
	page = page_lookup(pg_round_down(va));
	
	if (!page) {
		return NULL;
	}
	
	return page;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED,
		struct page *page UNUSED) {
	int succ = false;
	/* TODO: Fill this function. */
	if (!hash_insert(&spt->supplemental_page_hash, &page->hash_elem)) {
		succ = true;
	}

	return succ;
}

void
spt_remove_page (struct supplemental_page_table *spt UNUSED, struct page *page) {
	// hash_delete(&spt->supplemental_page_hash, &page->hash_elem);

	vm_dealloc_page (page);
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */
    struct thread *curr = thread_current();
    
    lock_acquire(&frame_table_lock);
    for (; now != list_end(&frame_table); now = list_next(now)) {
        victim = list_entry(now, struct frame, frame_elem);

        if (pml4_is_accessed(curr->pml4, victim->page->va)) {
            pml4_set_accessed(curr->pml4, victim->page->va, 0);
        } else {
            lock_release(&frame_table_lock);
            return victim;
        }
    }

    struct list_elem *now = list_begin(&frame_table);

    for (; now != list_end(&frame_table); now = list_next(now)) {
        victim = list_entry(now, struct frame, frame_elem);

        if (pml4_is_accessed(curr->pml4, victim->page->va)) {
            pml4_set_accessed(curr->pml4, victim->page->va, 0);
        } else {
            lock_release(&frame_table_lock);
            return victim;
        }
    }
    
    lock_release(&frame_table_lock);
    ASSERT(now != NULL);
    return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */
    swap_out(victim->page);
	return victim;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame = NULL;
	/* TODO: Fill this function. */
    // also allocate a frame
	frame = (struct frame *)malloc(sizeof(struct frame));
    // Gets a new physical page
	frame->kva = palloc_get_page(PAL_USER);
	if (!frame->kva){
		frame = vm_evict_frame();
		frame->page = NULL;
        return frame;
	}

	// initialize its members
	frame->page = NULL;

	// lock을 걸어주어야 assertion 'intr_context ()'를 피할 수 있다
	lock_acquire(&frame_table_lock);
    list_push_back(&frame_table, &frame->frame_elem);
	lock_release(&frame_table_lock);

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static bool
vm_stack_growth(void *addr UNUSED) {
	// addr = stack_bottom - PGSIZE
	// Increases the stack size by allocating one or more anonymous pages
	if (vm_alloc_page(VM_MARKER_0 | VM_ANON, addr, true)) {
		// Make sure you round down the addr to PGSIZE
		thread_current()->stack_bottom -= PGSIZE;
		return true;
	}
	return false;
}

/* Handle the fault on write_protected page */
// static bool
// vm_handle_wp (struct page *page UNUSED) {
// }

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct page *page UNUSED = NULL;
	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
	void *rsp_stack;
	/* TODO: Validate the fault */
	/* TODO: Your code goes here */
	// you need to check whether the page fault is a valid case for a stack growth or not.
	if(!addr || is_kernel_vaddr(addr))
		return false;
	
	// exception인지 아니면 유저로 온 것인지 확인
	rsp_stack = is_kernel_vaddr(f->rsp) ? (void *) thread_current()->rsp_stack : (void *) f->rsp;
	if (not_present){
		// 일단 시도
		if (vm_claim_page (addr))
			return true;
		
		// 공간이 없어서 실패하면
		int compare_addr = (long int)addr;
		// a page fault 8 bytes below the stack pointer && limit the stack size to be 1MB
		if (rsp_stack - 8 <= addr && USER_STACK - (1<<20) <= compare_addr && compare_addr <= USER_STACK){
			void *fault_addr = thread_current()->stack_bottom - PGSIZE;
			// call vm_stack_growth with the faulted address.
			vm_stack_growth(fault_addr);
			// no longer a faulted address
			vm_claim_page(addr);
			return true;
		}
	}
	return false;
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
	hash_init(&spt->supplemental_page_hash, (hash_hash_func *)page_hash, page_less, NULL);
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
	
	struct page *parent_page	= NULL;
	struct page *child_page		= NULL;

	enum vm_type type;
	void *upage;
	bool writable;
	vm_initializer *init;
	void *aux;

	// Iterate through each page in the src's supplemental page table
	struct hash_iterator i;
	hash_first(&i, &src->supplemental_page_hash);
	while (hash_next(&i))
	{
		parent_page = hash_entry(hash_cur(&i), struct page, hash_elem);
		
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

// kill helper
static void
page_destroy(struct hash_elem *e, void *aux UNUSED) {
	struct page *page = hash_entry (e, struct page, hash_elem);
	vm_dealloc_page(page);
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	hash_destroy(&spt->supplemental_page_hash, page_destroy);
}
