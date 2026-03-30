/*
 * guest.c — The Guest Driver (runs inside the "VM")
 *
 * Simulates a virtio-balloon guest driver. Reads config from
 * the host, then sits in a loop obeying inflate/deflate commands.
 *
 * Build:  gcc -Wall -Wextra -g guest.c -o guest -lrt
 * Run:    ./guest <vm_id>    (e.g. ./guest 1)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <signal.h>
#include <time.h>
#include "shared_mem.h"

/* ── globals ── */
static void                **pages_held = NULL;
static int                   page_count = 0;
static struct balloon_shm   *shm        = NULL;
static struct balloon_config *cfg       = NULL;
static int                   shm_fd     = -1;
static int                   cfg_fd     = -1;
static int                   my_vm_id   = 0;
static int                   running    = 1;

/* pulled from host's config region */
static int max_pages    = 0;
static int inflate_step = 0;

/* local stats */
static int stat_total_inflated = 0;
static int stat_total_deflated = 0;
static int stat_peak_pages     = 0;

/* ── timestamped logging ── */
static void vm_log(const char *fmt, ...)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    printf("[%02d:%02d:%02d VM%d] ",
           t->tm_hour, t->tm_min, t->tm_sec, my_vm_id);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/* ── read config written by host ── */
void read_config(void)
{
    vm_log("Looking for host config...\n");

    cfg_fd = shm_open(SHM_CONFIG_NAME, O_RDONLY, 0666);
    if (cfg_fd == -1) {
        vm_log("Can't find %s — is the host running?\n", SHM_CONFIG_NAME);
        exit(1);
    }

    cfg = mmap(NULL, sizeof(struct balloon_config),
               PROT_READ, MAP_SHARED, cfg_fd, 0);
    if (cfg == MAP_FAILED) { perror("mmap config"); exit(1); }

    /* spin until host signals config_ready */
    while (cfg->config_ready != 1) {
        printf("\r");
        vm_log("Waiting for host to finish config...");
        fflush(stdout);
        sleep(1);
    }
    printf("\n");

    max_pages    = cfg->max_pages;
    inflate_step = cfg->inflate_step;

    vm_log("Config loaded — max %d pages (%dKB), step %d\n",
           max_pages, max_pages * (PAGE_SIZE_SIM / 1024), inflate_step);

    pages_held = calloc(max_pages, sizeof(void *));
    if (!pages_held) {
        vm_log("Failed to allocate page tracking array\n");
        exit(1);
    }
}

/* ── connect to this VM's shared memory region ── */
void open_vm_shm(void)
{
    char name[64];
    snprintf(name, sizeof(name), "%s%d", SHM_NAME_PREFIX, my_vm_id);

    vm_log("Connecting to %s\n", name);

    for (int try = 0; try < 10; try++) {
        shm_fd = shm_open(name, O_RDWR, 0666);
        if (shm_fd != -1) break;
        vm_log("Host hasn't created it yet, retrying (%d/10)...\n", try + 1);
        sleep(1);
    }

    if (shm_fd == -1) {
        vm_log("Gave up waiting for %s\n", name);
        exit(1);
    }

    shm = mmap(NULL, sizeof(struct balloon_shm),
               PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm == MAP_FAILED) { perror("mmap vm"); exit(1); }

    vm_log("Connected!\n");
}

/* ── update peak tracking ── */
static void update_peak(void)
{
    if (page_count > stat_peak_pages) {
        stat_peak_pages = page_count;
        shm->peak_pages = stat_peak_pages;
    }
}

/*
 * Inflate — grab pages until we reach the target.
 *
 * Each malloc(PAGE_SIZE_SIM) simulates what alloc_page() does
 * in the real Linux kernel: take one 4KB page away from the
 * guest's free list and hand it to the balloon.
 *
 * We memset the page so the OS actually backs it with physical
 * memory (otherwise it's just a lazy virtual mapping).
 */
void do_inflate(int target)
{
    if (target > max_pages) target = max_pages;

    vm_log("INFLATE %d → %d pages\n", page_count, target);

    int grabbed = 0;
    while (page_count < target) {
        void *page = malloc(PAGE_SIZE_SIM);
        if (!page) {
            vm_log("malloc failed — host is asking for too much!\n");
            break;
        }
        memset(page, 0xAB, PAGE_SIZE_SIM);
        pages_held[page_count++] = page;
        grabbed++;

        /* update shared state so host sees progress in real time */
        shm->current_pages = page_count;
    }

    stat_total_inflated += grabbed;
    shm->total_inflated = stat_total_inflated;
    update_peak();

    vm_log("Done — holding %d pages (%dKB), grabbed %d this round\n",
           page_count, page_count * (PAGE_SIZE_SIM / 1024), grabbed);

    shm->command = CMD_IDLE;
}

/*
 * Deflate — free pages down to the target.
 *
 * Each free() simulates __free_page() in the real kernel:
 * return a page from the balloon back to the guest's free list
 * so the guest can use that memory again.
 *
 * Unlike the old version that always freed everything, this
 * supports partial deflation — the host can ask us to shrink
 * to any target, not just zero.
 */
void do_deflate(int target)
{
    if (target < 0) target = 0;

    vm_log("DEFLATE %d → %d pages\n", page_count, target);

    int freed = 0;
    while (page_count > target) {
        page_count--;
        free(pages_held[page_count]);
        pages_held[page_count] = NULL;
        freed++;
        shm->current_pages = page_count;
    }

    stat_total_deflated += freed;
    shm->total_deflated = stat_total_deflated;

    if (page_count == 0)
        shm->pressure = PRESSURE_NONE;

    vm_log("Done — holding %d pages, freed %d this round\n", page_count, freed);

    shm->command = CMD_IDLE;
}

/*
 * Simulate memory pressure — in a real VM this would come
 * from Linux's shrinker callbacks or /proc/meminfo.
 *
 * We only fake pressure when the balloon is taking up a
 * significant chunk of memory. The probabilities are:
 *   balloon > 80% full → 10% chance critical, 20% low
 *   balloon > 50% full → 5% chance critical, 10% low
 */
void simulate_pressure(void)
{
    if (max_pages == 0 || page_count == 0) {
        shm->pressure = PRESSURE_NONE;
        return;
    }

    int pct_full = (page_count * 100) / max_pages;
    int roll = rand() % 100;

    if (pct_full > 80) {
        if (roll < 10)      shm->pressure = PRESSURE_CRITICAL;
        else if (roll < 30) shm->pressure = PRESSURE_LOW;
        else                shm->pressure = PRESSURE_NONE;
    } else if (pct_full > 50) {
        if (roll < 5)       shm->pressure = PRESSURE_CRITICAL;
        else if (roll < 15) shm->pressure = PRESSURE_LOW;
        else                shm->pressure = PRESSURE_NONE;
    } else {
        shm->pressure = PRESSURE_NONE;
    }

    if (shm->pressure == PRESSURE_CRITICAL)
        vm_log("⚠ CRITICAL pressure! Balloon at %d%%, need memory back!\n", pct_full);
    else if (shm->pressure == PRESSURE_LOW)
        vm_log("Pressure: low (balloon at %d%%)\n", pct_full);
}

/* ── signal + cleanup ── */
void handle_signal(int s) { (void)s; running = 0; }

void cleanup(void)
{
    printf("\n");
    vm_log("Shutting down...\n");

    for (int i = 0; i < page_count; i++) {
        if (pages_held[i]) free(pages_held[i]);
    }
    free(pages_held);

    vm_log("Stats: inflated %d pages, deflated %d, peak %d\n",
           stat_total_inflated, stat_total_deflated, stat_peak_pages);

    if (shm) {
        shm->guest_ready   = 0;
        shm->current_pages = 0;
        shm->pressure      = PRESSURE_NONE;
        munmap(shm, sizeof(struct balloon_shm));
    }
    if (shm_fd > 0) close(shm_fd);
    if (cfg)        munmap(cfg, sizeof(struct balloon_config));
    if (cfg_fd > 0) close(cfg_fd);

    vm_log("Bye!\n");
}

/* ── main ── */
int main(int argc, char *argv[])
{
    if (argc != 2) {
        printf("Usage: ./guest <vm_id>\n");
        printf("  e.g: ./guest 1\n");
        return 1;
    }

    my_vm_id = atoi(argv[1]);
    if (my_vm_id < 1 || my_vm_id > MAX_VMS_LIMIT) {
        printf("VM ID must be between 1 and %d\n", MAX_VMS_LIMIT);
        return 1;
    }

    printf("\n");
    printf("╔══════════════════════════════════════╗\n");
    printf("║   BALLOON GUEST DRIVER               ║\n");
    printf("║   VM #%-2d                             ║\n", my_vm_id);
    printf("╚══════════════════════════════════════╝\n\n");

    srand(time(NULL) + my_vm_id);
    signal(SIGINT, handle_signal);

    read_config();
    open_vm_shm();

    /* tell the host we're alive */
    shm->vm_id         = my_vm_id;
    shm->current_pages = 0;
    shm->pressure      = PRESSURE_NONE;
    shm->guest_ready   = 1;

    vm_log("Ready — waiting for commands...\n\n");

    while (running) {
        int cmd = shm->command;

        if (cmd == CMD_INFLATE) {
            do_inflate(shm->target_pages);
        } else if (cmd == CMD_DEFLATE) {
            do_deflate(shm->target_pages);
        } else {
            simulate_pressure();
            usleep(500000);  /* check twice per second */
        }
    }

    cleanup();
    return 0;
}