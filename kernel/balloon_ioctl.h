/*
 * balloon_ioctl.h — Shared header between kernel module and userspace tools
 *
 * This file is the kernel-level equivalent of shared_mem.h from the userspace version.
 * Both the kernel module (balloon_kmod.c) and the userspace host daemon (host_kmod.c)
 * include this file so they agree on:
 *   1. What ioctl commands exist (inflate, deflate, get status, reset)
 *   2. What data structures are sent back and forth
 *
 * In the userspace version, host and guest communicated through shared memory files
 * in /tmp/ (mmap'd structs). Here, communication happens through ioctl() calls on
 * a character device (/dev/balloon_ctl). ioctl is how real kernel drivers expose
 * control interfaces to userspace — VirtualBox guest additions, GPU drivers, etc.
 *
 * HOW IOCTL WORKS:
 *   1. Kernel module creates a device file (/dev/balloon_ctl) via misc_register()
 *   2. Userspace opens it: fd = open("/dev/balloon_ctl", O_RDWR)
 *   3. Userspace sends commands: ioctl(fd, BALLOON_IOC_INFLATE, &cmd)
 *   4. Kernel module's ioctl handler receives the command and acts on it
 *
 * This is analogous to how virtio devices use PCI BARs and virtqueues —
 * ioctl is just a simpler transport mechanism for the same idea.
 */

#ifndef BALLOON_IOCTL_H
#define BALLOON_IOCTL_H

/*
 * We need different headers depending on whether we're compiling
 * inside the kernel or in userspace. Both provide the _IOW/_IOR/_IO
 * macros used to define ioctl command numbers, but from different paths.
 */
#ifdef __KERNEL__
#include <linux/ioctl.h>   /* kernel-space ioctl definitions */
#else
#include <sys/ioctl.h>     /* userspace ioctl definitions */
#endif

/*
 * IOCTL MAGIC NUMBER
 *
 * Every ioctl driver picks a unique "magic" byte to namespace its commands.
 * This prevents collisions — if two drivers both used command number 1,
 * the kernel wouldn't know which driver to route the call to.
 *
 * We use 'B' for Balloon. The Linux kernel maintains a registry of magic
 * numbers in Documentation/userspace-api/ioctl/ioctl-number.rst.
 * For a student project, any unused letter is fine.
 */
#define BALLOON_IOC_MAGIC 'B'

/*
 * struct balloon_cmd — Command from host to kernel module
 *
 * This is what the host daemon sends when it wants to inflate or deflate.
 * It replaces the `command` + `target_pages` fields from struct balloon_shm
 * in the userspace version.
 *
 * In shared_mem.h we had:
 *   shm->command      = CMD_INFLATE;
 *   shm->target_pages = 100;
 *
 * Now we do:
 *   struct balloon_cmd cmd = { .target_pages = 100 };
 *   ioctl(fd, BALLOON_IOC_INFLATE, &cmd);
 *
 * The command type (inflate vs deflate) is encoded in the ioctl number itself,
 * so we only need the target page count in the struct.
 */
struct balloon_cmd {
    int target_pages;   /* how many pages the balloon should hold after this command */
};

/*
 * struct balloon_status — Status report from kernel module to userspace
 *
 * This replaces reading fields from struct balloon_shm in the userspace version.
 * The host daemon calls ioctl(fd, BALLOON_IOC_GET_STATUS, &status) and the
 * kernel module fills in all these fields.
 *
 * KEY DIFFERENCE from userspace version:
 *   - pressure is now REAL (calculated from actual system memory via si_meminfo())
 *   - free_kb and total_kb are actual system memory stats (same as /proc/meminfo)
 *   - no more random dice rolls for pressure simulation
 */
struct balloon_status {
    int current_pages;     /* pages currently held in the balloon (same as shm->current_pages) */
    int peak_pages;        /* highest balloon size ever reached (same as shm->peak_pages) */
    int max_pages;         /* ceiling set via module parameter at insmod time */
    int total_inflated;    /* lifetime count of pages inflated (same as shm->total_inflated) */
    int total_deflated;    /* lifetime count of pages deflated (same as shm->total_deflated) */
    int pressure;          /* 0=NONE, 1=LOW, 2=CRITICAL — calculated from REAL memory stats */
    unsigned long free_kb; /* actual system free memory in KB (from kernel's si_meminfo) */
    unsigned long total_kb;/* actual system total memory in KB (from kernel's si_meminfo) */
};

/*
 * IOCTL COMMAND DEFINITIONS
 *
 * These macros create unique 32-bit command numbers that encode:
 *   - Magic number ('B')       — identifies our driver
 *   - Command number (1,2,3,4) — identifies the operation
 *   - Direction (read/write)   — tells the kernel which way data flows
 *   - Data size                — how many bytes to copy between user/kernel space
 *
 * _IOW = "I/O Write" — userspace WRITES data TO the kernel (host sends a command)
 * _IOR = "I/O Read"  — userspace READS data FROM the kernel (host gets status)
 * _IO  = "I/O"       — no data transfer, just a signal (reset command)
 *
 * These map to the old shared memory commands:
 *   CMD_INFLATE  (shared_mem.h)  →  BALLOON_IOC_INFLATE  (ioctl)
 *   CMD_DEFLATE  (shared_mem.h)  →  BALLOON_IOC_DEFLATE  (ioctl)
 *   reading shm fields           →  BALLOON_IOC_GET_STATUS (ioctl)
 *   (no equivalent before)       →  BALLOON_IOC_RESET     (ioctl)
 */

/* Host sends target page count, kernel inflates the balloon to that level */
#define BALLOON_IOC_INFLATE    _IOW(BALLOON_IOC_MAGIC, 1, struct balloon_cmd)

/* Host sends target page count, kernel deflates the balloon to that level */
#define BALLOON_IOC_DEFLATE    _IOW(BALLOON_IOC_MAGIC, 2, struct balloon_cmd)

/* Host requests current balloon status, kernel fills in the struct */
#define BALLOON_IOC_GET_STATUS _IOR(BALLOON_IOC_MAGIC, 3, struct balloon_status)

/* Host tells the kernel to deflate everything and reset all counters */
#define BALLOON_IOC_RESET      _IO(BALLOON_IOC_MAGIC, 4)

#endif /* BALLOON_IOCTL_H */
