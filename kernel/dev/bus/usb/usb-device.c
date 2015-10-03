#include <ananas/types.h>
#include <ananas/device.h>
#include <ananas/error.h>
#include <ananas/bus/usb/config.h>
#include <ananas/bus/usb/core.h>
#include <ananas/bus/usb/transfer.h>
#include <ananas/dqueue.h>
#include <ananas/lib.h>
#include <ananas/thread.h>
#include <ananas/pcpu.h>
#include <ananas/schedule.h>
#include <ananas/trace.h>
#include <ananas/mm.h>
#include <machine/param.h> /* for PAGE_SIZE XXX */
#include "usb-bus.h"
#include "usb-device.h"

TRACE_SETUP;

extern struct DEVICE_PROBE probe_queue; /* XXX gross */

struct USB_DEVICE*
usb_alloc_device(struct USB_BUS* bus, struct USB_HUB* hub, int flags)
{
	void* hcd_privdata = bus->bus_hcd->driver->drv_usb_hcd_initprivdata(flags);

	struct USB_DEVICE* usb_dev = kmalloc(sizeof *usb_dev);
	memset(usb_dev, 0, sizeof *usb_dev);
	usb_dev->usb_bus = bus;
	usb_dev->usb_hub = hub;
	usb_dev->usb_device = device_alloc(bus->bus_dev, NULL);
	usb_dev->usb_hcd_privdata = hcd_privdata;
	usb_dev->usb_address = 0;
	usb_dev->usb_max_packet_sz0 = USB_DEVICE_DEFAULT_MAX_PACKET_SZ0;
	usb_dev->usb_num_interfaces = 0;
	usb_dev->usb_cur_interface = -1;
	device_add_resource(usb_dev->usb_device, RESTYPE_USB_DEVICE, (resource_base_t)usb_dev, 0);

	DQUEUE_INIT(&usb_dev->usb_pipes);
	return usb_dev;
}

void
usb_free_device(struct USB_DEVICE* usb_dev)
{
	device_free(usb_dev->usb_device);
	kfree(usb_dev);
}

/*
 * Attaches a single USB device (which hasn't got an address or anything yet)
 *
 * Should _only_ be called by the usb-bus thread!
 */
errorcode_t
usbdev_attach(struct USB_DEVICE* usb_dev)
{
	struct DEVICE* dev = usb_dev->usb_device;
	char tmp[1024]; /* XXX */
	size_t len;
	errorcode_t err;

	/*
	 * First of all, obtain the first 8 bytes of the device descriptor; this
	 * tells us how how large the control endpoint requests can be.
	 */
	struct USB_DESCR_DEVICE* d = &usb_dev->usb_descr_device;
	len = 8;
	err = usb_control_xfer(usb_dev, USB_CONTROL_REQUEST_GET_DESC, USB_CONTROL_RECIPIENT_DEVICE, USB_CONTROL_TYPE_STANDARD, USB_REQUEST_MAKE(USB_DESCR_TYPE_DEVICE, 0), 0, d, &len, 0);
	ANANAS_ERROR_RETURN(err);

	TRACE_DEV(USB, INFO, dev,
	 "got partial device descriptor: len=%u, type=%u, version=%u, class=%u, subclass=%u, protocol=%u, maxsize=%u",
		d->dev_length, d->dev_type, d->dev_version, d->dev_class,
		d->dev_subclass, d->dev_protocol, d->dev_maxsize0);

	/* Store the maximum endpoint 0 packet size */
	usb_dev->usb_max_packet_sz0 = d->dev_maxsize0;

	/* Construct a device address */
	int dev_address = usbbus_alloc_address(usb_dev->usb_bus);
	if (dev_address <= 0) {
		device_printf(dev, "out of addresses on bus %s, aborting attachment!", usb_dev->usb_bus->bus_dev->name);
		return ANANAS_ERROR(NO_RESOURCE);
	}

	/* Assign the device a logical address */
	err = usb_control_xfer(usb_dev, USB_CONTROL_REQUEST_SET_ADDRESS, USB_CONTROL_RECIPIENT_DEVICE, USB_CONTROL_TYPE_STANDARD, dev_address, 0, NULL, NULL, 1);
	ANANAS_ERROR_RETURN(err);

	/*
	 * Address configured - we could attach more things in parallel from now on,
	 * but this only complicates things to no benefit...
	 */
	usb_dev->usb_address = dev_address;
	TRACE_DEV(USB, INFO, usb_dev->usb_device, "logical address is %u", usb_dev->usb_address);

	/* Now, obtain the entire device descriptor */
	len = sizeof(usb_dev->usb_descr_device);
	err = usb_control_xfer(usb_dev, USB_CONTROL_REQUEST_GET_DESC, USB_CONTROL_RECIPIENT_DEVICE, USB_CONTROL_TYPE_STANDARD, USB_REQUEST_MAKE(USB_DESCR_TYPE_DEVICE, 0), 0, d, &len, 0);
	ANANAS_ERROR_RETURN(err);

	TRACE_DEV(USB, INFO, dev,
	 "got full device descriptor: len=%u, type=%u, version=%u, class=%u, subclass=%u, protocol=%u, maxsize=%u, vendor=%u, product=%u numconfigs=%u",
		d->dev_length, d->dev_type, d->dev_version, d->dev_class,
		d->dev_subclass, d->dev_protocol, d->dev_maxsize0, d->dev_vendor,
		d->dev_product, d->dev_num_configs);

	/* Obtain the language ID of this device */
	struct USB_DESCR_STRING s;
	len = 4  /* just the first language */ ;
	err = usb_control_xfer(usb_dev, USB_CONTROL_REQUEST_GET_DESC, USB_CONTROL_RECIPIENT_DEVICE, USB_CONTROL_TYPE_STANDARD, USB_REQUEST_MAKE(USB_DESCR_TYPE_STRING, 0), 0, &s, &len, 0);
	ANANAS_ERROR_RETURN(err);

	/* Retrieved string language code */
	uint16_t langid = s.u.str_langid[0];
	TRACE_DEV(USB, INFO, dev, "got language code, first is %u", langid);

	/* Time to fetch strings; this must be done in two steps: length and content */
	len = 4 /* length only */ ;
	err = usb_control_xfer(usb_dev, USB_CONTROL_REQUEST_GET_DESC, USB_CONTROL_RECIPIENT_DEVICE, USB_CONTROL_TYPE_STANDARD, USB_REQUEST_MAKE(USB_DESCR_TYPE_STRING, d->dev_productidx), langid, &s, &len, 0);
	ANANAS_ERROR_RETURN(err);

	/* Retrieved string length */
	TRACE_DEV(USB, INFO, dev, "got string length=%u", s.str_length);

	/* Fetch the entire string this time */
	struct USB_DESCR_STRING* s_full = (void*)tmp;
	len = s.str_length;
	KASSERT(len < sizeof(tmp), "very large string descriptor %u", s.str_length);
	err = usb_control_xfer(usb_dev, USB_CONTROL_REQUEST_GET_DESC, USB_CONTROL_RECIPIENT_DEVICE, USB_CONTROL_TYPE_STANDARD, USB_REQUEST_MAKE(USB_DESCR_TYPE_STRING, d->dev_productidx), langid, s_full, &len, 0);
	ANANAS_ERROR_RETURN(err);

	kprintf("%s%u: product <", dev->name, dev->unit);
	for (int i = 0; i < s_full->str_length / 2; i++) {
		kprintf("%c", s_full->u.str_string[i] & 0xff);
	}
	kprintf(">\n");

	/*
	 * Grab the first few bytes of configuration descriptor. Note that we
	 * have no idea how long the configuration exactly is, so we must
	 * do this in two steps.
	 */
	struct USB_DESCR_CONFIG c;
	len = sizeof(c);
	err = usb_control_xfer(usb_dev, USB_CONTROL_REQUEST_GET_DESC, USB_CONTROL_RECIPIENT_DEVICE, USB_CONTROL_TYPE_STANDARD, USB_REQUEST_MAKE(USB_DESCR_TYPE_CONFIG, 0), 0, &c, &len, 0);
	ANANAS_ERROR_RETURN(err);

	/* Retrieved partial config descriptor */
	TRACE_DEV(USB, INFO, dev,
	 "got partial config descriptor: len=%u, num_interfaces=%u, id=%u, stringidx=%u, attrs=%u, maxpower=%u, total=%u",
	 c.cfg_length, c.cfg_numinterfaces, c.cfg_identifier, c.cfg_stringidx, c.cfg_attrs, c.cfg_maxpower,
	 c.cfg_totallen);

	/* Fetch the full descriptor */
	struct USB_DESCR_CONFIG* c_full = (void*)tmp;
	len = c.cfg_totallen;
	KASSERT(len < sizeof(tmp), "very large configuration descriptor %u", c.cfg_totallen);
	err = usb_control_xfer(usb_dev, USB_CONTROL_REQUEST_GET_DESC, USB_CONTROL_RECIPIENT_DEVICE, USB_CONTROL_TYPE_STANDARD, USB_REQUEST_MAKE(USB_DESCR_TYPE_CONFIG, 0), 0, c_full, &len, 0);
	ANANAS_ERROR_RETURN(err);

	/* Retrieved full device descriptor */
	TRACE_DEV(USB, INFO, dev, "got full config descriptor");

	/* Handle the configuration */
	err = usb_parse_configuration(usb_dev, c_full, c.cfg_totallen);
	ANANAS_ERROR_RETURN(err);

	/* For now, we'll just activate the very first configuration */
	err = usb_control_xfer(usb_dev, USB_CONTROL_REQUEST_SET_CONFIGURATION, USB_CONTROL_RECIPIENT_DEVICE, USB_CONTROL_TYPE_STANDARD, c_full->cfg_identifier, 0, NULL, NULL, 1);
	ANANAS_ERROR_RETURN(err);

	/* Configuration activated */
	TRACE_DEV(USB, INFO, dev, "configuration activated");
	usb_dev->usb_cur_interface = 0;

	/* Now, we'll have to hook up some driver... XXX this is duplicated from device.c/pcibus.c */
	int device_attached = 0;
	DQUEUE_FOREACH(&probe_queue, p, struct PROBE) {
		/* See if the device lives on our bus */
		int exists = 0;
		for (const char** curbus = p->bus; *curbus != NULL; curbus++) {
			/*
			 * Note that we need to check the _parent_ as that is the bus; 'dev' will
			 * be turned into a fully-flegded device once we find a driver which
			 * likes it.
			 */
			if (strcmp(*curbus, dev->parent->name) == 0) {
				exists = 1;
				break;
			}
		}
		if (!exists)
			continue;

		/* This device may work - give it a chance to attach */
		dev->driver = p->driver;
		strcpy(dev->name, dev->driver->name);
		dev->unit = dev->driver->current_unit++;
		errorcode_t err = device_attach_single(dev);
		if (err == ANANAS_ERROR_NONE) {
			/* This worked; use the next unit for the new device */
			device_attached++;
			break;
		} else {
			/* No luck, revert the unit number */
			dev->driver->current_unit--;
		}
	}

	if (!device_attached) {
		/* XXX we should revert to some 'generic USB' device here... */
		kprintf("XXX no device found, removing !!!\n");
		dev->driver = NULL;
		usb_free_device(usb_dev);
	}
	return ANANAS_ERROR_OK;
}

/* vim:set ts=2 sw=2: */
