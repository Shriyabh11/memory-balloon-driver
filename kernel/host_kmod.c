/*
 * host_kmod.c — Userspace Host Daemon for the Kernel Balloon Module
 *
 * This is the kernel-level version of host.c. Instead of communicating
 * through shared memory files in /tmp/, it talks to the balloon kernel
 * module via ioctl() on /dev/balloon_ctl.
 *
 * HOW IT WORKS:
 *   1. Open /dev/balloon_ctl (created by the balloon_kmod.ko module)
 *   2. Send commands via ioctl():
 *      - ioctl(fd, BALLOON_IOC_INFLATE, &cmd)   → tells module to grab pages
 *      - ioctl(fd, BALLOON_IOC_DEFLATE, &cmd)   → tells module to free pages
 *      - ioctl(fd, BALLOON_IOC_GET_STATUS, &st)  → reads current state
 *   3. The kernel module does the actual alloc_page/free_page work
 *
 * WHAT CHANGED FROM host.c:
 *   shm->command = CMD_INFLATE        →  ioctl(fd, BALLOON_IOC_INFLATE, &cmd)
 *   shm->target_pages = 100           →  cmd.target_pages = 100
 *   reading shm->current_pages        →  ioctl(fd, BALLOON_IOC_GET_STATUS, &st)
 *   open("/tmp/balloon_vm1", ...)     →  open("/dev/balloon_ctl", O_RDWR)
 *   mmap(MAP_SHARED)                  →  ioctl() (no shared memory needed)
 *
 * The policy engine is the same: inflate in steps, check pressure, deflate
 * if the system is stressed, rebalance every 4 cycles.
 *
 * Build:  gcc -Wall -Wextra -g host_kmod.c -o host_kmod
 * Run:    sudo ./host_kmod           (after insmod balloon_kmod.ko)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "balloon_ioctl.h"

static int dev_fd    = -1;
static int running   = 1;

/* config — set interactively at startup */
static int inflate_step = 25;
static int loop_delay   = 3;

/* ── timestamped logging ── */
static void host_log(const char *fmt, ...)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    printf("[%02d:%02d:%02d HOST] ",
           t->tm_hour, t->tm_min, t->tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/* ── safe integer input ── */
static int read_int(const char *prompt, int min, int max)
{
    int value;
    while (1) {
        printf("%s (%d–%d): ", prompt, min, max);
        fflush(stdout);
        if (scanf("%d", &value) == 1) {
            while (getchar() != '\n');
            if (value >= min && value <= max) return value;
            printf("  Out of range, try again.\n");
        } else {
            while (getchar() != '\n');
            printf("  Not a number, try again.\n");
        }
    }
}

/*
 * get_status — ask the kernel module for current balloon state.
 *
 * In host.c this was: reading vms[i]->current_pages from shared memory.
 * Now we call ioctl(fd, BALLOON_IOC_GET_STATUS, &st) which triggers
 * the kernel module's balloon_ioctl handler, which fills in the struct
 * using copy_to_user() and returns it to us.
 *
 * The status includes REAL system memory info (free_kb, total_kb)
 * from the kernel's si_meminfo(), not simulated values.
 */
static int get_status(struct balloon_status *st)
{
    if (ioctl(dev_fd, BALLOON_IOC_GET_STATUS, st) == -1) {
        perror("ioctl GET_STATUS");
        return -1;
    }
    return 0;
}

/*
 * send_inflate — tell the kernel module to inflate to a target page count.
 *
 * In host.c this was:
 *   shm->target_pages = new_target;
 *   shm->command = CMD_INFLATE;
 * Now we pack target_pages into a struct and send it via ioctl.
 * The kernel module receives it via copy_from_user() and calls alloc_page().
 */
static int send_inflate(int target)
{
    struct balloon_cmd cmd = { .target_pages = target };
    if (ioctl(dev_fd, BALLOON_IOC_INFLATE, &cmd) == -1) {
        perror("ioctl INFLATE");
        return -1;
    }
    return 0;
}

/*
 * send_deflate — tell the kernel module to deflate to a target page count.
 *
 * In host.c this was:
 *   shm->target_pages = target;
 *   shm->command = CMD_DEFLATE;
 * The kernel module calls __free_page() for each page it releases.
 */
static int send_deflate(int target)
{
    struct balloon_cmd cmd = { .target_pages = target };
    if (ioctl(dev_fd, BALLOON_IOC_DEFLATE, &cmd) == -1) {
        perror("ioctl DEFLATE");
        return -1;
    }
    return 0;
}


/* ── print the dashboard ── */
static void print_dashboard(int cycle, struct balloon_status *st)
{
    int pct = (st->max_pages > 0) ? (st->current_pages * 30 / st->max_pages) : 0;

    /* progress bar, 30 chars wide */
    char bar[31];
    for (int j = 0; j < 30; j++)
        bar[j] = (j < pct) ? '#' : '.';
    bar[30] = '\0';

    const char *p_str = "\033[32m OK \033[0m";
    if (st->pressure == 1) p_str = "\033[33m LOW\033[0m";
    if (st->pressure == 2) p_str = "\033[31mCRIT\033[0m";

    printf("\n");
    printf("┌──────────────────────────────────────────────────────────┐\n");
    printf("│  KERNEL BALLOON DASHBOARD — CYCLE %-4d                   │\n", cycle);
    printf("├──────────────────────────────────────────────────────────┤\n");
    printf("│  [%s] %4d / %4d pages                │\n",
           bar, st->current_pages, st->max_pages);
    printf("│  Balloon:    %5dKB held                                │\n",
           st->current_pages * 4);
    printf("│  Peak:       %5d pages                                 │\n",
           st->peak_pages);
    printf("│  Lifetime:   +%d / -%d pages                             │\n",
           st->total_inflated, st->total_deflated);
    printf("│  Pressure:   %s                                         │\n",
           p_str);
    printf("├──────────────────────────────────────────────────────────┤\n");
    printf("│  System free:  %8lu KB  (%lu MB)                      │\n",
           st->free_kb, st->free_kb / 1024);
    printf("│  System total: %8lu KB  (%lu MB)                      │\n",
           st->total_kb, st->total_kb / 1024);
    printf("└──────────────────────────────────────────────────────────┘\n\n");
}

/* ── signal handler ── */
static void handle_signal(int s) { (void)s; running = 0; }

/* ── interactive setup ── */
static void ask_config(void)
{
    printf("\n");
    printf("╔══════════════════════════════════════════╗\n");
    printf("║   KERNEL BALLOON HOST DAEMON             ║\n");
    printf("╚══════════════════════════════════════════╝\n\n");

    /* get current kernel module config */
    struct balloon_status st;
    if (get_status(&st) == -1) {
        printf("ERROR: Cannot talk to kernel module.\n");
        printf("Did you run: sudo insmod balloon_kmod.ko ?\n");
        exit(1);
    }

    printf("Connected to kernel module!\n");
    printf("  Module max_pages: %d (%dKB)\n", st.max_pages, st.max_pages * 4);
    printf("  Current balloon:  %d pages\n", st.current_pages);
    printf("  System memory:    %luMB free / %luMB total\n\n",
           st.free_kb / 1024, st.total_kb / 1024);

    printf("─── Inflate step size? ───────────────────\n");
    printf("Pages to grab per inflate cycle.\n");
    int step_max = st.max_pages / 2;
    if (step_max < 1) step_max = 1;
    inflate_step = read_int("Step (pages)", 1, step_max);
    printf("  %d pages per inflate = %dKB\n\n", inflate_step, inflate_step * 4);

    printf("─── Seconds between host decisions? ──────\n");
    printf("1 = fast, 5 = slow and easy to follow.\n");
    loop_delay = read_int("Delay (sec)", 1, 10);
    printf("  %d second(s)\n\n", loop_delay);

    printf("╔══════════════════════════════════════════╗\n");
    printf("║  Max balloon:  %-4d pages (%4dKB)       ║\n",
           st.max_pages, st.max_pages * 4);
    printf("║  Step:         %-4d pages (%4dKB)       ║\n",
           inflate_step, inflate_step * 4);
    printf("║  Delay:        %-2d sec                   ║\n", loop_delay);
    printf("╚══════════════════════════════════════════╝\n\n");

    host_log("Starting policy loop...\n\n");
}

/* ── main ── */
int main(void)
{
    signal(SIGINT, handle_signal);   /* Ctrl+C triggers graceful shutdown */

    /*
     * Open the kernel module's character device.
     *
     * In host.c we did: open("/tmp/balloon_vm1", O_RDWR) + mmap(...)
     * Now we just: open("/dev/balloon_ctl", O_RDWR)
     *
     * /dev/balloon_ctl was created by balloon_kmod.ko when it called
     * misc_register(). If the module isn't loaded, this file won't exist
     * and open() will fail with ENOENT.
     */
    dev_fd = open("/dev/balloon_ctl", O_RDWR);
    if (dev_fd == -1) {
        printf("\n");
        printf("ERROR: Cannot open /dev/balloon_ctl: %s\n", strerror(errno));
        printf("\n");
        printf("Make sure the kernel module is loaded:\n");
        printf("  sudo insmod balloon_kmod.ko\n");
        printf("  sudo insmod balloon_kmod.ko max_pages=500  (custom)\n\n");
        return 1;
    }

    ask_config();

    int cycle = 0;

    while (running) {
        cycle++;

        struct balloon_status st;
        if (get_status(&st) == -1) {
            host_log("Lost connection to kernel module!\n");
            break;
        }

        printf("══════════════════════════════════════════\n");
        host_log("Cycle %d\n", cycle);
        printf("══════════════════════════════════════════\n\n");

        /*
         * POLICY ENGINE — same priorities as host.c:
         *
         * Priority 1: If the kernel reports real memory pressure, deflate.
         *   In host.c we checked shm->pressure (which was simulated)
         *   Here pressure comes from si_meminfo() in the kernel (REAL).
         *
         * Priority 2: If no pressure, inflate by one step.
         *   Same as host.c's inflate_vm() but via ioctl instead of
         *   writing to shared memory.
         */

        /* priority 1: relieve pressure (REAL pressure from si_meminfo!) */
        if (st.pressure == 2) {
            host_log("\033[31m⚠ CRITICAL pressure — full deflate!\033[0m\n");
            send_deflate(0);   /* ioctl → kernel calls __free_page for all pages */
        } else if (st.pressure == 1) {
            host_log("\033[33mLow pressure — partial deflate\033[0m\n");
            int target = st.current_pages / 2;
            send_deflate(target);
        }
        /* priority 2: inflate by one step */
        else if (st.current_pages < st.max_pages) {
            int new_target = st.current_pages + inflate_step;
            if (new_target > st.max_pages) new_target = st.max_pages;

            host_log("Inflating: %d → %d pages\n", st.current_pages, new_target);
            send_inflate(new_target);   /* ioctl → kernel calls alloc_page for each */
        } else {
            host_log("Balloon at max capacity (%d pages)\n", st.max_pages);
        }

        /* refresh status after action (ioctl reads updated counters) */
        get_status(&st);
        print_dashboard(cycle, &st);

        /* periodic rebalance: every 4 cycles, trim 25% (same as host.c) */
        if (cycle % 4 == 0 && st.current_pages > 0) {
            host_log("Periodic rebalance — trimming 25%%\n");
            int target = st.current_pages * 3 / 4;
            send_deflate(target);
            get_status(&st);
            print_dashboard(cycle, &st);
        }

        sleep(loop_delay);
    }

    /* cleanup */
    printf("\n");
    host_log("Shutting down — deflating balloon...\n");
    send_deflate(0);

    struct balloon_status final_st;
    if (get_status(&final_st) == 0) {
        host_log("═══ FINAL STATS ═══\n");
        host_log("  Total inflated: %d pages\n", final_st.total_inflated);
        host_log("  Total deflated: %d pages\n", final_st.total_deflated);
        host_log("  Peak:           %d pages\n", final_st.peak_pages);
    }

    close(dev_fd);
    host_log("Done!\n");
    return 0;
}
