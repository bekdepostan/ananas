#include <ananas/types.h>
#include <machine/param.h>
#include <machine/macro.h>
#include <machine/interrupts.h>
#include <machine/vm.h>
#include <machine/pcpu.h>
#include <machine/thread.h>
#include <ananas/x86/acpi.h>
#include <ananas/x86/pic.h>
#include <ananas/x86/pit.h>
#include <ananas/x86/smap.h>
#include <ananas/handle.h>
#include <ananas/bootinfo.h>
#include <ananas/kmem.h>
#include <ananas/init.h>
#include <ananas/vm.h>
#include <ananas/pcpu.h>
#include <ananas/mm.h>
#include <ananas/lib.h>
#include <loader/module.h>

/* Pointer to the page directory level 4 */
uint64_t* kernel_pagedir;

/* Global Descriptor Table */
uint8_t gdt[GDT_NUM_ENTRIES * 16];

/* Interrupt Descriptor Table */
uint8_t idt[IDT_NUM_ENTRIES * 16];

/* Boot CPU pcpu structure */
static struct PCPU bsp_pcpu;

/* TSS used by the kernel */
struct TSS kernel_tss;

/* Bootinfo as supplied by the loader, or NULL */
struct BOOTINFO* bootinfo = NULL;
static struct BOOTINFO _bootinfo;

#define PHYSMEM_NUM_CHUNKS 32
struct PHYSMEM_CHUNK {
	addr_t addr, len;
};
static struct PHYSMEM_CHUNK phys[PHYSMEM_NUM_CHUNKS];

/* CPU clock speed, in MHz */
int md_cpu_clock_mhz = 0;

static inline uint64_t
rdmsr(uint32_t msr)
{
	uint32_t hi, lo;

	__asm(
		"rdmsr\n"
	: "=a" (lo), "=d" (hi) : "c" (msr));
	return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static inline void
wrmsr(uint32_t msr, uint64_t val)
{
	__asm(
		"wrmsr\n"
	: : "a" (val & 0xffffffff),
	    "d" (val >> 32),
      "c" (msr));
}

static void*
bootstrap_get_pages(addr_t* avail, size_t num)
{
	void* ptr = (void*)*avail;
	memset(ptr, 0, num * PAGE_SIZE);
	*avail += num * PAGE_SIZE;
	return ptr;
}

typedef uint64_t (phys_flags_t)(void* ctx, addr_t phys, addr_t virt);

/*
 * Maps num_pages of phys -> virt; when pages are needed, *avail is used and incremented.
 *
 * get_flags(flags_ctx, phys, virt) will be called for every actual mapping to determine the
 * mapping-specific flags that are to be used.
 */
static void
map_kernel_pages(addr_t phys, addr_t virt, unsigned int num_pages, addr_t* avail, phys_flags_t* get_flags, void* flags_ctx)
{
#define ADDR_MASK 0xffffffffff000 /* bits 12 .. 51 */

	for (unsigned int n = 0; n < num_pages; n++) {
		uint64_t* pml4e = &kernel_pagedir[(virt >> 39) & 0x1ff];
		if (*pml4e == 0) {
			*pml4e = *avail | PE_RW | PE_P | PE_C_G;
			*avail += PAGE_SIZE;
		}
		uint64_t* p = (uint64_t*)(*pml4e & ADDR_MASK);
		uint64_t* pdpe = &p[(virt >> 30) & 0x1ff];
		if (*pdpe == 0) {
			*pdpe = *avail | PE_RW | PE_P | PE_C_G;
			*avail += PAGE_SIZE;
		}
		uint64_t* q = (uint64_t*)(*pdpe & ADDR_MASK);
		uint64_t* pde = &q[(virt >> 21) & 0x1ff];
		if (*pde == 0) {
			*pde = *avail | PE_RW | PE_P | PE_C_G;
			*avail += PAGE_SIZE;
		}

		uint64_t* r = (uint64_t*)(*pde & ADDR_MASK);
		r[(virt >> 12) & 0x1ff] = phys | get_flags(flags_ctx, phys, virt);
		virt += PAGE_SIZE;
		phys += PAGE_SIZE;
	}

#undef ADDR_MASK
}

struct AVAIL_CTX {
	addr_t avail_start;
	addr_t* avail_end;
};

static uint64_t
kva_get_flags(void* ctx, addr_t phys, addr_t virt)
{
	struct AVAIL_CTX* actx = ctx;
	if (phys >= actx->avail_start && phys <= *actx->avail_end)
		return PE_G | PE_RW | PE_P;
	return 0;
}

static uint64_t
kmem_get_flags(void* ctx, addr_t phys, addr_t virt)
{
	addr_t kernel_text_end = *(addr_t*)ctx;
	uint64_t flags = PE_G | PE_P;
	if (virt > kernel_text_end)
		flags |= PE_RW;
	return flags;
}

static uint64_t
dynkva_get_flags(void* ctx, addr_t phys, addr_t virt)
{
	/* We just setup space for mappings; not the actual mappings themselves */
	return 0;
}

/*
 * Calculated how many PAGE_SIZE-sized pieces we need to map mem_size bytes.
 */
static inline void
calculate_num_pages_required(size_t mem_size, unsigned int* num_pages_needed, unsigned int* length_in_pages)
{
	/* Number of level-4 page-table entries required; these map bits 63..39 */
	unsigned int num_pml4e = (mem_size + (1ULL << 39) - 1) >> 39;
	/* Number of level-3 page-table entries required; these map bits 38..30 */
	unsigned int num_pdpe = (mem_size + (1ULL << 30) - 1) >> 30;
	/* Number of level-2 page-table entries required; these map bits 29..21 */
	unsigned int num_pde = (mem_size + (1ULL << 21) - 1) >> 21;
	/* Number of level-1 page-table entries required; these map bits 20..12 */
	unsigned int num_pte = (mem_size + (1ULL << 12) - 1) >> 12;
	*num_pages_needed = num_pml4e + num_pdpe + num_pde;
	*length_in_pages = num_pte;
}

static void
setup_paging(addr_t* avail, size_t mem_size, size_t kernel_size)
{
#define KMAP_KVA_START KMEM_DIRECT_VA_START
#define KMAP_KVA_END KMEM_DYNAMIC_VA_END
	/*
	 * Taking the overview in machine/vm.h into account, we want to map the
	 * following regions:
	 *
	 * - KMAP_KVA_START .. KMAP_KVA_END: the kernel's KVA
	 *   This may be mapped as 4KB pages; however, we can lower the estimate if
	 *   there is less memory available than the total size of this region.
	 * - KERNBASE ... KERNEND: the kernel code/data
	 *   We always map this as 4KB pages to ensure we can benefit most optimally
	 *   from NX.
	 */
	uint64_t kmap_kva_end = KMAP_KVA_END;
	if (kmap_kva_end - KMAP_KVA_START >= mem_size) {
		/* Tone down KVA as we have less memory */
		kmap_kva_end = KMAP_KVA_START + mem_size;
	}

	// XXX Ensure we'll have at least 4GB KVA; this is needed in order to map PCI
	// devices (we should actually just add specific mappings for this place instead)
	if (mem_size < 4UL * 1024 * 1024 * 1024)
		kmap_kva_end = KMAP_KVA_START + 4UL * 1024 * 1024 * 1024;

	uint64_t kva_size = kmap_kva_end - KMAP_KVA_START;
	unsigned int kva_pages_needed, kva_size_in_pages;
	calculate_num_pages_required(kva_size, &kva_pages_needed, &kva_size_in_pages);
	addr_t kva_pages = (addr_t)bootstrap_get_pages(avail, kva_pages_needed);

	/* Finally, allocate the kernel pagedir itself */
	kernel_pagedir = (uint64_t*)bootstrap_get_pages(avail, 1);
	kprintf(">>> kernel_pagedir = %p\n", kernel_pagedir);

	addr_t avail_start = (((struct LOADER_MODULE*)(addr_t)bootinfo->bi_modules)->mod_phys_end_addr);
	avail_start = (avail_start | (PAGE_SIZE - 1)) + 1;

	/*
	 * Calculate the number of entries we need for the kernel itself; we just
	 * need to grab the bare necessities as we'll map all other memory using the
	 * KVA instead. We do this here to update the 'avail' pointer to ensure
	 * we'll map these tables in KVA as well, should we ever need to change them.
	 *
	 * XXX We could consider mapping the kernel using 2MB pages if the NX bits align...
 	 */
	unsigned int kernel_pages_needed, kernel_size_in_pages;
	calculate_num_pages_required(kernel_size, &kernel_pages_needed, &kernel_size_in_pages);
	addr_t kernel_pages = (addr_t)bootstrap_get_pages(avail, kernel_pages_needed);

	/*
	 * Calculate the amount we need for dynamic KVA mappings; this is likely a
	 * gross overstatement because we can typically map everything 1:1 since we
	 * have address space plenty...
	 */
	unsigned int dyn_kva_pages_needed, dyn_kva_size_in_pages;
	calculate_num_pages_required(KMEM_DYNAMIC_VA_END - KMEM_DYNAMIC_VA_START, &dyn_kva_pages_needed, &dyn_kva_size_in_pages);
	addr_t dyn_kva_pages = (addr_t)bootstrap_get_pages(avail, dyn_kva_pages_needed);

	/*
	 * Map the KVA - we will only map what we have used for our page tables, to
	 * ensure we can change them later as necessary. We explicitly won't map the
	 * kernel page tables here because we never need to change them.
	 */
	struct AVAIL_CTX actx = { avail_start, avail };
	addr_t kva_avail_ptr = (addr_t)kva_pages;
	map_kernel_pages(0, KMAP_KVA_START, kva_size_in_pages, &kva_avail_ptr, kva_get_flags, &actx);
	KASSERT(kva_avail_ptr == (addr_t)kva_pages + kva_pages_needed * PAGE_SIZE, "not all KVA pages used (used %d, expected %d)", (kva_avail_ptr - kva_pages) / PAGE_SIZE, kva_pages);

	/* Now map the kernel itself */
	extern void *__entry, *__rodata_end;
	addr_t kernel_addr = (addr_t)&__entry & ~(PAGE_SIZE - 1);
	addr_t kernel_text_end  = (addr_t)&__rodata_end & ~(PAGE_SIZE - 1);

	addr_t kernel_avail_ptr = (addr_t)kernel_pages;
	map_kernel_pages(kernel_addr & 0x00ffffff, kernel_addr, kernel_size_in_pages, &kernel_avail_ptr, kmem_get_flags, &kernel_text_end);
	KASSERT(kernel_avail_ptr == (addr_t)kernel_pages + kernel_pages_needed * PAGE_SIZE, "not all kernel pages used (used %d, expected %d)", (kernel_avail_ptr - kernel_pages) / PAGE_SIZE, kernel_pages_needed);

	/* And map the dynamic KVA pages */
	addr_t dyn_kva_avail_ptr = dyn_kva_pages;
	map_kernel_pages(0, KMEM_DYNAMIC_VA_START, dyn_kva_size_in_pages, &dyn_kva_avail_ptr, dynkva_get_flags, NULL);
	KASSERT(dyn_kva_avail_ptr == (addr_t)dyn_kva_pages + dyn_kva_pages_needed * PAGE_SIZE, "not all dynamic KVA pages used (used %d, expected %d)", (dyn_kva_avail_ptr - dyn_kva_pages) / PAGE_SIZE, dyn_kva_pages_needed);

	/* Enable global pages */
	__asm(
		"movq	%cr4, %rax\n"
		"orq	$0x80, %rax\n"		/* PGE */
		"movq	%rax, %cr4\n"
	);

	/* Activate our new page tables */
	kprintf(">>> activating kernel_pagedir = %p\n", kernel_pagedir);
	__asm(
		"movq %0, %%cr3\n"
	: : "r" (kernel_pagedir));

	/*
	 * Now we must update the kernel pagedir pointer to the new location, as that
	 * we have paging in place.
	 */
	kernel_pagedir = (uint64_t*)PTOKV((addr_t)kernel_pagedir);
	kprintf(">>> kernel_pagedir = %p\n", kernel_pagedir);
	kprintf(">>> *kernel_pagedir = %p\n", *kernel_pagedir);
}

void
md_startup(struct BOOTINFO* bootinfo_ptr)
{
	/*
	 * This function will be called by the loader, which hands us a bootinfo
	 * structure containing a lot of useful information. We need it as soon
	 * as possible to bootstrap the VM and initialize the memory manager.
	 *
	 * Note that this implicitely assumes that the bootinfo memory is mapped -
	 * indeed, we'll assume that the entire 1GB memory space is mapped. This
	 * makes setting up our page tables much easier as we'll always have enough
	 * space to store them.
	 */
	if (bootinfo_ptr != NULL) {
		/*
		 * Copy enough bytes to cover our bootinfo - but only activate the bootinfo
	   * if the size is correct.
		 */
		memcpy(&_bootinfo, bootinfo_ptr, sizeof(struct BOOTINFO));
		if (bootinfo_ptr->bi_size >= sizeof(struct BOOTINFO))
			bootinfo = (struct BOOTINFO*)&_bootinfo;
	}
	if (bootinfo == NULL)
		panic("going nowhere without my bootinfo");

	/*
	 * Initialize a new Global Descriptor Table; the trampoline adds a lot of
	 * now-useless fields which we can do without, and we'll need things like a
	 * TSS.
	 */
	memset(gdt, 0, sizeof(gdt));
	GDT_SET_CODE64(gdt, GDT_SEL_KERNEL_CODE, SEG_DPL_SUPERVISOR);
	GDT_SET_DATA64(gdt, GDT_SEL_KERNEL_DATA, SEG_DPL_SUPERVISOR);
	GDT_SET_CODE64(gdt, GDT_SEL_USER_CODE, SEG_DPL_USER);
	GDT_SET_DATA64(gdt, GDT_SEL_USER_DATA, SEG_DPL_USER);
	GDT_SET_TSS64 (gdt, GDT_SEL_TASK, 0, (addr_t)&kernel_tss, sizeof(struct TSS));
	MAKE_RREGISTER(gdtr, gdt, GDT_LENGTH - 1);

	/* Load the GDT, and reload our registers */
	__asm(
		"lgdt (%%rax)\n"
		"mov %%bx, %%ds\n"
		"mov %%bx, %%es\n"
		"mov %%bx, %%fs\n"
		"mov %%bx, %%gs\n"
		"mov %%bx, %%ss\n"
		/* Jump to our new %cs */
		"pushq	%%rcx\n"
		"pushq	$1f\n"
		"lretq\n"
"1:\n"
	: : "a" (&gdtr),
 	    "b" (GDT_SEL_KERNEL_DATA),
	    "c" (GDT_SEL_KERNEL_CODE));

	/* Ask the PIC to mask everything; we'll initialize when we are ready */
	x86_pic_mask_all();

	/*
	 * Construct the IDT; this ensures we can handle exceptions, which is useful
	 * and we'll want to have as soon as possible.
	 */
	memset(idt, 0, sizeof(idt));
	IDT_SET_ENTRY( 0, SEG_TGATE_TYPE, 0, exception0);
	IDT_SET_ENTRY( 1, SEG_TGATE_TYPE, 0, exception1);
	IDT_SET_ENTRY( 2, SEG_TGATE_TYPE, 0, exception2);
	IDT_SET_ENTRY( 3, SEG_TGATE_TYPE, 0, exception3);
	IDT_SET_ENTRY( 4, SEG_TGATE_TYPE, 0, exception4);
	IDT_SET_ENTRY( 5, SEG_TGATE_TYPE, 0, exception5);
	IDT_SET_ENTRY( 6, SEG_TGATE_TYPE, 0, exception6);
	IDT_SET_ENTRY( 7, SEG_TGATE_TYPE, 0, exception7);
	IDT_SET_ENTRY( 8, SEG_TGATE_TYPE, 0 /* 1 */, exception8); /* use IST1 for double fault */
	IDT_SET_ENTRY( 9, SEG_TGATE_TYPE, 0, exception9);
	IDT_SET_ENTRY(10, SEG_TGATE_TYPE, 0, exception10);
	IDT_SET_ENTRY(11, SEG_TGATE_TYPE, 0, exception11);
	IDT_SET_ENTRY(12, SEG_TGATE_TYPE, 0, exception12);
	IDT_SET_ENTRY(13, SEG_TGATE_TYPE, 0, exception13);
	/*
	 * Page fault must disable interrupts to ensure %cr2 (fault address) will not
	 * be overwritten; it will re-enable the interrupt flag when it's safe to do
	 * so.
	 */
	IDT_SET_ENTRY(14, SEG_IGATE_TYPE, 0, exception14);
	IDT_SET_ENTRY(16, SEG_TGATE_TYPE, 0, exception16);
	IDT_SET_ENTRY(17, SEG_TGATE_TYPE, 0, exception17);
	IDT_SET_ENTRY(18, SEG_TGATE_TYPE, 0, exception18);
	IDT_SET_ENTRY(19, SEG_TGATE_TYPE, 0, exception19);
	/* Use interrupt gates for IRQ's so that we can keep track of the nesting */
	IDT_SET_ENTRY(32, SEG_IGATE_TYPE, 0, irq0);
	IDT_SET_ENTRY(33, SEG_IGATE_TYPE, 0, irq1);
	IDT_SET_ENTRY(34, SEG_IGATE_TYPE, 0, irq2);
	IDT_SET_ENTRY(35, SEG_IGATE_TYPE, 0, irq3);
	IDT_SET_ENTRY(36, SEG_IGATE_TYPE, 0, irq4);
	IDT_SET_ENTRY(37, SEG_IGATE_TYPE, 0, irq5);
	IDT_SET_ENTRY(38, SEG_IGATE_TYPE, 0, irq6);
	IDT_SET_ENTRY(39, SEG_IGATE_TYPE, 0, irq7);
	IDT_SET_ENTRY(40, SEG_IGATE_TYPE, 0, irq8);
	IDT_SET_ENTRY(41, SEG_IGATE_TYPE, 0, irq9);
	IDT_SET_ENTRY(42, SEG_IGATE_TYPE, 0, irq10);
	IDT_SET_ENTRY(43, SEG_IGATE_TYPE, 0, irq11);
	IDT_SET_ENTRY(44, SEG_IGATE_TYPE, 0, irq12);
	IDT_SET_ENTRY(45, SEG_IGATE_TYPE, 0, irq13);
	IDT_SET_ENTRY(46, SEG_IGATE_TYPE, 0, irq14);
	IDT_SET_ENTRY(47, SEG_IGATE_TYPE, 0, irq15);

	/* Load the IDT */
	MAKE_RREGISTER(idtr, idt, (IDT_NUM_ENTRIES * 16) - 1);
	__asm(
		"lidt (%%rax)\n"
	: : "a" (&idtr));

	/* Set up the userland %fs/%gs base adress to zero (it should be, but don't take chances) */
	wrmsr(MSR_FS_BASE, 0);
	wrmsr(MSR_GS_BASE, 0);

	/*
	 * Set up the kernel / current %gs base; this points to our per-cpu data; the
	 * current %gs base must be set because the interrupt code will not swap it
	 * if we came from kernel context.
	 */
	wrmsr(MSR_GS_BASE, (addr_t)&bsp_pcpu); // XXX Fixen crasht zonder dit
	wrmsr(MSR_KERNEL_GS_BASE, (addr_t)&bsp_pcpu);
	
	/*
	 * Load the kernel TSS; this is still needed to switch stacks between
	 * ring 0 and 3 code.
	 */
	memset(&kernel_tss, 0, sizeof(struct TSS));
extern void* bootstrap_stack;
	kernel_tss.ist1 = (addr_t)&bootstrap_stack;
	__asm("ltr %%ax\n" : : "a" (GDT_SEL_TASK));

	/*
	 * Set up the fast system call (SYSCALL/SYSEXIT) mechanism.
	 */
	wrmsr(MSR_EFER, rdmsr(MSR_EFER) | MSR_EFER_SCE);
	wrmsr(MSR_STAR, ((uint64_t)(GDT_SEL_USER_CODE - 0x10) | SEG_DPL_USER) << 48L |
                  ((uint64_t)GDT_SEL_KERNEL_CODE << 32L));
extern void* syscall_handler;
	wrmsr(MSR_LSTAR, (addr_t)&syscall_handler);
	wrmsr(MSR_SFMASK, 0x200 /* IF */);

	/*
	 * Determine how much memory we have; we need this in order to pre-allocate
	 * our page tables.
	 */
	if (bootinfo->bi_memory_map_addr == (addr_t)NULL ||
	    bootinfo->bi_memory_map_size == 0 ||
	    (bootinfo->bi_memory_map_size % sizeof(struct SMAP_ENTRY)) != 0)
		panic("going nowhere without a memory map");

	/*
	 * The loader tells us how large the kernel is; we use pages directly after
	 * the kernel.
	 */
	extern void *__entry, *__end;
	addr_t avail = (addr_t)(((struct LOADER_MODULE*)(addr_t)bootinfo_ptr->bi_modules)->mod_phys_end_addr);
	avail = (addr_t)((addr_t)avail | (PAGE_SIZE - 1)) + 1;
	kprintf("avail = %p\n", avail);

	/*
	 * Figure out where the kernel is in physical memory; we must exclude it from
	 * the memory maps.
	 */
	addr_t kernel_from = ((addr_t)&__entry - KERNBASE) & ~(PAGE_SIZE - 1);
	addr_t kernel_to = (((addr_t)&__end - KERNBASE) | (PAGE_SIZE - 1)) + 1;

	/* Now build the memory chunk list */
	int phys_idx = 0;

	size_t mem_size = 0;
	struct SMAP_ENTRY* smap_entry = (void*)(uintptr_t)bootinfo->bi_memory_map_addr;
	for (int i = 0; i < bootinfo->bi_memory_map_size / sizeof(struct SMAP_ENTRY); i++, smap_entry++) {
		if (smap_entry->type != SMAP_TYPE_MEMORY)
			continue;

		/* This piece of memory is available; add it */
		addr_t base = (addr_t)smap_entry->base_hi << 32 | smap_entry->base_lo;
		size_t len = (size_t)smap_entry->len_hi << 32 | smap_entry->len_lo;
		base  = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
		len  &= ~(PAGE_SIZE - 1);

		/* See if this chunk collides with our kernel; if it does, add it in 0 .. 2 slices */
#define MAX_SLICES 2
		addr_t start[MAX_SLICES], end[MAX_SLICES];
		kmem_chunk_reserve(base, base + len, kernel_from, kernel_to, &start[0], &end[0]);
		for (unsigned int n = 0; n < MAX_SLICES; n++) {
			KASSERT(start[n] <= end[n] && end[n] >= start[n], "invalid start/end pair %p/%p", start[n], end[n]);
			if (start[n] == end[n])
				continue;
			// XXX We should check to make sure we don't go beyond phys_... here
			phys[phys_idx].addr = start[n];
			phys[phys_idx].len = end[n] - start[n];
			mem_size += phys[phys_idx].len;
			phys_idx++;
		}
#undef MAX_SLICES
	}
	KASSERT(phys_idx > 0, "no physical memory chunks");

	/* Dump the physical memory layout as far as we know it */
	kprintf("physical memory dump: %d chunk(s), %d MB\n", phys_idx, mem_size / (1024 * 1024));
	for (int n = 0; n < phys_idx; n++) {
		struct PHYSMEM_CHUNK* p = &phys[n];
		kprintf("  chunk %d: %016p-%016p (%u KB)\n",
		 n, p->addr, p->addr + p->len - 1,
		 p->len / 1024);
	}
	uint64_t prev_avail = avail;
	setup_paging(&avail, mem_size, kernel_to - kernel_from);

	/*
	 * Now add the physical chunks of memory. Note that phys[] isn't up-to-date
	 * anymore as it will contain things we have used for the page tables - this
	 * is why we store prev_avail: it's always at the start of a chunk, and we can
	 * just update the chunk as needed.
	 */
	for (int n = 0; n < phys_idx; n++) {
		struct PHYSMEM_CHUNK* p = &phys[n];
		if (p->addr == prev_avail) {
			kprintf("altering prev chunk\n");
			p->addr = avail;
			p->len -= avail - prev_avail;
		}
		kprintf("+adding memory chunk %p (%p)\n", p->addr, p->len);
		page_zone_add(p->addr, p->len);
		kprintf("-adding memory chunk %p (%p)\n", p->addr, p->len);
	}

	/* We have memory - initialize our VM */
	vm_init();

	/* Enable fast system calls */
	__asm(
		"movq	%cr4, %rax\n"
		"orq	$0x600, %rax\n"		/* OSFXSR | OSXMMEXCPT */
		"movq	%rax, %cr4\n"
	);

	/* Enable the memory code - handle_init() calls kmalloc() */
	mm_init();

	/* Initialize the handles; this is needed by the per-CPU code as it initialize threads */
	handle_init();

	/*
	 * Initialize the per-CPU thread; this needs a working memory allocator, so that is why
	 * we delay it.
	 */
	memset(&bsp_pcpu, 0, sizeof(bsp_pcpu));
	pcpu_init(&bsp_pcpu);
	bsp_pcpu.tss = (addr_t)&kernel_tss;

	/*
	 * Switch to the idle thread - the reason we do it here is because it removes the
	 * curthread==NULL case and it will has a larger stack than our bootstrap stack. The
	 * bootstrap stack is re-used as double fault stack.
	 */
	__asm __volatile(
		"movq %0, %%rsp\n"
	: : "r" (bsp_pcpu.idlethread->md_rsp));
	PCPU_SET(curthread, bsp_pcpu.idlethread);
	scheduler_add_thread(bsp_pcpu.idlethread);

  /*
   * If we have ACPI, do a pre-initialization; we need this mostly in the SMP
   * case because we're going to parse tables if we can.
   */
#ifdef OPTION_ACPI
	acpi_init();
#endif

	/*
	 * XXX No SMP here yet, so use the PIC for now.
	 */
	x86_pic_init();

	/* Initialize the PIT */
	x86_pit_init();

	/*
	 * Enable interrupts. We do this right before the machine-independant code
	 * because it will allow us to rely on interrupts when probing devices etc.
	 *
	 * Note that we explicitely block the scheduler, as this only should be
	 * enabled once we are ready to run userland threads.
	 */
	md_interrupts_enable();

	/* Find out how quick the CPU is; this requires interrupts and will be needed for delay() */
	md_cpu_clock_mhz = x86_pit_calc_cpuspeed_mhz();

	/* All done - it's up to the machine-independant code now */
	mi_startup();
}

/* vim:set ts=2 sw=2: */
