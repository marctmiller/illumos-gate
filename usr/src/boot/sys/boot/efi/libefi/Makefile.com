#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#

#
# Copyright 2016 Toomas Soome <tsoome@me.com>
#

include $(SRC)/Makefile.master

CC=     $(GCC_ROOT)/bin/gcc

install:

SRCS +=	delay.c devpath.c efi_console.c efinet.c efipart.c env.c errno.c \
	gfx_fb.c handles.c libefi.c wchar.c

OBJS=	$(SRCS:%.c=%.o)

CFLAGS = -O2

#.if ${MACHINE_CPUARCH} == "aarch64"
#CFLAGS+=	-msoft-float -mgeneral-regs-only
#.endif

CPPFLAGS = -nostdinc -I. -I../../../../../include -I../../../..
CPPFLAGS += -I$(SRC)/common/ficl -I../../libficl
CPPFLAGS += -I../../include
CPPFLAGS += -I../../include/${MACHINE}
CPPFLAGS += -I../../../../../lib/libstand

# Pick up the bootstrap header for some interface items
CPPFLAGS += -I../../../common
CPPFLAGS += -DSTAND -DEFI

# Handle FreeBSD specific %b and %D printf format specifiers
# CFLAGS+= ${FORMAT_EXTENSIONS}
# CFLAGS += -D__printf__=__freebsd_kprintf__

include ../../Makefile.inc

# For multiboot2.h, must be last, to avoid conflicts
CPPFLAGS +=	-I$(SRC)/uts/common

libefi.a: $(OBJS)
	$(AR) $(ARFLAGS) $@ $(OBJS)

clean: clobber
clobber:
	$(RM) $(CLEANFILES) $(OBJS) libefi.a

machine:
	$(RM) machine
	$(SYMLINK) ../../../../${MACHINE}/include machine

x86:
	$(RM) x86
	$(SYMLINK) ../../../../x86/include x86

%.o:	../%.c
	$(COMPILE.c) $<

%.o:	../../../common/%.c
	$(COMPILE.c) $<
