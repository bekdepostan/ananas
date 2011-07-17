#include <ananas/types.h>
#include <ananas/lib.h>
#include <ananas/trace.h>

uint32_t trace_subsystem_mask[TRACE_SUBSYSTEM_LAST];

void
tracef(int fileid, const char* func, const char* fmt, ...)
{
	uint32_t timestamp = 0;
	#if defined(__i386__) || defined(__amd64__)
#if 1
		uint32_t x86_get_ms_since_boot();
		timestamp = x86_get_ms_since_boot();
#endif
	#endif

	/* XXX This function is a SMP-disaster for now */
  va_list ap;
	kprintf("[% 4u.%03u] %s: ", timestamp / 1000, timestamp % 1000, func);
  va_start(ap, fmt);
  vaprintf(fmt, ap);
  va_end(ap);
	kprintf("\n");
}

/* vim:set ts=2 sw=2: */
