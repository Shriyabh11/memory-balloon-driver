/*
 * monitor.c — Read-only dashboard that watches all balloon VMs.
 *
 * Doesn't send any commands — just reads the shared memory
 * regions and prints a live view. Useful when you have multiple
 * terminals running and want one clean overview.
 *
 * Build:  gcc -Wall -Wextra -g monitor.c -o monitor -lrt
 * Run:    ./monitor   (after host is running)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <signal.h>
#include <time.h>
#include "shared_mem.h"

static struct balloon_config *cfg  = NULL;
static struct balloon_shm    *vms[MAX_VMS_LIMIT];
static int running = 1;

void handle_signal(int s) { (void)s; running = 0; }

/* pressure level to a short colored string */
const char *pressure_str(int p)
{
    switch (p) {
        case PRESSURE_CRITICAL: return "\033[31mCRIT\033[0m";
        case PRESSURE_LOW:      return "\033[33mLOW \033[0m";
        default:                return "\033[32m OK \033[0m";
    }
}

/* state of guest_ready to a string */
const char *status_str(int ready)
{
    return ready ? "\033[32mONLINE \033[0m" : "\033[31mOFFLINE\033[0m";
}

int main(void)
{
    signal(SIGINT, handle_signal);

    printf("\n");
    printf("╔══════════════════════════════════════╗\n");
    printf("║   BALLOON MONITOR (read-only)        ║\n");
    printf("╚══════════════════════════════════════╝\n\n");

    /* open config region */
    int cfg_fd = shm_open(SHM_CONFIG_NAME, O_RDONLY, 0666);
    if (cfg_fd == -1) {
        printf("Can't find %s — start the host first.\n", SHM_CONFIG_NAME);
        return 1;
    }

    cfg = mmap(NULL, sizeof(struct balloon_config),
               PROT_READ, MAP_SHARED, cfg_fd, 0);
    if (cfg == MAP_FAILED) { perror("mmap config"); return 1; }

    if (cfg->config_ready != 1) {
        printf("Host config not ready yet, waiting...\n");
        while (cfg->config_ready != 1 && running) sleep(1);
        if (!running) return 0;
    }

    int num_vms   = cfg->num_vms;
    int max_pages = cfg->max_pages;

    printf("Config: %d VMs, max %d pages/VM, step %d, delay %ds\n\n",
           num_vms, max_pages, cfg->inflate_step, cfg->loop_delay);

    /* open each VM's shared memory (read-only) */
    for (int i = 0; i < num_vms; i++) {
        char name[64];
        snprintf(name, sizeof(name), "%s%d", SHM_NAME_PREFIX, i + 1);

        int fd = shm_open(name, O_RDONLY, 0666);
        if (fd == -1) {
            printf("Can't open %s — host may not have created it yet\n", name);
            vms[i] = NULL;
            continue;
        }

        vms[i] = mmap(NULL, sizeof(struct balloon_shm),
                       PROT_READ, MAP_SHARED, fd, 0);
        if (vms[i] == MAP_FAILED) {
            vms[i] = NULL;
            close(fd);
        }
    }

    /* live monitoring loop — refresh every second */
    int tick = 0;
    while (running) {
        tick++;

        /* get current time */
        time_t now = time(NULL);
        struct tm *t = localtime(&now);

        /* clear screen for a clean refresh */
        printf("\033[2J\033[H");

        printf("╔══════════════════════════════════════════════════════════════╗\n");
        printf("║  BALLOON MONITOR   %02d:%02d:%02d   tick #%-4d                    ║\n",
               t->tm_hour, t->tm_min, t->tm_sec, tick);
        printf("╠══════════════════════════════════════════════════════════════╣\n");

        int grand_total = 0;
        int grand_peak  = 0;

        for (int i = 0; i < num_vms; i++) {
            if (!vms[i]) {
                printf("║  VM%-2d   [not mapped]                                       ║\n", i + 1);
                continue;
            }

            int pages    = vms[i]->current_pages;
            int peak     = vms[i]->peak_pages;
            int pressure = vms[i]->pressure;
            int ready    = vms[i]->guest_ready;
            int cmd      = vms[i]->command;
            int t_inf    = vms[i]->total_inflated;
            int t_def    = vms[i]->total_deflated;

            grand_total += pages;
            if (peak > grand_peak) grand_peak = peak;

            /* progress bar — 30 chars wide */
            int filled = (max_pages > 0) ? (pages * 30 / max_pages) : 0;
            char bar[31];
            for (int j = 0; j < 30; j++)
                bar[j] = (j < filled) ? '#' : '.';
            bar[30] = '\0';

            /* what command is active? */
            const char *cmd_str = "idle";
            if (cmd == CMD_INFLATE) cmd_str = "INFLATE";
            if (cmd == CMD_DEFLATE) cmd_str = "DEFLATE";

            printf("║                                                              ║\n");
            printf("║  VM%-2d  %s  %s                                  ║\n",
                   i + 1, status_str(ready), pressure_str(pressure));
            printf("║    [%s] %4d/%4d pages                  ║\n",
                   bar, pages, max_pages);
            printf("║    %5dKB held | peak %d | cmd: %-7s                      ║\n",
                   pages * (PAGE_SIZE_SIM / 1024), peak, cmd_str);
            printf("║    lifetime: +%d / -%d pages                                  ║\n",
                   t_inf, t_def);
        }

        printf("║                                                              ║\n");
        printf("╠══════════════════════════════════════════════════════════════╣\n");
        printf("║  TOTAL: %5d pages  (%7dKB)   all-time peak: %d          ║\n",
               grand_total,
               grand_total * (PAGE_SIZE_SIM / 1024),
               grand_peak);
        printf("╚══════════════════════════════════════════════════════════════╝\n");
        printf("\n  Press Ctrl+C to exit monitor.\n");

        sleep(1);
    }

    printf("\nMonitor stopped.\n");
    return 0;
}
