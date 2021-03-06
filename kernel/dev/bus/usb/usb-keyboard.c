#include <ananas/types.h>
#include <ananas/device.h>
#include <ananas/error.h>
#include <ananas/bus/usb/config.h>
#include <ananas/bus/usb/core.h>
#include <ananas/bus/usb/pipe.h>
#include <ananas/dqueue.h>
#include <ananas/lib.h>
#include <ananas/thread.h>
#include <ananas/pcpu.h>
#include <ananas/schedule.h>
#include <ananas/trace.h>
#include <ananas/mm.h>
#include "usb-device.h"

TRACE_SETUP;

static errorcode_t
usbkbd_probe(device_t dev)
{
	struct USB_DEVICE* usb_dev = device_alloc_resource(dev, RESTYPE_USB_DEVICE, 0);
	if (usb_dev == NULL)
		return ANANAS_ERROR(NO_DEVICE);

	/* XXX This is crude */
	struct USB_INTERFACE* iface = &usb_dev->usb_interface[usb_dev->usb_cur_interface];
	if (iface->if_class != USB_IF_CLASS_HID || iface->if_protocol != 1 /* keyboard */)
		return ANANAS_ERROR(NO_DEVICE);

	return ANANAS_ERROR_OK;
}

static void
usbkbd_callback(struct USB_PIPE* pipe)
{
	struct USB_TRANSFER* xfer = pipe->p_xfer;

	kprintf("usbkbd_callback! -> [");

	if (xfer->xfer_flags & TRANSFER_FLAG_ERROR) {
		kprintf("error, aborting]\n");
		return;
	}

	for (int i = 0; i < xfer->xfer_result_length; i++) {
		kprintf("%x ", xfer->xfer_data[i]);
	}
	kprintf("]\n");

	/* Reschedule the pipe for future updates */
	usbpipe_schedule(pipe);
}

static errorcode_t
usbkbd_attach(device_t dev)
{
	struct USB_DEVICE* usb_dev = device_alloc_resource(dev, RESTYPE_USB_DEVICE, 0);

	/*
	 * OK; there's a keyboard here we want to attach. There must be an
	 * interrupt IN endpoint; this is where we get our data from.
	 */
	struct USB_PIPE* pipe;
	errorcode_t err = usbpipe_alloc(usb_dev, 0, TRANSFER_TYPE_INTERRUPT, EP_DIR_IN, 0, usbkbd_callback, &pipe);
	if (err != ANANAS_ERROR_OK) {
		device_printf(dev, "endpoint 0 not interrupt/in");
		return ANANAS_ERROR(NO_RESOURCE);
	}
	return usbpipe_schedule(pipe);
}

struct DRIVER drv_usbkeyboard = {
	.name = "usbkeyboard",
	.drv_probe = usbkbd_probe,
	.drv_attach = usbkbd_attach
};

DRIVER_PROBE(usbkeyboard)
DRIVER_PROBE_BUS(usbbus)
DRIVER_PROBE_END()

/* vim:set ts=2 sw=2: */
