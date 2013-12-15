NDK?=/opt/android-ndk
CC_VER?=4.8
CROSS?=arm-linux-androideabi
XCC?=$(NDK)/toolchains/$(CROSS)-$(CC_VER)/prebuilt/linux-$(shell uname -m)/bin/$(CROSS)-gcc
ABI?=android-16/arch-arm
CPUTYPE?=armv7-a
SYSROOT?=$(NDK)/platforms/$(ABI)
CFLAGS?=-O2 -pipe -Wall -Wextra -pedantic

ifdef RICE
CFLAGS=-frename-registers -flto -march=armv7-a -ansi -fwhole-program -O3 -pipe -fomit-frame-pointer
endif

LDFLAGS?=-s
LIBS?=-pthread

SYS_CFLAGS?=-isysroot$(SYSROOT) -isystem$(SYSROOT)/usr/include -B$(SYSROOT)/usr/lib/

all: battery-decision

battery-decision: Makefile main.c
	$(XCC) $(SYS_CFLAGS) $(CFLAGS) -o battery-decision main.c $(LIBS) $(LDFLAGS)

clean:
	rm -fv ./battery-decision
