#ifndef __ANANAS_VFS_TYPES_H__
#define __ANANAS_VFS_TYPES_H__

#include <ananas/stat.h> /* for 'struct stat' */
#include <ananas/vfs/dentry.h> /* for 'struct DENTRY_CACHE_QUEUE' */
#include <ananas/vfs/icache.h> /* for 'struct ICACHE_QUEUE' */

struct DEVICE;
struct VFS_MOUNTED_FS;
struct VFS_INODE_OPS;
struct VFS_FILESYSTEM_OPS;

#define INODE_LOCK(i) \
	spinlock_lock(&(i)->i_spinlock)
#define INODE_UNLOCK(i) \
	spinlock_unlock(&(i)->i_spinlock)

/*
 * VFS_INODE refers to an inode; it must be locked before any fields can
 * be updated. The refcount protects the inode from disappearing while
 * it is still being used.
 */
struct VFS_INODE {
	struct SPINLOCK	i_spinlock;		/* Spinlock protecting inode */
	refcount_t	i_refcount;		/* Refcount, must be >=1 */
	unsigned int	i_flags;		/* Inode flags */
#define INODE_FLAG_GONE		(1 << 0)	/* Inode is gone */
#define INODE_FLAG_DIRTY	(1 << 1)	/* Needs to be written */
	struct stat 	i_sb;			/* Inode information */
	struct VFS_INODE_OPS* i_iops;		/* Inode operations */

	struct VFS_MOUNTED_FS* i_fs;		/* Filesystem where the inode lives */
	void*		i_privdata;		/* Filesystem-specific data */
	uint8_t		i_fsop[1];		/* File system object pointer */
};

/*
 * VFS_FILE refers to an opened file. It will have an associated
 * inode and a position.
 */
struct VFS_FILE {
	off_t			f_offset;
	/*
	 * An opened file can have an inode or device as backend.
	 */
	struct VFS_INODE*	f_inode;
	struct DEVICE*		f_device;
};

/*
 * Directory entry, as returned by the kernel. The fsop length is stored
 * for easy iteration on the caller side.
 */
struct VFS_DIRENT {
	uint32_t	de_flags;		/* Flags */
	uint8_t		de_fsop_length;		/* Length of identifier */
	uint8_t		de_name_length;		/* Length of name */
	char		de_fsop[1];		/* Identifier */
	/*
	 * de_name will be stored directly after the fsop.
	 */
};
#define DE_LENGTH(x) (sizeof(struct VFS_DIRENT) + (x)->de_fsop_length + (x)->de_name_length)
#define DE_FSOP(x) (&((x)->de_fsop))
#define DE_NAME(x) (&((x)->de_fsop[(x)->de_fsop_length]))

/*
 * VFS_MOUNTED_FS is used to refer to a mounted filesystem. Note that
 * we will also use it during the filesystem-operations mount() call.
 *
 * Fields marked with (R) are readonly and must never be changed after
 * mounting.
 */
struct VFS_MOUNTED_FS {
	struct SPINLOCK	fs_spinlock;		/* Protects fields marked with (F) */
	device_t	fs_device;		/* (F) Device where the filesystem lives */
	unsigned int	fs_flags;		/* (F) Filesystem flags */
#define VFS_FLAG_INUSE    0x0001		/* Filesystem entry is in use */
#define VFS_FLAG_READONLY 0x0002		/* Filesystem is readonly */
	const char*	fs_mountpoint;		/* (R) Mount point */
	uint32_t	fs_block_size;		/* (R) Block size */
	uint8_t		fs_fsop_size;		/* (R) FSOP identifier length */
	void*		fs_privdata;		/* (R) Private filesystem data */

	/* Inode cache */
	struct SPINLOCK		fs_icache_lock;		/* Protects fields marked with (I) */
	struct ICACHE_QUEUE	fs_icache_inuse;	/* (I) Currently used inodes */
	struct ICACHE_QUEUE	fs_icache_free;		/* (I) Available inode list */
	void*			fs_icache_buffer;	/* (I) Inode cache buffer, for cleanup */

	/* Dentry cache */
	struct SPINLOCK			fs_dcache_lock;		/* Protects fields marked with (D) */
	struct DENTRY_CACHE_QUEUE	fs_dcache_inuse;	/* (D) Currently used items */
	struct DENTRY_CACHE_QUEUE	fs_dcache_free;		/* (D) Currently used items */
	void*				fs_dcache_buffer;	/* (D) Dentry cache buffer, for cleanup */


	struct VFS_FILESYSTEM_OPS* fs_fsops;		/* (R) Filesystem operations */
	struct VFS_INODE* fs_root_inode;		/* (R) Filesystem's root inode */
};

/*
 * VFS_FILESYSTEM_OPS are operations corresponding to the highest level:
 * mount/unmount a filesystem and obtaining statistics.
 */
struct VFS_FILESYSTEM_OPS {
	/*
	 * Mount a filesystem. device, mountpoint and fsops in 'fs' are
	 * guaranteed to be filled out.
	 */
	errorcode_t (*mount)(struct VFS_MOUNTED_FS* fs);

	/*
	 * Allocate an inode. The purpose for this function is to initialize
	 * the 'privdata' field of the inode - the function should call
	 * vfs_make_inode() to obtain the new, locked inode.
	 */
	struct VFS_INODE* (*alloc_inode)(struct VFS_MOUNTED_FS* fs, const void* fsop);

	/*
	 * Destroy a locked inode. The purpose for this function is to deinitialize
	 * the 'privdata' field of the inode. The function should end by
	 * calling vfs_destroy_inode() to remove the inode itself.
	 */
	void (*destroy_inode)(struct VFS_INODE* inode);

	/*
	 * Read an inode from disk; inode is locked and pre-allocated using
	 * alloc_inode().  The 'fs' field of the inode is guaranteed to be
	 * filled out.
	 */
	errorcode_t (*read_inode)(struct VFS_INODE* inode, void* fsop);

	/*
	 * Writes an inode back to disk; inode is locked.
	 */
	errorcode_t (*write_inode)(struct VFS_INODE* inode);
};

struct VFS_INODE_OPS {
	/*
	 * Reads directory entries. Must set length to amount of data filled on
	 * success.
	 */
	errorcode_t (*readdir)(struct VFS_FILE* file, void* buf, size_t* length);

	/*
	 * Looks up an entry within a directory, updates 'destinode' on success.
	 */
	errorcode_t (*lookup)(struct VFS_INODE* dirinode, struct VFS_INODE** destinode, const char* dentry);

	/*
	 * Maps the inode's given block number to a block device's block
	 * number. A new block is to be allocated if create is non-zero.
	 */
	errorcode_t (*block_map)(struct VFS_INODE* inode, blocknr_t block_in, blocknr_t* block_out, int create);

	/*
	 * Reads inode data to a buffer, up to len bytes. Must update len on success
	 * with the amount of data read.
	 */
	errorcode_t (*read)(struct VFS_FILE* file, void* buf, size_t* len);

	/*
	 * Writes inode data from a buffer, up to len bytes. Must update len on success
	 * wit hthe amount of data written.
	 */
	errorcode_t (*write)(struct VFS_FILE* file, const void* buf, size_t* len);

	/*
	 * Creates a new entry in the directory. On success, calls
	 * dentry_set_inode() to fill out the entry's inode.
	 */
	errorcode_t (*create)(struct VFS_INODE* dir, struct DENTRY_CACHE_ITEM* de, int mode);
};

#endif /* __ANANAS_VFS_TYPES_H__ */