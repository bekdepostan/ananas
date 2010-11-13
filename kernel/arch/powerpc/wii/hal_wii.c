#include <ananas/types.h>
#include <machine/hal.h>
#include <ananas/lib.h>

/*
 * Wii physical memory map is as follows (http://www.wiibrew.org/wiki/Memory_Map)
 *
 * MEM1: 0x00000000 - 0x017fffff, 24MB memory
 *  XFB: 0x01698000 - 0x017fffff, framebuffer memory (2MB, contained in MEM1)
 * Hole: 0x017fffff - 0x0cffffff
 *  I/O: 0x0d000000 - 0x0d008000, hollywood registers
 * Hole: 0x0d008001 - 0x0fffffff
 * MEM2: 0x10000000 - 0x13ffffff, 64MB memory
 *  IOS: 0x133e0000 - 0x13ffffff, IOS memory (12MB, contained in MEM2)
 *
 * However, we need to reserve some memory from the MEM1 range:
 *  - 0x0 - 0x3f00   - contains various information on the Wii
 */

static struct HAL_REGION hal_avail_region[] = {
 { .reg_base = 0x00000000, .reg_size = 0x1697fff },		/* MEM1: 24MB - 2MB framebuffer = 22MB */
 { .reg_base = 0x10000000, .reg_size = 0x33dffff },		/* MEM2: 64MB - IOS = 52MB */
};

size_t
hal_init_memory()
{
	size_t memory_total = 0;

	kprintf("IOS heap range: %x - %x\n", *(uint32_t*)(0x3130), *(uint32_t*)(0x3134));
	kprintf("Area lo       : %x\n", *(uint32_t*)(0x30));
	kprintf("Area hi       : %x\n", *(uint32_t*)(0x34));
	kprintf("CPU speed     : %u\n", *(uint32_t*)(0xfc));

	for (unsigned int i = 0; i < sizeof(hal_avail_region) / sizeof(struct HAL_REGION); i++) {
		memory_total += hal_avail_region[i].reg_size;
	}
	return memory_total;
}

void
hal_get_available_memory(struct HAL_REGION** region, int* num_regions)
{
	*region = hal_avail_region;
	*num_regions = sizeof(hal_avail_region) / sizeof(struct HAL_REGION);
}

uint32_t
hal_get_cpu_speed()
{
	return 729000000; /* 729MHz */
}

/* vim:set ts=2 sw=2: */