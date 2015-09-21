#include <ananas/console.h>
#include <ananas/device.h>
#include <ananas/error.h>
#include <ananas/kdb.h>
#include <ananas/lib.h>
#include <ananas/mm.h>
#include <ananas/kmem.h>
#include <ananas/tty.h>
#include <ananas/thread.h>
#include <ananas/trace.h>
#include <ananas/vm.h>
#include "options.h"

TRACE_SETUP;

/*
 * devhints.inc will be generated by config and lists all hints we have.
 */
#include "hints.inc"

static void device_print_attachment(device_t dev);

static spinlock_t spl_devicequeue = SPINLOCK_DEFAULT_INIT;
static struct DEVICE_QUEUE device_queue;
struct DEVICE_PROBE probe_queue; /* XXX not locked yet */

/* Note: drv = NULL will be used if the driver isn't yet known! */
device_t
device_alloc(device_t bus, driver_t drv)
{
	device_t dev = kmalloc(sizeof(struct DEVICE));
	memset(dev, 0, sizeof(struct DEVICE));
	dev->driver = drv;
	dev->parent = bus;
	sem_init(&dev->waiters, 1);

	if (drv != NULL) {
		strcpy(dev->name, drv->name);
		dev->unit = drv->current_unit++;
	}

	return dev;
}

/* Clones a device; it will get a new unit number */
device_t
device_clone(device_t dev)
{
	device_t new_dev = device_alloc(dev->parent, dev->driver);

	/* Copy the resources over */
	for (int i = 0; i < DEVICE_MAX_RESOURCES; i++) {
		new_dev->resource[i].type = dev->resource[i].type;
		new_dev->resource[i].base = dev->resource[i].base;
		new_dev->resource[i].length = dev->resource[i].length;
	}
	return new_dev;
}

void
device_free(device_t dev)
{
	/* XXX clear waiters; should we signal them? */

	spinlock_lock(&spl_devicequeue);
	DQUEUE_REMOVE(&device_queue, dev);
	spinlock_unlock(&spl_devicequeue);
	kfree(dev);
}

static void
device_add_to_tree(device_t dev)
{
	spinlock_lock(&spl_devicequeue);
	if (!DQUEUE_EMPTY(&device_queue)) {
		DQUEUE_FOREACH(&device_queue, dq_dev, struct DEVICE) {
			if (dq_dev == dev)
				goto skip;
		}
	}
	DQUEUE_ADD_TAIL(&device_queue, dev);
skip:
	spinlock_unlock(&spl_devicequeue);
}

errorcode_t
device_attach_single(device_t dev)
{
	driver_t driver = dev->driver;

	if (driver->drv_probe != NULL) {
		/*
		 * This device has a probe function; we must call it to figure out
	 	 * whether the device actually exists or we're about to attach
		 * something out of thin air here...
		 */
		errorcode_t err = driver->drv_probe(dev);
		ANANAS_ERROR_RETURN(err);
	}

	/*
	 * XXX This is a kludge: it prevents us from displaying attach information for drivers
	 * that will be initialize outside the probe tree (such as the console which will be
	 * initialized as soon as possible.
	 */
	if (dev->parent != NULL)
		device_print_attachment(dev);
	if (driver->drv_attach != NULL) {
		errorcode_t err = driver->drv_attach(dev);
		ANANAS_ERROR_RETURN(err);
	}

	/* Hook the device up to the tree */
	device_add_to_tree(dev);

	/* Attempt to attach child devices, if any */
	if (driver->drv_attach_children != NULL)
		driver->drv_attach_children(dev);
	device_attach_bus(dev);

	return ANANAS_ERROR_OK;
}

static int
device_resolve_type(char** value)
{
	if (!memcmp(*value, "mem=", 4)) {
		*value += 4;
		return RESTYPE_MEMORY;
	}
	if (!memcmp(*value, "io=", 3)) {
		*value += 3;
		return RESTYPE_IO;
	}
	if (!memcmp(*value, "irq=", 4)) {
		*value += 4;
		return RESTYPE_IRQ;
	}
	return RESTYPE_UNUSED;
}

int
device_add_resource(device_t dev, resource_type_t type, unsigned int base, unsigned int len)
{
	unsigned int curhint;

	for (curhint = 0; curhint < DEVICE_MAX_RESOURCES; curhint++) {
		if (dev->resource[curhint].type != RESTYPE_UNUSED)
			continue;
		dev->resource[curhint].type = type;
		dev->resource[curhint].base = base;
		dev->resource[curhint].length = len;
		return 1;
	}

	return 0;
}

/*
 * Handles retrieving the resources for a device based on a given hint.
 */
int
device_get_resources_byhint(device_t dev, const char* hint, const char** hints)
{
	const char** curhint;
	int num_hints = 0;

	/* Clear out any current device resources */
	memset(dev->resource, 0, sizeof(struct RESOURCE) * DEVICE_MAX_RESOURCES);
	for (curhint = hints; *curhint != NULL; curhint++) {
		/*
		 * A hint is in the form bus.device.unit.resource, i.e. isa.vga.0.io. We
		 * need to match it piece-for-piece so that we can handle wildcards
		 * (i.e. *.vga.0.io must match both isa.vga.0.io and pci.vga.0.io)
		 */
		const char* src = hint;
		const char* dst = *curhint;
		while(*src != '\0' && *dst != '\0') {
			/* If we need to match a wildcard, skip over the entire thing */
			if (*src == '*' && *(src + 1) == '.') {
				const char* ptr = strchr(dst, '.');
				if (ptr == NULL) {
					ptr = strchr(dst, '\0');
					continue;
				}
				src += 2;
				dst = ptr + 1;
				continue;
			}
			/* Match both parts */
			const char* ptr1 = strchr(src, '.');
			const char* ptr2 = strchr(dst, '.');
			if (ptr1 == NULL)
				ptr1 = strchr(src, '\0');
			if (ptr2 == NULL)
				ptr2 = strchr(dst, '\0');
			if (ptr1 - src != ptr2 - dst)
				break;

			if (strncmp(src, dst, ptr1 - src) != 0)
				break;

			src = ptr1; dst = ptr2;
			if (*src == '.') src++;
			if (*dst == '.') dst++;
		}
		if (*src != '\0')
			continue;

		/*
		 * We got a resource match; need to figure out the type.
		 */
		char* value = (char*)dst;
		int type = device_resolve_type(&value);
		if (type == RESTYPE_UNUSED) {
			kprintf("%s: ignoring unparsable resource '%s'\n", dev->name, *curhint);
			continue;
		}
		unsigned long v = strtoul(value, NULL, 0);
		if (!device_add_resource(dev, type, v, 0)) {
			kprintf("%s: skipping resource type 0x%x, too many specified\n", dev->name, type);
			continue;
		} else {
			num_hints++;
		}
	}

	return num_hints;
}

/*
 * Handles retrieving the resources for a bus.device.unit from
 * the hints.
 */
int
device_get_resources(device_t dev, const char** hints)
{
	char tmphint[32 /* XXX */];

	if (dev->parent != NULL)
		sprintf(tmphint, "%s.%s.%u.", dev->parent->name, dev->name, dev->unit);
	else
		sprintf(tmphint, "%s.%u.", dev->name, dev->unit);

	return device_get_resources_byhint(dev, tmphint, hints);
}

static void
device_print_attachment(device_t dev)
{
	KASSERT(dev->parent != NULL, "can't print device which doesn't have a parent bus");
	kprintf("%s%u on %s%u ", dev->name, dev->unit, dev->parent->name, dev->parent->unit);
	int i;
	for (i = 0; i < DEVICE_MAX_RESOURCES; i++) {
		int hex = 0;
		switch (dev->resource[i].type) {
			case RESTYPE_MEMORY: kprintf("memory "); hex = 1; break;
			case RESTYPE_IO: kprintf("io "); hex = 1; break;
			case RESTYPE_IRQ: kprintf("irq "); break;
			case RESTYPE_CHILDNUM: kprintf("child "); break;
			case RESTYPE_PCI_BUS: kprintf("bus "); break;
			case RESTYPE_PCI_DEVICE: kprintf("dev "); break;
			case RESTYPE_PCI_FUNCTION: kprintf("func "); break;
			case RESTYPE_PCI_VENDORID: kprintf("vendor "); hex = 1; break;
			case RESTYPE_PCI_DEVICEID: kprintf("device "); hex = 1; break;
			case RESTYPE_PCI_CLASSREV: kprintf("class/revision "); break;
			default: continue;
		}
		kprintf(hex ? "0x%x" : "%u", dev->resource[i].base);
		if (dev->resource[i].length > 0)
			kprintf(hex ? "-0x%x" : "-%u", dev->resource[i].base + dev->resource[i].length);
		kprintf(" ");
	}
	kprintf("\n");
}

struct RESOURCE*
device_get_resource(device_t dev, resource_type_t type, int index)
{
	int i;
	for (i = 0; i < DEVICE_MAX_RESOURCES; i++) {
		if (dev->resource[i].type != type)
			continue;
		if (index-- > 0)
			continue;
		return &dev->resource[i];
	}

	return NULL;
}

void*
device_alloc_resource(device_t dev, resource_type_t type, size_t len)
{
	struct RESOURCE* res = device_get_resource(dev, type, 0);
	if (res == NULL)
		/* No such resource! */
		return NULL;

	res->length = len;

	switch(type) {
		case RESTYPE_MEMORY:
			return (void*)kmem_map(res->base, len, VM_FLAG_READ | VM_FLAG_WRITE | VM_FLAG_DEVICE);
		case RESTYPE_IO:
		case RESTYPE_CHILDNUM:
		case RESTYPE_IRQ: /* XXX should allocate, not just return */
			return (void*)(uintptr_t)res->base;
		default:
			panic("%s: resource type %u exists, but can't allocate", dev->name, type);
	}

	/* NOTREACHED */
	return NULL;
}

/*
 * Handles attaching devices to a bus; may recursively call itself.
 */
void
device_attach_bus(device_t bus)
{
	if (DQUEUE_EMPTY(&probe_queue))
		return;

	/*
	 * Fetch TTY devices; we need them to report the device that is already
	 * attached at this point.
	 */
	device_t input_dev = tty_get_inputdev(console_tty);
	device_t output_dev = tty_get_outputdev(console_tty);
	DQUEUE_FOREACH_SAFE(&probe_queue, p, struct PROBE) {
		/* See if the device exists on this bus */
		int exists = 0;
		for (const char** curbus = p->bus; *curbus != NULL; curbus++) {
			if (strcmp(*curbus, bus->name) == 0) {
				exists = 1;
				break;
			}
		}

		if (!exists)
			continue;

		/*
		 * OK, the device may be present on this bus; allocate a device for it.
		 */
		driver_t driver = p->driver;
		KASSERT(driver != NULL, "matched a probe device without a driver!");

		/*
		 * If we found the driver for the in- or output driver, display it; we'll give
		 * the extra units a chance to attach as well.
		 */
		if (input_dev != NULL && input_dev->driver == driver) {
			input_dev->parent = bus;
			device_print_attachment(input_dev);
			device_add_to_tree(input_dev);
		}
		if (output_dev != NULL && output_dev->driver == driver && input_dev != output_dev) {
			output_dev->parent = bus;
			device_print_attachment(output_dev);
			device_add_to_tree(output_dev);
		}

		/* Attach any units */
		while (1) {
			device_t dev = device_alloc(bus, driver);
			/*
			 * Obtain resources; note that if we cannot obtain any, we still attempt
			 * to attach the first unit of the device (as it may not require any
			 * additional resources to function properly)
			 */
			if (device_get_resources(dev, config_hints) == 0 && dev->unit > 0)
				break;

			/* This unlock/lock should be safe because we use the safe foreach */
			errorcode_t err = device_attach_single(dev);
			if (err != ANANAS_ERROR_OK) {
				device_free(dev);
				continue;
			}
		}
	}
}

static errorcode_t
device_init()
{
	DQUEUE_INIT(&device_queue);

	/*
	 * First of all, create the core bus; this is as bare to the metal as it
	 * gets.
	 */
	device_t corebus = (device_t)kmalloc(sizeof(struct DEVICE));
	memset(corebus, 0, sizeof(struct DEVICE));
	strcpy(corebus->name, "corebus");
	device_attach_bus(corebus);
	return ANANAS_ERROR_OK;
}

INIT_FUNCTION(device_init, SUBSYSTEM_DEVICE, ORDER_LAST);

struct DEVICE*
device_find(const char* name)
{
	char* ptr = (char*)name;
	while (*ptr != '\0' && (*ptr < '0' || *ptr > '9')) ptr++;
	int unit = (*ptr != '\0') ? strtoul(ptr, NULL, 10) : 0;

	DQUEUE_FOREACH(&device_queue, dev, struct DEVICE) {
		if (!strncmp(dev->name, name, ptr - name) && dev->unit == unit)
			return dev;
	}
	return NULL;
}

errorcode_t
device_write(device_t dev, const char* buf, size_t* len, off_t offset)
{
	KASSERT(dev->driver != NULL, "device_write() without a driver");
	KASSERT(dev->driver->drv_write != NULL, "device_write() without drv_write");

	return dev->driver->drv_write(dev, buf, len, offset);
}

errorcode_t
device_bwrite(device_t dev, struct BIO* bio)
{
	KASSERT(dev->driver != NULL, "device_bwrite() without a driver");
	KASSERT(dev->driver->drv_bwrite != NULL, "device_bwrite() without drv_bwrite");

	return dev->driver->drv_bwrite(dev, bio);
}

errorcode_t
device_read(device_t dev, char* buf, size_t* len, off_t offset)
{
	KASSERT(dev->driver != NULL, "device_read() without a driver");
	KASSERT(dev->driver->drv_read != NULL, "device_read() without drv_read");

	return dev->driver->drv_read(dev, buf, len, offset);
}

errorcode_t
device_bread(device_t dev, struct BIO* bio)
{
	KASSERT(dev->driver != NULL, "device_bread() without a driver");
	KASSERT(dev->driver->drv_bread != NULL, "device_bread() without drv_bread");

	return dev->driver->drv_bread(dev, bio);
}

void
device_printf(device_t dev, const char* fmt, ...)
{
	va_list va;

	/* XXX will interleave printf's in SMP */
	kprintf("%s%u: ", dev->name, dev->unit);
	va_start(va, fmt);
	vaprintf(fmt, va);
	va_end(va);
	kprintf("\n");
}

struct DEVICE_QUEUE*
device_get_queue()
{
	return &device_queue;
}

errorcode_t
device_register_probe(struct PROBE* p)
{
	if (!DQUEUE_EMPTY(&probe_queue))
		DQUEUE_FOREACH(&probe_queue, pqi, struct PROBE) {
			if (pqi->driver == p->driver) {
				/* Duplicate driver */
				return ANANAS_ERROR(FILE_EXISTS);
			}
		}
	DQUEUE_ADD_TAIL(&probe_queue, p);
	return ANANAS_ERROR_OK;
}

errorcode_t
device_unregister_probe(struct PROBE* p)
{
	DQUEUE_REMOVE(&probe_queue, p);
	return ANANAS_ERROR_OK;
}

#ifdef OPTION_KDB
KDB_COMMAND(devices, NULL, "Displays a list of all devices")
{
	DQUEUE_FOREACH(&device_queue, dev, struct DEVICE) {
		kprintf("device %p: '%s' unit %u\n",
	 	 dev, dev->name, dev->unit);
	}
}
#endif

/* vim:set ts=2 sw=2: */
