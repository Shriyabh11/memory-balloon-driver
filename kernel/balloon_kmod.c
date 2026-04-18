/*
 * balloon_kmod.c — Kernel-Level Memory Balloon Driver (Loadable Kernel Module)
 *
 * This is the REAL kernel version of guest.c. Instead of running as a userspace
 * process that fakes page allocation with malloc(), this runs INSIDE the Linux
 * kernel at ring 0, using the actual kernel memory management APIs.
 *
 * WHAT CHANGED FROM guest.c:
 *   malloc(4096)           →  alloc_page(GFP_KERNEL)   real buddy allocator
 *   free(page)             →  __free_page(page)         real page freeing
 *   pages_held[] array     →  struct list_head           kernel linked list
 *   printf()               →  printk() / pr_info()      visible in dmesg
 *   /tmp/ shared memory    →  /dev/balloon_ctl (ioctl) + /proc/balloon
 *   simulate_pressure()    →  si_meminfo()              real /proc/meminfo data
 *
 * BUILD:  make   (requires linux-headers-$(uname -r))
 * USAGE:
 *   sudo insmod balloon_kmod.ko                   (load, default 200 max pages)
 *   sudo insmod balloon_kmod.ko max_pages=500     (load with custom max)
 *   cat /proc/balloon                             (check status)
 *   sudo dmesg -w                                 (watch kernel logs)
 *   sudo rmmod balloon_kmod                       (unload, auto-frees all pages)
 */

/* ── kernel headers ──
 * These are NOT the libc headers we used in guest.c (#include <stdio.h> etc.)
 * They live in /lib/modules/$(uname -r)/build/include/linux/
 * and provide access to kernel-only APIs.
 */

#include <linux/module.h>      /* ALL kernel modules need this: MODULE_LICENSE, module_init, etc. */
#include <linux/kernel.h>      /* pr_info, pr_warn, pr_err — kernel logging macros (replace printf) */
#include <linux/init.h>        /* __init and __exit — mark init/cleanup functions for memory optimization */
#include <linux/fs.h>          /* file_operations struct — needed to create /dev/ devices */
#include <linux/miscdevice.h>  /* misc_register() — simpler way to create a char device */
#include <linux/proc_fs.h>     /* proc_create() — creates entries under /proc/ filesystem */
#include <linux/seq_file.h>    /* seq_printf() — printf-like function for writing to /proc files */
#include <linux/slab.h>        /* kmalloc, kfree — kernel heap allocation (for our tracking structs) */
#include <linux/list.h>        /* LIST_HEAD, list_add, list_del — kernel doubly-linked list */
#include <linux/mutex.h>       /* DEFINE_MUTEX, mutex_lock/unlock — kernel mutex (thread safety) */
#include <linux/mm.h>          /* si_meminfo() — reads system memory stats (like /proc/meminfo) */
#include <linux/gfp.h>         /* GFP_KERNEL, alloc_page, __free_page — THE page allocator */
#include <linux/uaccess.h>     /* copy_to_user, copy_from_user — safe user↔kernel data transfer */
#include <linux/version.h>     /* LINUX_VERSION_CODE — for kernel version checks if needed */

#include "balloon_ioctl.h"     /* our shared ioctl definitions (the kernel↔userspace contract) */

/*
 * MODULE METADATA — every kernel module must declare these.
 * GPL license is REQUIRED to access many kernel APIs (enforced by EXPORT_SYMBOL_GPL).
 * These show up when you run: modinfo balloon_kmod.ko
 */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shriya");
MODULE_DESCRIPTION("Memory Balloon Driver — kernel module version");
MODULE_VERSION("1.0");

/* ════════════════════════════════════════════════════════════════
 * MODULE PARAMETERS
 *
 * These are like command-line args but for kernel modules.
 * Set at load time: sudo insmod balloon_kmod.ko max_pages=500
 *
 * In guest.c, max_pages came from reading the host's config.
 * Here, it's set directly via module_param() at insmod time.
 * You can check its value at /sys/module/balloon_kmod/parameters/max_pages
 *
 * 0444 = read-only in sysfs after loading (r--r--r--)
 * ════════════════════════════════════════════════════════════════ */

static int max_pages = 200;   /* default: 200 pages = 800KB balloon capacity */
module_param(max_pages, int, 0444);
MODULE_PARM_DESC(max_pages, "Maximum balloon size in pages (1 page = 4KB)");

/* ════════════════════════════════════════════════════════════════
 * INTERNAL STATE
 *
 * In guest.c we had:
 *   static void **pages_held = NULL;  — flat array of malloc'd pointers
 *   static int page_count = 0;        — how many pages we're holding
 *
 * Here we use a kernel linked list instead. Each node holds a pointer
 * to a `struct page` — the kernel's representation of a physical page.
 *
 * WHY A LINKED LIST?
 *   - No need to pre-allocate a fixed-size array
 *   - O(1) add/remove from either end
 *   - This is EXACTLY how the real virtio_balloon tracks pages
 *   - list_head is used everywhere in the kernel (task list, inode list, etc.)
 * ════════════════════════════════════════════════════════════════ */

/*
 * Each page in the balloon gets one of these tracking structs.
 * struct page is the kernel's metadata for a physical page frame —
 * it contains the reference count, flags, mapping info, etc.
 * We get one from alloc_page() and return it with __free_page().
 */
struct balloon_page {
    struct page     *page;    /* pointer to the actual kernel page (from alloc_page) */
    struct list_head list;    /* linked list node — connects to other balloon_page structs */
};

/*
 * LIST_HEAD creates an empty doubly-linked list (statically initialized).
 * This is the head node — balloon_page structs attach here.
 * In guest.c this was: static void **pages_held = NULL;
 */
static LIST_HEAD(balloon_pages);

/*
 * DEFINE_MUTEX creates a mutual exclusion lock.
 *
 * WHY DO WE NEED A MUTEX?
 * In guest.c, everything was single-threaded. But in the kernel, multiple
 * things can call our ioctl simultaneously (two host_kmod processes, or a
 * /proc read during an ioctl). The mutex prevents race conditions.
 *
 * We use a mutex (NOT a spinlock) because alloc_page(GFP_KERNEL) can SLEEP
 * (it might wait for memory). Spinlocks can't be held while sleeping.
 */
static DEFINE_MUTEX(balloon_lock);

/* same counters as guest.c's globals */
static int current_pages   = 0;    /* how many pages balloon holds right now */
static int peak_pages      = 0;    /* highest balloon size ever reached */
static int total_inflated  = 0;    /* lifetime count of pages grabbed */
static int total_deflated  = 0;    /* lifetime count of pages freed */

/* ════════════════════════════════════════════════════════════════
 * PRESSURE DETECTION (REAL, NOT SIMULATED!)
 *
 * In guest.c, simulate_pressure() used random dice rolls:
 *   int roll = rand() % 100;
 *   if (pct_full > 80 && roll < 10) pressure = CRITICAL;
 * That was fake — nothing to do with actual system memory.
 *
 * Here we call si_meminfo(), which fills a struct sysinfo with the
 * kernel's REAL memory stats. This is the same data behind /proc/meminfo.
 *
 * So if the balloon eats too much memory (or you run a stress test),
 * the pressure level will GENUINELY change because the system is
 * actually running low on free pages. No more dice rolls!
 * ════════════════════════════════════════════════════════════════ */

static int detect_real_pressure(void)
{
    struct sysinfo si;        /* kernel struct: holds memory + swap + uptime stats */
    unsigned long free_pct;   /* percentage of total RAM that is free */

    si_meminfo(&si);          /* fills `si` with real memory stats from the kernel */
    /* si.freeram  = number of free pages (in PAGE_SIZE units, NOT bytes) */
    /* si.totalram = total physical RAM pages */

    if (si.totalram == 0) return 0;   /* avoid divide-by-zero */

    free_pct = (si.freeram * 100) / si.totalram;   /* what % of RAM is free? */

    /*
     * Thresholds inspired by Linux's own memory watermarks:
     *   < 10% free → CRITICAL (like the "min" watermark, OOM danger zone)
     *   < 25% free → LOW      (like the "low" watermark, reclamation starts)
     *   >= 25%     → NONE     (system is healthy)
     * The real virtio_balloon uses PSI (Pressure Stall Information) or
     * shrinker callbacks. si_meminfo is simpler but still reflects real state.
     */
    if (free_pct < 10)
        return 2;  /* CRITICAL — system dangerously low on memory */
    else if (free_pct < 25)
        return 1;  /* LOW — moderate memory pressure */

    return 0;      /* NONE — plenty of free memory */
}

/* ════════════════════════════════════════════════════════════════
 * INFLATE: GRAB PAGES FROM THE KERNEL'S BUDDY ALLOCATOR
 *
 * This is the kernel equivalent of do_inflate() in guest.c.
 *
 * In guest.c we did:
 *   void *page = malloc(PAGE_SIZE_SIM);      // fake: userspace heap
 *   memset(page, 0xAB, PAGE_SIZE_SIM);       // touch to force backing
 *   pages_held[page_count++] = page;          // track in flat array
 *
 * Here we do:
 *   struct page *pg = alloc_page(GFP_KERNEL); // real: buddy allocator
 *   void *addr = page_address(pg);            // get kernel virtual address
 *   memset(addr, 0xAB, PAGE_SIZE);            // touch it (same reason)
 *   list_add(&bp->list, &balloon_pages);      // track in linked list
 *
 * alloc_page(GFP_KERNEL) does the following inside the kernel:
 *   1. Checks the per-CPU page cache (fast path)
 *   2. Falls back to the buddy allocator if cache is empty
 *   3. Finds a free page frame in physical memory
 *   4. Returns a struct page* pointing to that frame's metadata
 *   5. The page is REMOVED from the free list — nobody else can use it
 *
 * GFP_KERNEL = "Get Free Page, Kernel context" meaning:
 *   - We're in process context (not an interrupt handler)
 *   - We CAN sleep if needed (allocator may wait for pages)
 *   - If memory is tight, it may trigger reclamation or even OOM killer
 * ════════════════════════════════════════════════════════════════ */

static int balloon_inflate(int target)
{
    int grabbed = 0;   /* how many pages we grabbed this round */

    mutex_lock(&balloon_lock);   /* acquire lock before touching shared state */

    /* clamp target to max (same as guest.c: if (target > max_pages) target = max_pages) */
    if (target > max_pages)
        target = max_pages;

    /* keep grabbing pages until we reach the target (same loop as guest.c) */
    while (current_pages < target) {
        struct page *pg;           /* kernel's representation of a physical page frame */
        struct balloon_page *bp;   /* our tracking struct (links page into our list) */
        void *addr;                /* kernel virtual address of the page (for memset) */

        /*
         * alloc_page(GFP_KERNEL) — THE KEY LINE
         * This replaces malloc(PAGE_SIZE_SIM) from guest.c.
         * It grabs a REAL 4KB physical page from the buddy allocator.
         * Returns NULL if the system is completely out of memory.
         */
        pg = alloc_page(GFP_KERNEL);
        if (!pg) {
            /* same as guest.c: vm_log("malloc failed — host asking for too much!\n") */
            pr_warn("balloon: alloc_page() failed at %d pages — system is out of memory!\n",
                    current_pages);
            break;
        }

        /*
         * kmalloc — kernel's malloc for small objects
         * Allocates from the slab allocator (NOT buddy allocator).
         * We need this to track which pages we hold.
         * If it fails, we MUST __free_page the page we just got (no GC in kernel!).
         */
        bp = kmalloc(sizeof(*bp), GFP_KERNEL);
        if (!bp) {
            __free_page(pg);   /* don't leak the page! */
            pr_warn("balloon: kmalloc failed for tracking struct\n");
            break;
        }

        /*
         * page_address(pg) — converts struct page* to kernel virtual address
         * struct page is just metadata. To read/write the page's contents,
         * we need its virtual address in the kernel's direct memory mapping.
         *
         * memset 0xAB forces the MMU to actually back it with a physical frame.
         * Same reason as guest.c: memset(page, 0xAB, PAGE_SIZE_SIM);
         * PAGE_SIZE is the real kernel constant = 4096 on x86/x64
         */
        addr = page_address(pg);
        memset(addr, 0xAB, PAGE_SIZE);

        /*
         * list_add() — insert at HEAD of our linked list (LIFO / stack push)
         * In guest.c this was: pages_held[page_count++] = page;
         */
        bp->page = pg;
        list_add(&bp->list, &balloon_pages);
        current_pages++;
        grabbed++;
    }

    total_inflated += grabbed;
    if (current_pages > peak_pages)
        peak_pages = current_pages;

    mutex_unlock(&balloon_lock);

    pr_info("balloon: INFLATE → now holding %d pages (%dKB), grabbed %d this round\n",
            current_pages, current_pages * 4, grabbed);

    return current_pages;
}

/* ════════════════════════════════════════════════════════════════
 * DEFLATE: RETURN PAGES TO THE KERNEL'S BUDDY ALLOCATOR
 *
 * Kernel equivalent of do_deflate() in guest.c.
 *
 * In guest.c:   free(pages_held[--page_count]);  // return to userspace heap
 * Here:         __free_page(bp->page);            // return to buddy allocator
 *
 * __free_page() puts the page back on the buddy allocator's free list.
 * Any process or kernel subsystem can now use it. This is how a
 * hypervisor "gives memory back" to a VM after deflation.
 * ════════════════════════════════════════════════════════════════ */

static int balloon_deflate(int target)
{
    int freed = 0;   /* how many pages freed this round */

    mutex_lock(&balloon_lock);

    if (target < 0)
        target = 0;   /* can't go below zero (same as guest.c) */

    while (current_pages > target && !list_empty(&balloon_pages)) {
        struct balloon_page *bp;

        /*
         * list_first_entry() — get the first node from our linked list
         * Equivalent to pages_held[--page_count] in guest.c.
         * Args: list head, struct type, field name of the list_head member.
         */
        bp = list_first_entry(&balloon_pages, struct balloon_page, list);

        list_del(&bp->list);       /* unlink from list BEFORE freeing */

        /*
         * __free_page(bp->page) — THE KEY LINE
         * Returns this physical page to the buddy allocator's free list.
         * After this, the page can be allocated by anyone in the system.
         * This replaces free(pages_held[page_count]) from guest.c.
         */
        __free_page(bp->page);

        /*
         * kfree(bp) — free our tracking struct (allocated via kmalloc).
         * Every kmalloc MUST have a matching kfree. No garbage collector!
         */
        kfree(bp);

        current_pages--;
        freed++;
    }

    total_deflated += freed;

    mutex_unlock(&balloon_lock);

    pr_info("balloon: DEFLATE → now holding %d pages (%dKB), freed %d this round\n",
            current_pages, current_pages * 4, freed);

    return current_pages;
}

/* ════════════════════════════════════════════════════════════════
 * /proc/balloon — HUMAN-READABLE STATUS FILE
 *
 * Creates a virtual file at /proc/balloon that anyone can read:
 *   cat /proc/balloon              (one-shot read)
 *   watch -n1 cat /proc/balloon    (live updates every second)
 *
 * /proc is a VIRTUAL filesystem — files don't exist on disk.
 * When you `cat /proc/balloon`, the kernel calls our balloon_proc_show()
 * function which writes the output on-the-fly. This is the same mechanism
 * behind /proc/meminfo, /proc/cpuinfo, etc.
 *
 * In the userspace version, the monitor read shared memory files.
 * Here, the simplest monitoring is just: cat /proc/balloon
 *
 * We use the seq_file API for proper output buffering.
 * seq_printf works like printf but writes to a kernel buffer.
 * ════════════════════════════════════════════════════════════════ */

/* called when someone reads /proc/balloon */
static int balloon_proc_show(struct seq_file *m, void *v)
{
    struct sysinfo si;
    int pressure;

    (void)v;           /* unused parameter, required by seq_file API */
    si_meminfo(&si);   /* get real system memory stats */

    mutex_lock(&balloon_lock);
    pressure = detect_real_pressure();

    /* pretty box (same visual style as the userspace monitor) */
    seq_printf(m, "╔══════════════════════════════════════╗\n");
    seq_printf(m, "║   KERNEL BALLOON STATUS              ║\n");
    seq_printf(m, "╠══════════════════════════════════════╣\n");
    seq_printf(m, "║  Current pages:  %-6d (%dKB)      ║\n",
               current_pages, current_pages * 4);
    seq_printf(m, "║  Peak pages:     %-6d              ║\n", peak_pages);
    seq_printf(m, "║  Max pages:      %-6d (%dKB)      ║\n",
               max_pages, max_pages * 4);
    seq_printf(m, "║  Total inflated: %-6d              ║\n", total_inflated);
    seq_printf(m, "║  Total deflated: %-6d              ║\n", total_deflated);
    seq_printf(m, "╠══════════════════════════════════════╣\n");
    /* si.freeram is in PAGES, multiply by PAGE_SIZE/1024 to get KB */
    seq_printf(m, "║  System free:    %-8lu KB         ║\n",
               si.freeram * (unsigned long)(PAGE_SIZE / 1024));
    seq_printf(m, "║  System total:   %-8lu KB         ║\n",
               si.totalram * (unsigned long)(PAGE_SIZE / 1024));
    seq_printf(m, "║  Pressure:       %-8s             ║\n",
               pressure == 2 ? "CRITICAL" :
               pressure == 1 ? "LOW" : "NONE");
    seq_printf(m, "╚══════════════════════════════════════╝\n");

    mutex_unlock(&balloon_lock);
    return 0;   /* 0 = success */
}

/* called when someone opens /proc/balloon — boilerplate for seq_file */
static int balloon_proc_open(struct inode *inode, struct file *file)
{
    (void)inode;   /* unused */
    return single_open(file, balloon_proc_show, NULL);
}

/*
 * proc_ops — file operations for /proc/balloon
 * Tells the kernel what to call when someone opens/reads/closes this file.
 * On kernels 5.6+, /proc uses struct proc_ops (not struct file_operations).
 */
static const struct proc_ops balloon_proc_ops = {
    .proc_open    = balloon_proc_open,    /* our open handler */
    .proc_read    = seq_read,             /* standard seq_file read (handles buffering) */
    .proc_lseek   = seq_lseek,            /* standard seek */
    .proc_release = single_release,       /* standard cleanup for single_open */
};

/* ════════════════════════════════════════════════════════════════
 * /dev/balloon_ctl — IOCTL CHARACTER DEVICE
 *
 * The main control interface. The host daemon opens this and sends commands:
 *   fd = open("/dev/balloon_ctl", O_RDWR);
 *   ioctl(fd, BALLOON_IOC_INFLATE, &cmd);   // inflate
 *   ioctl(fd, BALLOON_IOC_DEFLATE, &cmd);   // deflate
 *   ioctl(fd, BALLOON_IOC_GET_STATUS, &st); // read status
 *   ioctl(fd, BALLOON_IOC_RESET, 0);        // full reset
 *
 * This replaces the shared memory command channel from guest.c:
 *   Old: shm->command = CMD_INFLATE;   →  ioctl(fd, BALLOON_IOC_INFLATE, &cmd)
 *   Old: shm->target_pages = 100;      →  cmd.target_pages = 100
 *
 * ioctl is how real kernel drivers expose control interfaces.
 * VirtualBox guest additions, GPU drivers, USB drivers all use ioctl.
 * ════════════════════════════════════════════════════════════════ */

static long balloon_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct balloon_cmd bcmd;       /* holds target_pages from userspace */
    struct balloon_status bst;     /* status struct we send back */
    struct sysinfo si;             /* for real memory stats */

    (void)f;   /* we don't need the file struct for our ioctl */

    switch (cmd) {

    case BALLOON_IOC_INFLATE:
        /*
         * copy_from_user() — safely copy data from userspace into kernel space.
         *
         * WHY CAN'T WE JUST DEREFERENCE arg?
         * `arg` is a userspace pointer. In the kernel we can't do
         * bcmd = *(struct balloon_cmd *)arg  because:
         *   1. The pointer might be invalid (NULL, unmapped)
         *   2. It might point to kernel memory (security hole!)
         *   3. The page might be swapped out
         *
         * copy_from_user handles all this safely:
         *   - Validates pointer is in userspace range
         *   - Handles page faults gracefully
         *   - Returns 0 on success, non-zero on failure
         *
         * -EFAULT = "bad address" (the standard error for bad pointers)
         */
        if (copy_from_user(&bcmd, (void __user *)arg, sizeof(bcmd)))
            return -EFAULT;
        pr_info("balloon: host requested inflate to %d pages\n", bcmd.target_pages);
        balloon_inflate(bcmd.target_pages);
        return 0;

    case BALLOON_IOC_DEFLATE:
        /* same pattern: safely copy command from userspace, then act */
        if (copy_from_user(&bcmd, (void __user *)arg, sizeof(bcmd)))
            return -EFAULT;
        pr_info("balloon: host requested deflate to %d pages\n", bcmd.target_pages);
        balloon_deflate(bcmd.target_pages);
        return 0;

    case BALLOON_IOC_GET_STATUS:
        /*
         * Fill status struct with current state + real memory info,
         * then copy it back to userspace via copy_to_user().
         * This replaces reading shared memory: pages = vms[i]->current_pages;
         */
        si_meminfo(&si);   /* get real system memory stats */
        mutex_lock(&balloon_lock);
        bst.current_pages  = current_pages;
        bst.peak_pages     = peak_pages;
        bst.max_pages      = max_pages;
        bst.total_inflated = total_inflated;
        bst.total_deflated = total_deflated;
        bst.pressure       = detect_real_pressure();   /* REAL, not simulated! */
        bst.free_kb        = si.freeram * (unsigned long)(PAGE_SIZE / 1024);
        bst.total_kb       = si.totalram * (unsigned long)(PAGE_SIZE / 1024);
        mutex_unlock(&balloon_lock);

        /* copy_to_user — the reverse: safely copy kernel data to userspace */
        if (copy_to_user((void __user *)arg, &bst, sizeof(bst)))
            return -EFAULT;
        return 0;

    case BALLOON_IOC_RESET:
        /* deflate everything and zero all counters */
        pr_info("balloon: host requested full reset\n");
        balloon_deflate(0);   /* free all held pages */
        mutex_lock(&balloon_lock);
        peak_pages     = 0;
        total_inflated = 0;
        total_deflated = 0;
        mutex_unlock(&balloon_lock);
        return 0;

    default:
        /* -ENOTTY = "not a typewriter" — standard error for unrecognized ioctl.
         * Yes, the name is historical and confusing, but it's the correct code. */
        return -ENOTTY;
    }
}

/*
 * file_operations — tells the kernel what our device file supports.
 * We only implement unlocked_ioctl (no read/write/mmap needed).
 * .owner = THIS_MODULE prevents unloading while the device is open.
 */
static const struct file_operations balloon_fops = {
    .owner          = THIS_MODULE,      /* prevents unload while device is open */
    .unlocked_ioctl = balloon_ioctl,    /* our ioctl handler */
};

/*
 * MISC DEVICE — simplified character device registration.
 * Instead of manually allocating a major number + creating a class + device,
 * misc_register() does it all in one call:
 *   - Allocates minor number under major 10 (the misc major)
 *   - Creates /dev/balloon_ctl automatically (via udev)
 * MISC_DYNAMIC_MINOR = "pick any free minor number for me"
 * .mode = 0666 = rw-rw-rw- (anyone can open it)
 */
static struct miscdevice balloon_misc = {
    .minor = MISC_DYNAMIC_MINOR,    /* let kernel pick a minor number */
    .name  = "balloon_ctl",         /* creates /dev/balloon_ctl */
    .fops  = &balloon_fops,         /* our file operations */
    .mode  = 0666,                  /* permissions: world-accessible */
};

/* ════════════════════════════════════════════════════════════════
 * MODULE INIT AND EXIT
 *
 * __init — marks balloon_init as initialization-only. The kernel can
 * reclaim its memory after the module finishes loading (runs once).
 *
 * __exit — marks balloon_exit as cleanup-only. If the module were
 * built into the kernel (not loadable), this function would be omitted.
 *
 * module_init(balloon_init) → "call this on insmod"
 * module_exit(balloon_exit) → "call this on rmmod"
 * ════════════════════════════════════════════════════════════════ */

static struct proc_dir_entry *proc_entry;   /* handle for /proc/balloon */

static int __init balloon_init(void)
{
    int ret;

    /* startup banner — visible in dmesg after insmod */
    pr_info("balloon: ═══════════════════════════════════\n");
    pr_info("balloon: Loading Memory Balloon Driver\n");
    pr_info("balloon: Max pages: %d (%dKB)\n", max_pages, max_pages * 4);
    pr_info("balloon: Page size: %lu bytes\n", PAGE_SIZE);
    pr_info("balloon: ═══════════════════════════════════\n");

    /* validate module parameter */
    if (max_pages < 1 || max_pages > 65536) {
        pr_err("balloon: max_pages must be between 1 and 65536\n");
        return -EINVAL;   /* -EINVAL = "invalid argument" — module won't load */
    }

    /*
     * Register /dev/balloon_ctl
     * After this, the device file exists and userspace can open() it.
     * If it fails, we return an error and the module doesn't load.
     */
    ret = misc_register(&balloon_misc);
    if (ret) {
        pr_err("balloon: failed to register misc device: %d\n", ret);
        return ret;
    }
    pr_info("balloon: /dev/balloon_ctl created\n");

    /*
     * Register /proc/balloon
     * proc_create() args:
     *   "balloon"          — filename  (→ /proc/balloon)
     *   0444               — permissions (r--r--r-- = read-only for all)
     *   NULL               — parent dir (NULL = /proc/ itself)
     *   &balloon_proc_ops  — our file operations
     *
     * If this fails, we MUST undo misc_register (no RAII in C!).
     */
    proc_entry = proc_create("balloon", 0444, NULL, &balloon_proc_ops);
    if (!proc_entry) {
        pr_err("balloon: failed to create /proc/balloon\n");
        misc_deregister(&balloon_misc);   /* undo misc_register */
        return -ENOMEM;
    }
    pr_info("balloon: /proc/balloon created\n");

    pr_info("balloon: Ready! Use host_kmod or ioctl to send commands.\n");
    return 0;   /* 0 = success, module is now loaded & active */
}

static void __exit balloon_exit(void)
{
    pr_info("balloon: Unloading — deflating all pages...\n");

    /*
     * Free ALL held pages before the module goes away.
     * If we skip this, those pages are LEAKED forever until reboot.
     * The kernel has no garbage collector!
     */
    balloon_deflate(0);

    /* remove /proc/balloon — after this, cat /proc/balloon returns "No such file" */
    if (proc_entry)
        proc_remove(proc_entry);

    /* remove /dev/balloon_ctl — after this, open("/dev/balloon_ctl") fails */
    misc_deregister(&balloon_misc);

    /* final stats, same as guest.c's cleanup() */
    pr_info("balloon: Final stats: inflated %d, deflated %d, peak %d\n",
            total_inflated, total_deflated, peak_pages);
    pr_info("balloon: Unloaded cleanly. Goodbye!\n");
}

/*
 * Tell the kernel which functions to call on insmod / rmmod.
 * These macros expand to special ELF section attributes that
 * the module loader reads when processing the .ko file.
 */
module_init(balloon_init);
module_exit(balloon_exit);
