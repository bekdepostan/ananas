#include <ananas/types.h>
#include <machine/debug.h>
#include <machine/frame.h>
#include <machine/param.h>
#include <machine/thread.h>
#include <machine/vm.h>
#include <machine/macro.h>
#include <machine/interrupts.h>
#include <ananas/error.h>
#include <ananas/lib.h>
#include <ananas/mm.h>
#include <ananas/page.h>
#include <ananas/pcpu.h>
#include <ananas/thread.h>
#include <ananas/threadinfo.h>
#include <ananas/vm.h>
#include <ananas/vmspace.h>
#include "options.h"

extern struct TSS kernel_tss;
void clone_return();
void userland_trampoline();
void kthread_trampoline();
extern uint32_t* kernel_pd;

errorcode_t
md_thread_init(thread_t* t, int flags)
{
	/*
	 * Create the user stack page piece, if we are not cloning - otherwise, we'll
	 * copy the parent's stack instead.
	 */
	if ((flags & THREAD_ALLOC_CLONE) == 0) {
		vmarea_t* va;
		errorcode_t err = vmspace_mapto(t->t_vmspace, USERLAND_STACK_ADDR, 0, THREAD_STACK_SIZE, VM_FLAG_USER | VM_FLAG_READ | VM_FLAG_WRITE | VM_FLAG_ALLOC | VM_FLAG_MD, &va);
		ANANAS_ERROR_RETURN(err);
	}

	/* Create the kernel stack for this thread; this is fixed-length and always mapped */
	t->md_kstack = kmalloc(KERNEL_STACK_SIZE);

	/* Fill our the %esp and %cr3 fields; we'll be started in supervisor mode, so use the appropriate stack */
	t->md_cr3 = KVTOP((addr_t)t->t_vmspace->vs_md_pagedir);
	t->md_esp0 = (addr_t)t->md_kstack + KERNEL_STACK_SIZE;
	t->md_esp = t->md_esp0;
	t->md_eip = (addr_t)&userland_trampoline;

	/* initialize FPU state similar to what finit would do */
	t->md_fpu_ctx.cw = 0x37f;
	t->md_fpu_ctx.tw = 0xffff;
	return ANANAS_ERROR_OK;
}

errorcode_t
md_kthread_init(thread_t* t, kthread_func_t kfunc, void* arg)
{
	/* Set up the environment */
	t->md_eip = (addr_t)&kthread_trampoline;
	t->md_arg1 = (addr_t)kfunc;
	t->md_arg2 = (addr_t)arg;
	t->md_cr3 = KVTOP((addr_t)kernel_pd);

	/*
	 * Kernel threads share the kernel pagemap and thus need to map the kernel
	 * stack. We do not differentiate between kernel and userland stacks as
	 * no kernelthread ever runs userland code.
	 */
	t->md_kstack = kmalloc(KERNEL_STACK_SIZE);
	t->md_esp = (addr_t)t->md_kstack + KERNEL_STACK_SIZE - 4;
	return ANANAS_ERROR_OK;
}

void
md_thread_free(thread_t* t)
{
	/*
	 * This is a royal pain: we are about to destroy the thread's mappings, but this also means
	 * we destroy the thread's stack - and it won't be able to continue. To prevent this, we
	 * can only be called from another thread (this means this code cannot be run until the
	 * thread to be destroyed is a zombie)
	 */
	KASSERT(THREAD_IS_ZOMBIE(t), "cannot free non-zombie thread");
	KASSERT(PCPU_GET(curthread) != t, "cannot free current thread");

	/* Throw away the kernel stack; it is no longer in use so it can go */
	kfree(t->md_kstack);
}

thread_t*
md_thread_switch(thread_t* new, thread_t* old)
{
	KASSERT(md_interrupts_save() == 0, "interrupts must be disabled");
	KASSERT(!THREAD_IS_ZOMBIE(new), "cannot switch to a zombie thread");
	KASSERT(new != old, "switching to self?");
	KASSERT(THREAD_IS_ACTIVE(new), "new thread isn't running?");

	/* XXX Safety nets to ensure we won't restore a bogus stack */
	KASSERT(new->md_esp > (addr_t)new->md_kstack, "new=%p(%s) esp %p underflow (%p)", new, new->t_threadinfo->ti_args, new->md_esp, new->md_kstack);
	KASSERT(new->md_esp <= ((addr_t)new->md_kstack + KERNEL_STACK_SIZE), "new=%p esp %p overflow (%p)", new, new->md_esp, new->md_kstack + KERNEL_STACK_SIZE);

	/* Activate the corresponding kernel stack in the TSS */
	struct TSS* tss = (struct TSS*)PCPU_GET(tss);
	tss->esp0 = new->md_esp0;

	/* Set debug registers */
	DR_SET(0, new->md_dr[0]);
	DR_SET(1, new->md_dr[1]);
	DR_SET(2, new->md_dr[2]);
	DR_SET(3, new->md_dr[3]);
	DR_SET(7, new->md_dr[7]);

	/* Switch to the new page table */
	__asm __volatile("movl %0, %%cr3" : : "r" (new->md_cr3));

	/*
	 * Switch to the new thread; as this will only happen in kernel -> kernel
	 * contexts and even then, between a frame, it is enough to just
	 * switch the stack and mark all registers are being clobbered by assigning
	 * them to dummy outputs.
	 *
	 * Note that we can't force the compiler to reload %ebp this way, so we'll
	 * just save it ourselves.
	 *
	 * %ebx is hardcoded to be the old thread upon entry and return (it won't be
	 * destroyed across function calls, so it's a good choice). This value is used
	 * by the caller to call scheduler_release() - ..._trampoline depends on the
	 * register allocation!
	 */
	register register_t ecx, edx, esi, edi;
	thread_t* prev;
	__asm __volatile(
		"pushfl\n"
		"pushl %%ebp\n"
		"movl %%esp,%[old_esp]\n" /* store old %esp */
		"movl %[new_esp], %%esp\n" /* write new %esp */
		"movl $1f, %[old_eip]\n" /* write next %eip for old thread */
		"pushl %[new_eip]\n" /* load next %eip for new thread */
		"ret\n"
	"1:\n" /* we are back */
		"popl %%ebp\n"
		"popfl\n"
	: /* out */
		[old_esp] "=m" (old->md_esp), [old_eip] "=m" (old->md_eip), "=b" (prev),
		/* clobbered registers */
		"=c" (ecx), "=d" (edx), "=S" (esi), "=D" (edi)
	: /* in */
		[new_eip] "a" (new->md_eip), "b" (old), [new_esp] "c" (new->md_esp)
	: /* clobber */"memory"
	);
	return prev;
}

void*
md_map_thread_memory(thread_t* thread, void* ptr, size_t length, int write)
{
	return ptr;
}

void
md_thread_set_entrypoint(thread_t* thread, addr_t entry)
{
	thread->md_arg1 = entry;
}

void
md_thread_set_argument(thread_t* thread, addr_t arg)
{
	thread->md_arg2 = arg;
}

void
md_thread_clone(thread_t* t, thread_t* parent, register_t retval)
{
	KASSERT(PCPU_GET(curthread) == parent, "must clone active thread");

	/* Restore the thread's own page directory */
	t->md_cr3 = KVTOP((addr_t)t->t_vmspace->vs_md_pagedir);

	/* Do not inherit breakpoints */
	t->md_dr[7] = DR7_LE | DR7_GE;

	/*
	 * Size of the syscall call frame; this is the following:
	 * hardware saved [*]: ss, esp, eflags, cs, eip
	 * software saved (syscall_int): ebp, ebx, esi, edi, ds, es, fs
	 * This makes 12 registers total, and 12 * 4 = 48.
	 *
	 * [*] A system call is always a ring3->0 transition, so the extra ss/esp
	 *     registers will be there.
	 */
#define SYSCALL_FRAME_SIZE 48

	/*
	 * Copy kernel stack over; it is always mapped. Note that we do not need the
	 * entire thing; we just need the part above as it is enough to return from
	 * the syscall. This is always the very first values on the stack because
	 * there is no other way to enter the system call handler; we cannot rely on
	 * the context's %esp because we may be preempted in between (and we must
	 * only restore the userland context)
	 */
	memcpy(t->md_kstack + KERNEL_STACK_SIZE - SYSCALL_FRAME_SIZE, parent->md_kstack + KERNEL_STACK_SIZE - SYSCALL_FRAME_SIZE, SYSCALL_FRAME_SIZE);

	/*
	 * Handle returning to the new thread; we don't want to call all code leading
	 * up to here, so we only need to travel down the syscall frame; clone_return()
	 * does nothing more than setting the syscall return code and returning to
	 * userland.
	 */
	t->md_esp -= SYSCALL_FRAME_SIZE;
	t->md_eip = (addr_t)&clone_return;
	t->md_arg1 = retval;
}

/* vim:set ts=2 sw=2: */
