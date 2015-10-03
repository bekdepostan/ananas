#include <ananas/types.h>
#include <ananas/device.h>
#include <ananas/error.h>
#include <ananas/bus/usb/core.h>
#include <ananas/bus/usb/pipe.h>
#include <ananas/trace.h>
#include <ananas/lib.h>
#include <ananas/mm.h>
#include "usb-device.h"
#include "usb-transfer.h"

TRACE_SETUP;

static void usbpipe_callback(struct USB_TRANSFER* xfer);

static struct USB_TRANSFER*
usbpipe_create_transfer(struct USB_PIPE* pipe)
{
	struct USB_ENDPOINT* ep = pipe->p_ep;
	struct USB_TRANSFER* xfer = usbtransfer_alloc(pipe->p_dev, ep->ep_type, (ep->ep_dir == EP_DIR_IN) ? TRANSFER_FLAG_READ : TRANSFER_FLAG_WRITE, ep->ep_address);
	xfer->xfer_length = ep->ep_maxpacketsize;
	xfer->xfer_callback = usbpipe_callback;
	xfer->xfer_callback_data = pipe;
	return xfer;
}

static void
usbpipe_callback(struct USB_TRANSFER* xfer)
{
	struct USB_PIPE* pipe = xfer->xfer_callback_data;
	kprintf("usbpipe_callback(): pipe=%p\n", pipe);
	pipe->p_callback(pipe);
}

errorcode_t
usbpipe_alloc(struct USB_DEVICE* usb_dev, int num, int type, int dir, usbpipe_callback_t callback, struct USB_PIPE** pipe)
{
	KASSERT(dir == EP_DIR_IN || dir == EP_DIR_OUT, "invalid direction %u", dir);
	KASSERT(type == TRANSFER_TYPE_CONTROL || type == TRANSFER_TYPE_INTERRUPT || type == TRANSFER_TYPE_BULK || type == TRANSFER_TYPE_ISOCHRONOUS, "invalid type %u", type);
	KASSERT(callback != NULL, "callback must be specified");
	KASSERT(usb_dev != NULL, "device must be specified");

	struct USB_INTERFACE* uif = &usb_dev->usb_interface[usb_dev->usb_cur_interface];
	if (num < 0 || num >= uif->if_num_endpoints)
		return ANANAS_ERROR(BAD_RANGE);

	struct USB_ENDPOINT* ep = &uif->if_endpoint[num];
	if (ep->ep_type != type || ep->ep_dir != dir)
		return ANANAS_ERROR(BAD_TYPE);

	struct USB_PIPE* p = kmalloc(sizeof *p);
	p->p_dev = usb_dev;
	p->p_callback = callback;
	p->p_ep = ep;
	p->p_xfer = usbpipe_create_transfer(p);

	/* Hook up the pipe to the device XXX lock? */
	DQUEUE_ADD_TAIL(&usb_dev->usb_pipes, p);
	*pipe = p;
	return ANANAS_ERROR_OK;
}

void
usbpipe_free(struct USB_PIPE** pipe)
{
	struct USB_PIPE* p = *pipe;
	struct USB_DEVICE* usb_dev = p->p_dev;

	kprintf("usbpipe_free(): pipe=%p xfer=%p\n", p, p->p_xfer);

	/* XXX lock? */
	DQUEUE_REMOVE(&usb_dev->usb_pipes, p);
	kfree(p);
}

errorcode_t
usbpipe_schedule(struct USB_PIPE* pipe)
{
	kprintf("usbpipe_schedule(): pipe=%p xfer=%p\n", pipe, pipe->p_xfer);
	return usbtransfer_schedule(pipe->p_xfer);
}

/* vim:set ts=2 sw=2: */
