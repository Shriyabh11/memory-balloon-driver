/*
 * guest_driver.c — The Guest (The Worker)
 * ─────────────────────────────────────────
 * Pretends to be a virtual machine.
 * Reads user config from /balloon_config (host wrote it).
 * Obeys inflate/deflate commands from its whiteboard.
 *
 * compile:  gcc guest_driver.c -o guest
 * run:      ./guest 1    (or 2, 3 ... up to num_vms)
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

/* ── globals ── */
static void                 **pages_held = NULL;  /* dynamic array of pages  */
static int                    page_count = 0;
static struct balloon_shm    *shm        = NULL;
static struct balloon_config *cfg        = NULL;
static int                    shm_fd     = -1;
static int                    cfg_fd     = -1;
static int                    my_vm_id   = 0;
static int                    running    = 1;

/* These get filled from config shared memory */
static int MAX_PAGES    = 0;
static int INFLATE_STEP = 0;

/* ─────────────────────────────────────────────────────
   FUNCTION: read_config
   Opens /balloon_config and reads user's settings.
   Waits until host has written config_ready = 1.
   ───────────────────────────────────────────────────── */
void read_config(void)
{
    printf("[VM%d] Reading config from host...\n", my_vm_id);

    /* Open the config shared memory the host created */
    cfg_fd = shm_open(SHM_CONFIG_NAME, O_RDONLY, 0666);
    if (cfg_fd == -1) {
        printf("[VM%d] ERROR: Cannot find /balloon_config\n", my_vm_id);
        printf("[VM%d] Make sure host is running first!\n", my_vm_id);
        exit(1);
    }

    cfg = mmap(NULL, sizeof(struct balloon_config),
               PROT_READ, MAP_SHARED, cfg_fd, 0);
    if (cfg == MAP_FAILED) { perror("mmap config"); exit(1); }

    /* Wait for host to finish writing config */
    while (cfg->config_ready != 1) {
        printf("[VM%d] Waiting for host config...\r", my_vm_id);
        fflush(stdout);
        sleep(1);
    }

    /* Read user's settings into our local variables */
    MAX_PAGES    = cfg->max_pages;
    INFLATE_STEP = cfg->inflate_step;

    printf("[VM%d] Config received!\n", my_vm_id);
    printf("[VM%d]   Max pages:    %d (%dKB)\n",
        my_vm_id, MAX_PAGES, MAX_PAGES * (PAGE_SIZE_SIM/1024));
    printf("[VM%d]   Inflate step: %d (%dKB)\n\n",
        my_vm_id, INFLATE_STEP, INFLATE_STEP * (PAGE_SIZE_SIM/1024));

    /* Allocate our page pointer array based on user's max */
    pages_held = calloc(MAX_PAGES, sizeof(void *));
    if (!pages_held) {
        printf("[VM%d] ERROR: Cannot allocate page array\n", my_vm_id);
        exit(1);
    }
}

/* ─────────────────────────────────────────────────────
   FUNCTION: open_vm_shm
   Opens this VM's whiteboard (host created it).
   ───────────────────────────────────────────────────── */
void open_vm_shm(void)
{
    char name[32];
    snprintf(name, sizeof(name), "%s%d", SHM_NAME_PREFIX, my_vm_id);

    printf("[VM%d] Opening whiteboard: %s\n", my_vm_id, name);

    /* Retry up to 10 times — host might still be setting up */
    for (int attempt = 0; attempt < 10; attempt++) {
        shm_fd = shm_open(name, O_RDWR, 0666);
        if (shm_fd != -1) break;
        printf("[VM%d] Waiting for host to create whiteboard... (%d/10)\n",
            my_vm_id, attempt+1);
        sleep(1);
    }

    if (shm_fd == -1) {
        printf("[VM%d] ERROR: Host never created %s\n", my_vm_id, name);
        printf("[VM%d] Is the host running?\n", my_vm_id);
        exit(1);
    }

    shm = mmap(NULL, sizeof(struct balloon_shm),
               PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm == MAP_FAILED) { perror("mmap vm"); exit(1); }

    printf("[VM%d] Connected to whiteboard!\n\n", my_vm_id);
}

/* ─────────────────────────────────────────────────────
   FUNCTION: do_inflate
   Grabs pages via malloc() until we hit target.
   Simulates alloc_page() from real Linux kernel.
   ───────────────────────────────────────────────────── */
void do_inflate(int target)
{
    /* Safety cap at user's chosen max */
    if (target > MAX_PAGES) target = MAX_PAGES;

    printf("[VM%d] INFLATE: %d → %d pages (%dKB → %dKB)\n",
        my_vm_id,
        page_count, target,
        page_count * (PAGE_SIZE_SIM/1024),
        target     * (PAGE_SIZE_SIM/1024));

    /*
     * Grab pages one at a time.
     * malloc(4096) = alloc_page() in real kernel
     * We memset to prove it's real memory, not lazy-allocated.
     */
    while (page_count < target) {
        void *page = malloc(PAGE_SIZE_SIM);
        if (!page) {
            printf("[VM%d] WARNING: malloc failed at page %d!\n",
                my_vm_id, page_count);
            break;
        }
        memset(page, 0xAB, PAGE_SIZE_SIM);   /* touch it — make it real  */
        pages_held[page_count++] = page;
        shm->current_pages = page_count;     /* live update to whiteboard */
    }

    printf("[VM%d] INFLATE done. Holding %d pages (%dKB)\n",
        my_vm_id, page_count, page_count * (PAGE_SIZE_SIM/1024));

    shm->command = CMD_IDLE;   /* tell host: done, ready for next command  */
}

/* ─────────────────────────────────────────────────────
   FUNCTION: do_deflate
   Frees all pages via free().
   Simulates __free_page() from real Linux kernel.
   ───────────────────────────────────────────────────── */
void do_deflate(void)
{
    printf("[VM%d] DEFLATE: freeing %d pages (%dKB)...\n",
        my_vm_id, page_count, page_count * (PAGE_SIZE_SIM/1024));

    for (int i = 0; i < page_count; i++) {
        free(pages_held[i]);       /* return page to OS — like __free_page() */
        pages_held[i] = NULL;
        shm->current_pages = page_count - i - 1;   /* live update           */
    }

    page_count         = 0;
    shm->current_pages = 0;
    shm->pressure      = PRESSURE_NONE;
    shm->command       = CMD_IDLE;

    printf("[VM%d] DEFLATE done. Balloon empty!\n", my_vm_id);
}

/* ─────────────────────────────────────────────────────
   FUNCTION: simulate_pressure
   Randomly signals memory pressure to host.
   Simulates Linux shrinker callbacks.
   ───────────────────────────────────────────────────── */
void simulate_pressure(void)
{
    /*
     * Only trigger pressure if balloon is at least half full.
     * No point begging to deflate if we're nearly empty anyway.
     */
    if (page_count < MAX_PAGES / 2) {
        shm->pressure = PRESSURE_NONE;
        return;
    }

    int roll = rand() % 100;

    if (roll < 5) {
        /* 5% chance of CRITICAL */
        if (shm->pressure != PRESSURE_CRITICAL)
            printf("[VM%d] PRESSURE: CRITICAL! Need memory NOW!\n", my_vm_id);
        shm->pressure = PRESSURE_CRITICAL;

    } else if (roll < 15) {
        /* 10% chance of LOW */
        if (shm->pressure == PRESSURE_NONE)
            printf("[VM%d] Pressure: LOW. Getting tight...\n", my_vm_id);
        shm->pressure = PRESSURE_LOW;

    } else {
        shm->pressure = PRESSURE_NONE;
    }
}

/* ─────────────────────────────────────────────────────
   SIGNAL + CLEANUP
   ───────────────────────────────────────────────────── */
void handle_signal(int s) { (void)s; running = 0; }

void cleanup(void)
{
    printf("\n[VM%d] Cleaning up...\n", my_vm_id);

    /* Free all held pages */
    for (int i = 0; i < page_count; i++) {
        if (pages_held[i]) free(pages_held[i]);
    }
    free(pages_held);

    /* Update whiteboard */
    if (shm) {
        shm->guest_ready   = 0;
        shm->current_pages = 0;
        shm->pressure      = PRESSURE_NONE;
        munmap(shm, sizeof(struct balloon_shm));
    }
    if (shm_fd > 0) close(shm_fd);
    if (cfg)        munmap(cfg, sizeof(struct balloon_config));
    if (cfg_fd > 0) close(cfg_fd);

    printf("[VM%d] Done!\n", my_vm_id);
}

/* ─────────────────────────────────────────────────────
   MAIN
   ───────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    /* Get VM ID from command line */
    if (argc != 2) {
        printf("Usage: ./guest <vm_id>\n");
        printf("Example: ./guest 1\n");
        return 1;
    }

    my_vm_id = atoi(argv[1]);
    if (my_vm_id < 1 || my_vm_id > MAX_VMS_LIMIT) {
        printf("VM ID must be 1 to %d\n", MAX_VMS_LIMIT);
        return 1;
    }

    printf("╔══════════════════════════════════════╗\n");
    printf("║   MEMORY BALLOON GUEST DRIVER v2.0   ║\n");
    printf("║   VM ID: %-2d                          ║\n", my_vm_id);
    printf("╚══════════════════════════════════════╝\n\n");

    srand(time(NULL) + my_vm_id);
    signal(SIGINT, handle_signal);

    /* Read user config that host wrote */
    read_config();

    /* Open this VM's whiteboard */
    open_vm_shm();

    /* Write our VM ID and tell host we're ready */
    shm->vm_id       = my_vm_id;
    shm->current_pages = 0;
    shm->pressure    = PRESSURE_NONE;
    shm->guest_ready = 1;   /* HOST IS WAITING FOR THIS! */

    printf("[VM%d] Ready! Listening for commands...\n\n", my_vm_id);

    /* Main loop — read and obey commands */
    while (running) {
        int cmd = shm->command;

        if (cmd == CMD_INFLATE) {
            do_inflate(shm->target_pages);
        } else if (cmd == CMD_DEFLATE) {
            do_deflate();
        } else {
            /* Idle — simulate pressure randomly */
            simulate_pressure();
            sleep(1);
        }
    }

    cleanup();
    return 0;
}