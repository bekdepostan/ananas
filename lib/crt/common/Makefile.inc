CC=		${TOOL_PREFIX}clang
LD=		${TOOL_PREFIX}ld

OBJS=		crt0.o crtn.o crti.o crtbegin.o crtend.o

# flags
CFLAGS=		--sysroot ${SYSROOT}
CFLAGS+=	-I../../../include -I. -std=c99
CFLAGS+=	-Wall -Werror
CFLAGS+=	-Wno-builtin-requires-header

.PHONY:		all clean

all:		$(OBJS)

machine:	../../../include/ananas/${ARCH}
		ln -sf ../../../include/ananas/${ARCH} machine

crt0.o:		machine ../common/crt0.c
		$(CC) $(CFLAGS) -c -o crt0.o ../common/crt0.c

install:	${SYSROOT}/usr/lib/crt0.o ${SYSROOT}/usr/lib/crti.o ${SYSROOT}/usr/lib/crtn.o ${SYSROOT}/usr/lib/crtbegin.o ${SYSROOT}/usr/lib/crtend.o

${SYSROOT}/usr/lib/crt0.o: crt0.o
		cp crt0.o ${SYSROOT}/usr/lib

${SYSROOT}/usr/lib/crti.o: crti.o
		cp crti.o ${SYSROOT}/usr/lib

${SYSROOT}/usr/lib/crtn.o: crtn.o
		cp crtn.o ${SYSROOT}/usr/lib

${SYSROOT}/usr/lib/crtbegin.o: crtbegin.o
		cp crtbegin.o ${SYSROOT}/usr/lib

${SYSROOT}/usr/lib/crtend.o: crtend.o
		cp crtend.o ${SYSROOT}/usr/lib

clean:
		rm -f machine $(OBJS)
