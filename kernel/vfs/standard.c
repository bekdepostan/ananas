#include <ananas/types.h>
#include <ananas/bio.h>
#include <ananas/device.h>
#include <ananas/error.h>
#include <ananas/lib.h>
#include <ananas/mm.h>
#include <ananas/schedule.h>
#include <ananas/trace.h>
#include <ananas/vfs.h>
#include <ananas/vfs/generic.h>
#include <ananas/vfs/mount.h>
#include "options.h"

TRACE_SETUP;

errorcode_t
vfs_open(const char* fname, struct VFS_INODE* cwd, struct VFS_FILE* file)
{
	struct VFS_INODE* inode;
	errorcode_t err = vfs_lookup(cwd, &inode, fname);
	ANANAS_ERROR_RETURN(err);

	memset(file, 0, sizeof(struct VFS_FILE));
	file->f_inode = inode;
	file->f_offset = 0;
	return ANANAS_ERROR_OK;
}

errorcode_t
vfs_close(struct VFS_FILE* file)
{
	if(file->f_inode != NULL)
		vfs_deref_inode(file->f_inode);
	file->f_inode = NULL; file->f_device = NULL;
	return ANANAS_ERROR_OK;
}

errorcode_t
vfs_read(struct VFS_FILE* file, void* buf, size_t* len)
{
	KASSERT(file->f_inode != NULL || file->f_device != NULL, "vfs_read on nonbacked file");
	if (file->f_device != NULL) {
		/* Device */
		if (file->f_device->driver == NULL || file->f_device->driver->drv_read == NULL)
			return ANANAS_ERROR(BAD_OPERATION);
		else {
			return file->f_device->driver->drv_read(file->f_device, buf, len, 0);
		}
	}

	if (file->f_inode == NULL || file->f_inode->i_iops == NULL)
		return ANANAS_ERROR(BAD_OPERATION);

	if (!S_ISDIR(file->f_inode->i_sb.st_mode)) {
		/* Regular file */
		if (file->f_inode->i_iops->read == NULL)
			return ANANAS_ERROR(BAD_OPERATION);
		return file->f_inode->i_iops->read(file, buf, len);
	}

	/* Directory */
	if (file->f_inode->i_iops->readdir == NULL)
		return ANANAS_ERROR(BAD_OPERATION);
	return file->f_inode->i_iops->readdir(file, buf, len);
}

errorcode_t
vfs_write(struct VFS_FILE* file, const void* buf, size_t* len)
{
	KASSERT(file->f_inode != NULL || file->f_device != NULL, "vfs_write on nonbacked file");
	if (file->f_device != NULL) {
		/* Device */
		if (file->f_device->driver == NULL || file->f_device->driver->drv_write == NULL)
			return ANANAS_ERROR(BAD_OPERATION);
		else
			return file->f_device->driver->drv_write(file->f_device, buf, len, 0);
	}

	if (file->f_inode == NULL || file->f_inode->i_iops == NULL)
		return ANANAS_ERROR(BAD_OPERATION);

	if (S_ISDIR(file->f_inode->i_sb.st_mode)) {
		/* Directory */
		return ANANAS_ERROR(BAD_OPERATION);
	}

	/* Regular file */
	if (file->f_inode->i_iops->write == NULL)
		return ANANAS_ERROR(BAD_OPERATION);
	return file->f_inode->i_iops->write(file, buf, len);
}

errorcode_t
vfs_seek(struct VFS_FILE* file, off_t offset)
{
	if (file->f_inode == NULL)
		return ANANAS_ERROR(BAD_HANDLE);
	if (offset > file->f_inode->i_sb.st_size)
		return ANANAS_ERROR(BAD_RANGE);
	file->f_offset = offset;
	return ANANAS_ERROR_OK;
}

/*
 * Called to perform a lookup from directory entry 'dentry' to an inode;
 * 'curinode' is the initial inode to start the lookup relative to, or NULL to
 * start from the root.
 */
errorcode_t
vfs_lookup(struct VFS_INODE* curinode, struct VFS_INODE** destinode, const char* dentry)
{
	char tmp[VFS_MAX_NAME_LEN + 1];

	/*
	 * First of all, see if we need to lookup relative to the root; if so,
	 * we must update the current inode.
	 */
	if (curinode == NULL || *dentry == '/') {
		struct VFS_MOUNTED_FS* fs = vfs_get_rootfs();
		if (fs == NULL)
			/* If there is no root filesystem, nothing to do */
			return ANANAS_ERROR(NO_FILE);
		if (*dentry == '/')
			dentry++;

		/* Start by looking up the root inode */
		curinode = fs->fs_root_inode;
		KASSERT(curinode != NULL, "no root inode");
	}

	/* Bail if there is no current inode; this makes the caller's path easier */
	if (curinode == NULL)
		return ANANAS_ERROR(NO_FILE);

	/*
	 * Explicitely reference the inode; this is normally done by the VFS lookup
	 * function when it returns an inode, but we need some place to start. The
	 * added benefit is that we won't need any exceptions, as we can just free
	 * any inode that doesn't work.
	 */
	vfs_ref_inode(curinode);

	const char* curdentry = dentry;
	const char* curlookup;
	while (curdentry != NULL && *curdentry != '\0' /* for trailing /-es */) {
		/*
		 * Isolate the next part of the part we have to look up. Note that
		 * we consider the input dentry as const, so we can't mess with it;
		 * this is why we need to make copies in 'tmp'.
		 */
		char* ptr = strchr(curdentry, '/');
		if (ptr != NULL) {
			/* There's a slash in the path - must lookup next part */
			strncpy(tmp, curdentry, ptr - curdentry);
			curdentry = ++ptr;
			curlookup = tmp;
		} else {
			curlookup = curdentry;
			curdentry = NULL;
		}

		/*
		 * If the entry to find is '.', continue to the next one; we are already
		 * there.
		 */
		if (strcmp(curlookup, ".") == 0)
			continue;

		/*
		 * We need to recurse; this can only be done if this is a directory, so
		 * refuse if this isn't the case.
		 */
		if (S_ISDIR(curinode->i_sb.st_mode) == 0) {
			vfs_deref_inode(curinode);
			return ANANAS_ERROR(NO_FILE);
		}

		/*
		 * See if the item is in the cache; we will add it otherwise since we we
		 * use the cache to look for items.
		 */
		struct DENTRY_CACHE_ITEM* dentry;
		while(1) {
			dentry = dcache_find_item_or_add_pending(curinode, curlookup);
			if (dentry != NULL)
				break;
			TRACE(VFS, WARN, "dentry item is already pending, waiting...");
			/* XXX There should be a wakeup signal of some kind */
			reschedule();
		}

		if (dentry->d_flags & DENTRY_FLAG_NEGATIVE) {
			/* Entry is in the cache but cannot be found; release the previous inode */
			KASSERT(dentry->d_entry_inode == NULL, "negative lookup with inode?");
			vfs_deref_inode(curinode);
			return ANANAS_ERROR(NO_FILE);
		}

		if (dentry->d_entry_inode != NULL) {
			/* Already have the inode cached; release the previous inode */
			vfs_deref_inode(curinode);
			/* And use the entry from the cache for the next lookup (refcount was incremented already) */
			curinode = dentry->d_entry_inode;
			continue;
		}

		/*
	 	 * Attempt to look up whatever entry we need. Once this is done, we can
		 * get rid of the current lookup inode (as it won't be the inode itself
		 * since we special-case "." above).
		 */
		struct VFS_INODE* inode;
		errorcode_t err = curinode->i_iops->lookup(curinode, &inode, curlookup);
		vfs_deref_inode(curinode);
		if (err == ANANAS_ERROR_NONE) {
			/*
			 * Lookup worked; we have a single-reffed inode now. We have to hook it
			 * up to the dentry cache, which means it needs an extra ref. The other
			 * ref is for the caller (or us, we'll deref it ourselves if this is just
			 * part of another lookup directory part)
			 */
			vfs_ref_inode(inode);
			dentry->d_entry_inode = inode;
		} else {
			/* Lookup failed; make the entry cache negative */
			dentry->d_flags |= DENTRY_FLAG_NEGATIVE;
			return err;
		}
		
		curinode = inode;
	}
	*destinode = curinode;
	return ANANAS_ERROR_OK;
}


/* vim:set ts=2 sw=2: */
