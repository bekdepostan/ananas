#include <ananas/types.h>
#include <ananas/lib.h>

/*
 * The naive implementation is easy but slow; we can do much better by
 * attempting to copy 32-bit chunks in a single go every time - this results in
 * a massive speedup (we choose 32-bit chunks because most CPU's seem to like
 * that)
 */
#undef NAIVE_IMPLEMENTATION

void*
memcpy(void* dst, const void* src, size_t len)
{
#ifdef NAIVE_IMPLEMENTATION
	char* d = (char*)dst;
	char* s = (char*)src;
	while (len--) {
		*d++ = *s++;
	}
#else
	/* Copies sz bytes of variable type T-sized data */
#define DO_COPY(T, sz) \
		do { \
			register size_t x = (size_t)sz; \
			while (x >= sizeof(T)) { \
				*(T*)d = *(T*)s; \
				x -= sizeof(T); \
				s += sizeof(T); \
				d += sizeof(T); \
				len -= sizeof(T);  \
			} \
		} while(0)

	/*
	 * Using the register prefix makes the resulting code much more efficient on
	 * i386 as this prevents values from being used on the stack.
	 */
	register void* d = dst;
	register const void* s = src;

	/* First of all, attempt to align to 32-bit boundary */
	if (len >= 4 && ((addr_t)dst & 3))
		DO_COPY(uint8_t, len & 3);

	/* Attempt to copy everything using 4 bytes at a time */
	DO_COPY(uint32_t, len);

	/* Cover the leftovers */
	DO_COPY(uint8_t, len);

#undef DO_COPY
#endif
	return dst;
}

/* vim:set ts=2 sw=2: */
