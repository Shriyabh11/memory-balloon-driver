# Makefile for the kernel balloon driver
#
# Builds:
#   balloon_kmod.ko  — the kernel module (loaded with insmod)
#   host_kmod        — userspace host daemon (sends ioctl commands)
#   monitor_kmod     — userspace monitor (reads /proc/balloon)
#
# Prerequisites:
#   sudo apt install build-essential linux-headers-$(uname -r)
#
# Usage:
#   make              — build everything
#   sudo insmod balloon_kmod.ko
#   sudo ./host_kmod
#   sudo rmmod balloon_kmod
#   make clean        — remove all build artifacts

# --- Kernel module build ---
# When the kernel build system invokes us, KERNELRELEASE is defined.
# Otherwise we're being called directly by the user.

ifneq ($(KERNELRELEASE),)

obj-m := balloon_kmod.o

else

KDIR  ?= /lib/modules/$(shell uname -r)/build
PWD   := $(shell pwd)
CC    := gcc
CFLAGS := -Wall -Wextra -g

.PHONY: all modules userspace clean help

all: modules userspace
	@echo ""
	@echo "════════════════════════════════════════════════"
	@echo "  Build successful!"
	@echo ""
	@echo "  Quick start:"
	@echo "    sudo insmod balloon_kmod.ko"
	@echo "    sudo ./host_kmod"
	@echo ""
	@echo "  In another terminal:"
	@echo "    watch -n1 cat /proc/balloon"
	@echo ""
	@echo "  Kernel logs:"
	@echo "    sudo dmesg -w"
	@echo ""
	@echo "  Cleanup:"
	@echo "    sudo rmmod balloon_kmod"
	@echo "════════════════════════════════════════════════"

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

userspace: host_kmod monitor_kmod

host_kmod: host_kmod.c balloon_ioctl.h
	$(CC) $(CFLAGS) host_kmod.c -o host_kmod

monitor_kmod: monitor_kmod.c balloon_ioctl.h
	$(CC) $(CFLAGS) monitor_kmod.c -o monitor_kmod

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f host_kmod monitor_kmod

help:
	@echo "Targets:"
	@echo "  all      — build kernel module + userspace tools"
	@echo "  modules  — build only the .ko kernel module"
	@echo "  userspace— build only host_kmod and monitor_kmod"
	@echo "  clean    — remove all build artifacts"
	@echo "  help     — show this message"

endif
