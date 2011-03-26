#ifndef __ANANAS_VFS_CORE_H__
#define __ANANAS_VFS_CORE_H__

#include <ananas/types.h>
#include <ananas/vfs/types.h>

#define VFS_MAX_NAME_LEN 255

struct VFS_MOUNTED_FS;
struct VFS_INODE_OPS;
struct VFS_FILESYSTEM_OPS;

void vfs_init();
errorcode_t vfs_mount(const char* from, const char* to, const char* type, void* options);

/* Low-level interface - designed for filesystem use (must not be used otherwise) */

/*
 * Creates a new, empty inode for the given filesystem. The corresponding inode is
 * locked, has a refcount of one and must be filled by the filesystem before
 * unlocking.
 */
struct VFS_INODE* vfs_make_inode(struct VFS_MOUNTED_FS* fs, const void* fsop);

/*
 * Destroys a locked inode; this should be called by the filesystem's
 * 'destroy_inode' function.
 */
void vfs_destroy_inode(struct VFS_INODE* inode);

/* Marks an inode as dirty; will trigger the filesystem's 'write_inode' function */
void vfs_set_inode_dirty(struct VFS_INODE* inode);

/* Internal interface only */
void vfs_deref_inode_locked(struct VFS_INODE* inode);
void vfs_dump_inode(struct VFS_INODE* inode);

/* Low-level interface */
/*
 * Obtains an inode for a given FSOP. The destination inode will have a reference count of
 * at least 2 (one for the caller, and one for the cache).
 */
errorcode_t vfs_get_inode(struct VFS_MOUNTED_FS* fs, void* fsop, struct VFS_INODE** destinode);

/*
 * Removes an inode reference; cleans up the inode if the refcount is zero.
 */
void vfs_deref_inode(struct VFS_INODE* inode);

/*
 * Explicitely add a reference to a given inode.
 */
void vfs_ref_inode(struct VFS_INODE* inode);

/*
 * Retrieves a given block for the given filesystem to a bio.
 */
errorcode_t vfs_bget(struct VFS_MOUNTED_FS* fs, blocknr_t block, struct BIO** bio, int flags);

/*
 * Reads a block for the given filesystem to bio.
 */
static inline errorcode_t vfs_bread(struct VFS_MOUNTED_FS* fs, blocknr_t block, struct BIO** bio)
{
	return vfs_bget(fs, block, bio, 0);
}

errorcode_t vfs_lookup(struct VFS_INODE* cwd, struct VFS_INODE** destinode, const char* dentry);

/* Higher-level interface */
errorcode_t vfs_open(const char* fname, struct VFS_INODE* cwd, struct VFS_FILE* file);
errorcode_t vfs_close(struct VFS_FILE* file);
errorcode_t vfs_read(struct VFS_FILE* file, void* buf, size_t* len);
errorcode_t vfs_write(struct VFS_FILE* file, const void* buf, size_t* len);
errorcode_t vfs_seek(struct VFS_FILE* file, off_t offset);
errorcode_t vfs_create(struct VFS_INODE* dirinode, struct VFS_FILE* destfile, const char* dentry, int mode);

/* Filesystem specific functions */
size_t vfs_filldirent(void** dirents, size_t* size, const void* fsop, int fsoplen, const char* name, int namelen);

#endif /* __ANANAS_VFS_CORE_H__ */