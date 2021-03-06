#include <ananas/types.h>
#include <ananas/bio.h>
#include <ananas/device.h>
#include <ananas/error.h>
#include <ananas/lib.h>
#include <ananas/trace.h>
#include <ananas/vfs.h>
#include <ananas/vfs/generic.h>

TRACE_SETUP;

#define VFS_DEBUG_LOOKUP 0

errorcode_t
vfs_generic_lookup(struct DENTRY* parent, struct VFS_INODE** destinode, const char* dentry)
{
	KASSERT(parent != NULL, "lookup with no parent?");
	char buf[1024]; /* XXX */

#if VFS_DEBUG_LOOKUP
	kprintf("vfs_generic_lookup(); parent=%p dentry='%s'\n", parent, dentry);
#endif

	/*
	 * XXX This is a very naive implementation which does not use the
	 * possible directory index.
	 */
	struct VFS_INODE* parent_inode = parent->d_inode;
	KASSERT(S_ISDIR(parent_inode->i_sb.st_mode), "supplied inode is not a directory");

	/* Rewind the directory back; we'll be traversing it from front to back */
	struct VFS_FILE dirf;
	memset(&dirf, 0, sizeof(dirf));
	dirf.f_offset = 0;
	dirf.f_dentry = parent;
	while (1) {
		size_t buf_len = sizeof(buf);
		int result = vfs_read(&dirf, buf, &buf_len);
		if (result != ANANAS_ERROR_NONE)
			return result;
		if (buf_len == 0)
			return ANANAS_ERROR(NO_FILE);

		char* cur_ptr = buf;
		while (buf_len > 0) {
			struct VFS_DIRENT* de = (struct VFS_DIRENT*)cur_ptr;
			buf_len -= DE_LENGTH(de); cur_ptr += DE_LENGTH(de);

#ifdef DEBUG_VFS_LOOKUP
			kprintf("vfs_generic_lookup('%s'): comparing with '%s'\n", dentry, de->de_fsop + de->de_fsop_length);
#endif
	
			if (strcmp(de->de_fsop + de->de_fsop_length, dentry) != 0)
				continue;

			/* Found it! */
			return vfs_get_inode(parent_inode->i_fs, de->de_fsop, destinode);
		}
	}
}

errorcode_t
vfs_generic_read(struct VFS_FILE* file, void* buf, size_t* len)
{
	struct VFS_INODE* inode = file->f_dentry->d_inode;
	struct VFS_MOUNTED_FS* fs = inode->i_fs;
	size_t read = 0;
	size_t left = *len;
	struct BIO* bio = NULL;

	KASSERT(inode->i_iops->block_map != NULL, "called without block_map implementation");

	/* Adjust left so that we don't attempt to read beyond the end of the file */
	if ((inode->i_sb.st_size - file->f_offset) < left) {
		left = inode->i_sb.st_size - file->f_offset;
	}

	blocknr_t cur_block = 0;
	while(left > 0) {
		/* Figure out which block to use next */
		blocknr_t want_block;
		errorcode_t err = inode->i_iops->block_map(inode, (file->f_offset / (blocknr_t)fs->fs_block_size), &want_block, 0);
		ANANAS_ERROR_RETURN(err);

		/* Grab the next block if necessary */
		if (cur_block != want_block || bio == NULL) {
			if (bio != NULL) bio_free(bio);
			err = vfs_bread(fs, want_block, &bio);
			ANANAS_ERROR_RETURN(err);
			cur_block = want_block;
		}

		/* Copy as much from the current entry as we can */
		off_t cur_offset = file->f_offset % (blocknr_t)fs->fs_block_size;
		int chunk_len = fs->fs_block_size - cur_offset;
		if (chunk_len > left)
			chunk_len = left;
		KASSERT(chunk_len > 0, "attempt to handle empty chunk");
		memcpy(buf, (void*)(BIO_DATA(bio) + cur_offset), chunk_len);

		read += chunk_len;
		buf += chunk_len;
		left -= chunk_len;
		file->f_offset += chunk_len;
	}
	if (bio != NULL) bio_free(bio);
	*len = read;
	return ANANAS_ERROR_OK;
}

errorcode_t
vfs_generic_write(struct VFS_FILE* file, const void* buf, size_t* len)
{
	struct VFS_INODE* inode = file->f_dentry->d_inode;
	struct VFS_MOUNTED_FS* fs = inode->i_fs;
	size_t written = 0;
	size_t left = *len;
	struct BIO* bio = NULL;

	KASSERT(inode->i_iops->block_map != NULL, "called without block_map implementation");

	int inode_dirty = 0;
	blocknr_t cur_block = 0;
	while(left > 0) {
		int create = 0;
		blocknr_t logical_block = file->f_offset / (blocknr_t)fs->fs_block_size;
		if (logical_block >= inode->i_sb.st_blocks /* XXX is this correct with sparse files? */)
			create++;

		/* Figure out which block to use next */
		blocknr_t want_block;
		errorcode_t err = inode->i_iops->block_map(inode, logical_block, &want_block, create);
		ANANAS_ERROR_RETURN(err);

		/* Calculate how much we have to put in the block */
		off_t cur_offset = file->f_offset % (blocknr_t)fs->fs_block_size;
		int chunk_len = fs->fs_block_size - cur_offset;
		if (chunk_len > left)
			chunk_len = left;

		/* Grab the next block if necessary */
		if (cur_block != want_block || bio == NULL) {
			if (bio != NULL) bio_free(bio);
			/* Only read the block if it's a new one or we're not replacing everything */
			err = vfs_bget(fs, want_block, &bio, (create || chunk_len == fs->fs_block_size) ? BIO_READ_NODATA : 0);
			ANANAS_ERROR_RETURN(err);
			cur_block = want_block;
		}

		/* Copy as much to the block as we can */
		KASSERT(chunk_len > 0, "attempt to handle empty chunk");
		memcpy((void*)(BIO_DATA(bio) + cur_offset), buf, chunk_len);
		bio_set_dirty(bio);

		/* Update the offsets and sizes */
		written += chunk_len;
		buf += chunk_len;
		left -= chunk_len;
		file->f_offset += chunk_len;

		/*
		 * If we had to create a new block or we'd have to write beyond the current
		 * inode's size, enlarge the inode and mark it as dirty.
		 */
		if (create || file->f_offset > inode->i_sb.st_size) {
			inode->i_sb.st_size = file->f_offset;
			inode_dirty++;
		}
	}
	if (bio != NULL) bio_free(bio);
	*len = written;

	if (inode_dirty)
		vfs_set_inode_dirty(inode);
	return ANANAS_ERROR_OK;
}

/* vim:set ts=2 sw=2: */
