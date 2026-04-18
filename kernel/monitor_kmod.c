/*
 * monitor_kmod.c — Read-only monitor for the kernel balloon module
 *
 * Reads /proc/balloon every second and displays a live dashboard.
 * This is the kernel-level version of monitor.c — instead of reading
 * shared memory files, it reads from the kernel's /proc interface.
 *
 * Build:  gcc -Wall -Wextra -g monitor_kmod.c -o monitor_kmod
 * Run:    ./monitor_kmod   (after insmod balloon_kmod.ko)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>

#include "balloon_ioctl.h"

static int running = 1;
static void handle_signal(int s) { (void)s; running = 0; }

/* Read status via ioctl (more reliable than parsing /proc text) */
static int get_status(int fd, struct balloon_status *st)
{
    if (ioctl(fd, BALLOON_IOC_GET_STATUS, st) == -1)
        return -1;
    return 0;
}

int main(void)
{
    signal(SIGINT, handle_signal);

    int dev_fd = open("/dev/balloon_ctl", O_RDWR);
    if (dev_fd == -1) {
        printf("\nCan't open /dev/balloon_ctl: %s\n", strerror(errno));
        printf("Load the module first: sudo insmod balloon_kmod.ko\n\n");
        return 1;
    }

    int tick = 0;

    while (running) {
        tick++;

        struct balloon_status st;
        if (get_status(dev_fd, &st) == -1) {
            printf("Lost connection to kernel module!\n");
            break;
        }

        time_t now = time(NULL);
        struct tm *t = localtime(&now);

        /* progress bar — 30 chars */
        int filled = (st.max_pages > 0) ? (st.current_pages * 30 / st.max_pages) : 0;
        char bar[31];
        for (int j = 0; j < 30; j++)
            bar[j] = (j < filled) ? '#' : '.';
        bar[30] = '\0';

        /* pressure string */
        const char *p_str;
        const char *p_color;
        if (st.pressure == 2) {
            p_str   = "CRITICAL";
            p_color = "\033[31m";
        } else if (st.pressure == 1) {
            p_str   = "LOW";
            p_color = "\033[33m";
        } else {
            p_str   = "OK";
            p_color = "\033[32m";
        }

        /* balloon fill percentage */
        int fill_pct = (st.max_pages > 0) ? (st.current_pages * 100 / st.max_pages) : 0;

        /* system memory percentage */
        int sys_used_pct = (st.total_kb > 0)
            ? (int)(((st.total_kb - st.free_kb) * 100) / st.total_kb)
            : 0;

        /* clear screen */
        printf("\033[2J\033[H");

        printf("╔══════════════════════════════════════════════════════════════╗\n");
        printf("║  KERNEL BALLOON MONITOR   %02d:%02d:%02d   tick #%-4d              ║\n",
               t->tm_hour, t->tm_min, t->tm_sec, tick);
        printf("╠══════════════════════════════════════════════════════════════╣\n");
        printf("║                                                              ║\n");
        printf("║  Status:     %s%-8s\033[0m                                      ║\n",
               p_color, p_str);
        printf("║  Balloon:    [%s] %3d%%               ║\n",
               bar, fill_pct);
        printf("║  Pages:      %4d / %4d  (%dKB / %dKB)                     ║\n",
               st.current_pages, st.max_pages,
               st.current_pages * 4, st.max_pages * 4);
        printf("║  Peak:       %4d pages                                      ║\n",
               st.peak_pages);
        printf("║  Lifetime:   +%-5d / -%-5d pages                            ║\n",
               st.total_inflated, st.total_deflated);
        printf("║                                                              ║\n");
        printf("╠══════════════════════════════════════════════════════════════╣\n");
        printf("║  SYSTEM MEMORY                                               ║\n");
        printf("║  Free:       %8lu KB  (%5lu MB)                          ║\n",
               st.free_kb, st.free_kb / 1024);
        printf("║  Total:      %8lu KB  (%5lu MB)                          ║\n",
               st.total_kb, st.total_kb / 1024);
        printf("║  Used:       %3d%%                                            ║\n",
               sys_used_pct);
        printf("║                                                              ║\n");
        printf("╠══════════════════════════════════════════════════════════════╣\n");
        printf("║  KERNEL INTERFACES                                           ║\n");
        printf("║    /dev/balloon_ctl    ioctl device (host commands)           ║\n");
        printf("║    /proc/balloon       proc status  (cat /proc/balloon)      ║\n");
        printf("║    dmesg               kernel logs  (sudo dmesg -w)          ║\n");
        printf("╚══════════════════════════════════════════════════════════════╝\n");
        printf("\n  Press Ctrl+C to exit monitor.\n");

        sleep(1);
    }

    close(dev_fd);
    printf("\nMonitor stopped.\n");
    return 0;
}
