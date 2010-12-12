/*
 * The reason we have this file is because it's quite a big effort to hook up
 * the real kern/device.c file - it expects files generated by config, probe
 * sequences etc. We avoid all that for the time being.
 */
#include "type-glue.h"
#include <fcntl.h>
#include <unistd.h>
#include <ananas/lib.h>
#include <ananas/error.h>
#include <ananas/bio.h>

static int dev_fd = -1;
static off_t dev_len;

static struct DEVICE drv_image = {
	.name = "image"
};

static struct DEVICE_QUEUE device_queue;

void
device_init()
{
	dev_fd = open("/home/rink/ananas.disk.img", O_RDONLY);
	if (dev_fd < 0)
		panic("cannot open disk image");
	dev_len = lseek(dev_fd, 0, SEEK_END);
	lseek(dev_fd, 0, SEEK_SET);

	/* Fake a device queue with only a single device */
	DQUEUE_INIT(&device_queue);
	DQUEUE_ADD_TAIL(&device_queue, &drv_image);
}

struct DEVICE*
device_find(const char* name)
{
	return &drv_image;
}

errorcode_t
device_bread(device_t dev, struct BIO* bio)
{
	off_t off = bio->io_block * 512;
	/* This is a very ugly hack to use the correct partition */
	off += 63 * 512;
	if (pread(dev_fd, bio->data, bio->length, off) != bio->length)
		panic("read error");
	bio->flags &= ~BIO_FLAG_DIRTY;
	return ANANAS_ERROR_OK;
}

errorcode_t
device_read(device_t dev, char* buf, size_t* len, off_t offset)
{
	/* Used only for device filesystem, which we won't read anyway */
	panic("device_read() is not implemented");
	return ANANAS_ERROR(BAD_SYSCALL);
}

struct DEVICE_QUEUE*
device_get_queue()
{
	return &device_queue;
}

/* vim:set ts=2 sw=2: */