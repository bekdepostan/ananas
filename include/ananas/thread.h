#include <ananas/types.h>
#include <ananas/dqueue.h>
#include <ananas/vfs.h>
#include <ananas/limits.h>
#include <ananas/handle.h>
#include <ananas/schedule.h>
#include <machine/thread.h>

#ifndef __THREAD_H__
#define __THREAD_H__

typedef struct THREAD* thread_t;
typedef void (*kthread_func_t)(void*);
struct THREADINFO;
struct VFS_INODE;
struct THREAD_MAPPING;

#define THREAD_EVENT_EXIT 1
/* Fault function: handles a page fault for thread t, mapping tm and address virt */
typedef errorcode_t (*threadmap_fault_t)(thread_t t, struct THREAD_MAPPING* tm, addr_t virt);

/* Clone function: copies mapping tsrc to tdest for thread t */
typedef errorcode_t (*threadmap_clone_t)(thread_t t, struct THREAD_MAPPING* tdest, struct THREAD_MAPPING* tsrc);

/* Destroy function: cleans up the given mapping's private data */
typedef errorcode_t (*threadmap_destroy_t)(thread_t t, struct THREAD_MAPPING* tm);

struct THREAD_MAPPING {
	unsigned int		tm_flags;		/* flags */
	addr_t			tm_virt;		/* userland address */
	size_t			tm_len;			/* length */
	addr_t			tm_phys;		/* physical address */
	void*			tm_privdata;		/* private data */
	threadmap_fault_t	tm_fault;		/* fault function */
	threadmap_clone_t	tm_clone;		/* clone function */
	threadmap_destroy_t	tm_destroy;		/* destroy function */

	DQUEUE_FIELDS(struct THREAD_MAPPING);
};

DQUEUE_DEFINE(THREAD_MAPPING_QUEUE, struct THREAD_MAPPING);

struct THREAD {
	/* Machine-dependant data - must be first */
	MD_THREAD_FIELDS

	struct SPINLOCK spl_thread;	/* Lock protecting the thread data */

	unsigned int flags;
#define THREAD_FLAG_ACTIVE	0x0001	/* Thread is scheduled somewhere */
#define THREAD_FLAG_SUSPENDED	0x0002	/* Thread is currently suspended */
#define THREAD_FLAG_TERMINATING	0x0004	/* Thread is terminating */
#define THREAD_FLAG_ZOMBIE	0x0008	/* Thread has no more resources */

	unsigned int terminate_info;
#define THREAD_MAKE_EXITCODE(a,b) (((a) << 24) | ((b) & 0x00ffffff))
#define THREAD_TERM_SYSCALL	0x1	/* euthanasia */
#define THREAD_TERM_FAULT	0x2	/* programming fault */
#define THREAD_TERM_FAILURE	0x3	/* generic failure */

	addr_t next_mapping;		/* address of next mapping */
	struct THREAD_MAPPING_QUEUE mappings;

	struct THREADINFO* threadinfo;	/* Thread startup information */

	/* Thread handles */
	struct HANDLE* thread_handle;	/* Handle identifying this thread */
	struct HANDLE_QUEUE handles;	/* Handles owned by the thread */
	struct HANDLE* path_handle;	/* Current path */

	/* Scheduler specific information */
	struct SCHED_PRIV sched_priv;

	DQUEUE_FIELDS(struct THREAD);
};

DQUEUE_DEFINE(THREAD_QUEUE, struct THREAD);

/* Machine-dependant callback to initialize a thread */
errorcode_t md_thread_init(thread_t thread);

/* Machine-dependant callback to free thread data */
void md_thread_free(thread_t thread);

/* Machine-dependant kernel thread activation */
void md_thread_setkthread(thread_t thread, kthread_func_t kfunc, void* arg);

errorcode_t thread_init(thread_t t, thread_t parent);
errorcode_t thread_alloc(thread_t parent, thread_t* dest);
void thread_free(thread_t);
void thread_destroy(thread_t);
errorcode_t thread_set_args(thread_t t, const char* args, size_t args_len);
errorcode_t thread_set_environment(thread_t t, const char* env, size_t env_len);

void md_thread_switch(thread_t new, thread_t old);
void md_idle_thread();

/* Thread memory map flags */
#define THREAD_MAP_READ 	0x01	/* Read */
#define THREAD_MAP_WRITE	0x02	/* Write */
#define THREAD_MAP_EXECUTE	0x04	/* Execute */
#define THREAD_MAP_LAZY		0x08	/* Lazy mapping: page in as needed */
#define THREAD_MAP_ALLOC 	0x10	/* Allocate memory for mapping */
void md_thread_set_entrypoint(thread_t thread, addr_t entry);
void md_thread_set_argument(thread_t thread, addr_t arg);
void* md_thread_map(thread_t thread, void* to, void* from, size_t length, int flags);
errorcode_t thread_unmap(thread_t t, addr_t virt, size_t len);
void* md_map_thread_memory(thread_t thread, void* ptr, size_t length, int write);
void md_thread_clone(struct THREAD* t, struct THREAD* parent, register_t retval);
errorcode_t md_thread_unmap(thread_t thread, addr_t virt, size_t length);

errorcode_t thread_mapto(thread_t t, addr_t virt, addr_t phys, size_t len, uint32_t flags, struct THREAD_MAPPING** out);
errorcode_t thread_map(thread_t t, addr_t from, size_t len, uint32_t flags, struct THREAD_MAPPING** out);
void thread_free_mapping(thread_t t, struct THREAD_MAPPING* tm);
void thread_free_mappings(thread_t t);
errorcode_t thread_handle_fault(thread_t t, addr_t virt, int flags);

void thread_suspend(thread_t t);
void thread_resume(thread_t t);
void thread_exit(int exitcode);
void thread_dump(int num_args, char** arg);
errorcode_t thread_clone(struct THREAD* parent, int flags, struct THREAD** dest);

#endif
