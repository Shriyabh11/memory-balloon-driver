#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <signal.h>
#include "shared_mem.h"

//defining user lvl variables
//static as we dont wanna them changed/visible in another file or if variable with same name in another files
//is present, linker will throw error

static int num_vms;
static int max_pages;
static int inflate_step;
static int loop_delay;

int read_int(const char *prompt, int min, int max)
{
    int value;
    while (1) {
        printf("%s (%d to %d): ", prompt, min, max);
        fflush(stdout);
 
        /* scanf reads one integer from keyboard */
        if (scanf("%d", &value) == 1) {
            /* clear leftover newline from input buffer */
            while (getchar() != '\n');
 
            if (value >= min && value <= max) {
                return value;   /* valid! return it */
            }
            printf("  Please enter a number between %d and %d\n", min, max);
        } else {
            /* user typed letters instead of a number */
            while (getchar() != '\n');
            printf("  Invalid input. Please type a number.\n");
        }
    }
}

void ask_user_config(void)
{
    printf("╔══════════════════════════════════════════╗\n");
    printf("║   MEMORY BALLOON DRIVER — SETUP          ║\n");
    printf("╚══════════════════════════════════════════╝\n\n");
 
    printf("Let's configure your simulation!\n");
    printf("(Press Enter after each number)\n\n");
 
    /* ── Question 1: Number of VMs ── */
    printf("─── Question 1 ───────────────────────────\n");
    printf("How many Virtual Machines do you want?\n");
    printf("(Each VM runs as a separate ./guest process)\n");
    NUM_VMS = read_int("Number of VMs", 1, MAX_VMS_LIMIT);
    printf("  ✓ Will simulate %d VM(s)\n\n", NUM_VMS);
 
    /* ── Question 2: Max balloon size ── */
    printf("─── Question 2 ───────────────────────────\n");
    printf("Max balloon size per VM (in pages)?\n");
    printf("(Each page = 4KB, so 100 pages = 400KB)\n");
    printf("Tip: try 200 for a quick demo, 800 for a big one\n");
    MAX_PAGES = read_int("Max pages per VM", 10, MAX_PAGES_LIMIT);
    printf("  ✓ Max balloon = %d pages = %dKB per VM\n\n",
        MAX_PAGES, MAX_PAGES * (PAGE_SIZE_SIM / 1024));
 
    /* ── Question 3: Inflate step ── */
    printf("─── Question 3 ───────────────────────────\n");
    printf("How many pages to grab per inflate step?\n");
    printf("(Smaller = slower and smoother, Bigger = faster)\n");
    printf("Tip: try 25 for smooth, 100 for fast\n");
    int max_step = MAX_PAGES / 2;
    if (max_step < 1) max_step = 1;
    INFLATE_STEP = read_int("Inflate step (pages)", 1, max_step);
    printf("  ✓ Each inflate grabs %d pages = %dKB\n\n",
        INFLATE_STEP, INFLATE_STEP * (PAGE_SIZE_SIM / 1024));
 
    /* ── Question 4: Loop delay ── */
    printf("─── Question 4 ───────────────────────────\n");
    printf("How many seconds between each host decision?\n");
    printf("(1 = fast demo, 5 = slow and readable)\n");
    LOOP_DELAY = read_int("Loop delay (seconds)", 1, 10);
    printf("  ✓ Host will decide every %d second(s)\n\n", LOOP_DELAY);
 
    /* ── Summary ── */
    printf("╔══════════════════════════════════════════╗\n");
    printf("║   YOUR CONFIGURATION                     ║\n");
    printf("╠══════════════════════════════════════════╣\n");
    printf("║  VMs:           %2d                       ║\n", NUM_VMS);
    printf("║  Max balloon:   %4d pages = %4dKB       ║\n",
        MAX_PAGES, MAX_PAGES * (PAGE_SIZE_SIM / 1024));
    printf("║  Inflate step:  %4d pages = %4dKB       ║\n",
        INFLATE_STEP, INFLATE_STEP * (PAGE_SIZE_SIM / 1024));
    printf("║  Loop delay:    %2d sec                   ║\n", LOOP_DELAY);
    printf("║  Total max:     %4dKB across all VMs    ║\n",
        NUM_VMS * MAX_PAGES * (PAGE_SIZE_SIM / 1024));
    printf("╚══════════════════════════════════════════╝\n\n");
 
    /* ── Write config to shared memory so guests can read it ── */
 
    /* Create /balloon_config shared memory */
    cfg_fd = shm_open(SHM_CONFIG_NAME, O_CREAT | O_RDWR, 0666);
    if (cfg_fd == -1) { perror("shm_open config"); exit(1); }
 
    ftruncate(cfg_fd, sizeof(struct balloon_config));
 
    cfg = mmap(NULL, sizeof(struct balloon_config),
               PROT_READ | PROT_WRITE, MAP_SHARED, cfg_fd, 0);
    if (cfg == MAP_FAILED) { perror("mmap config"); exit(1); }
 
    /* Write all user choices into shared config */
    cfg->num_vms       = NUM_VMS;
    cfg->max_pages     = MAX_PAGES;
    cfg->inflate_step  = INFLATE_STEP;
    cfg->loop_delay    = LOOP_DELAY;
    cfg->config_ready  = 1;   /* signal to guests: config is written!    */
 
    printf("[HOST] Config written to shared memory.\n");
    printf("[HOST] Guests can now read /balloon_config\n\n");
}
 
/* ─────────────────────────────────────────────────────
   FUNCTION: create_vm_shm
   Creates the whiteboard for one VM.
   ───────────────────────────────────────────────────── */
struct balloon_shm *create_vm_shm(int vm_num, int *fd_out)
{
    /* Build the name: "/balloon_vm1", "/balloon_vm2" etc */
    char name[32];
    snprintf(name, sizeof(name), "%s%d", SHM_NAME_PREFIX, vm_num);
 
    int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    if (fd == -1) { perror("shm_open vm"); exit(1); }
 
    ftruncate(fd, sizeof(struct balloon_shm));
 
    struct balloon_shm *shm = mmap(NULL, sizeof(struct balloon_shm),
                                   PROT_READ | PROT_WRITE,
                                   MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) { perror("mmap vm"); exit(1); }
 
    /* Zero out — sets command=CMD_IDLE, guest_ready=0 etc */
    memset(shm, 0, sizeof(struct balloon_shm));
 
    *fd_out = fd;
    return shm;
}
 
/* ─────────────────────────────────────────────────────
   FUNCTION: wait_for_guests
   Waits until all N guests have written guest_ready=1
   ───────────────────────────────────────────────────── */
void wait_for_guests(void)
{
    printf("[HOST] Waiting for %d guest(s) to connect...\n", NUM_VMS);
    printf("[HOST] Open %d new terminal(s) and run:\n", NUM_VMS);
 
    for (int i = 1; i <= NUM_VMS; i++) {
        printf("         ./guest %d\n", i);
    }
    printf("\n");
 
    while (1) {
        int ready = 0;
        for (int i = 0; i < NUM_VMS; i++) {
            if (vms[i]->guest_ready == 1) ready++;
        }
        if (ready == NUM_VMS) break;
        printf("[HOST] %d/%d guests ready...\r", ready, NUM_VMS);
        fflush(stdout);
        sleep(1);
    }
    printf("\n[HOST] All %d guests ready! Starting now.\n\n", NUM_VMS);
}
 
/* ─────────────────────────────────────────────────────
   FUNCTION: find_idlest_vm
   Returns index of VM holding fewest pages.
   That VM gets inflated next — it has the most room.
   ───────────────────────────────────────────────────── */
int find_idlest_vm(void)
{
    int idlest = 0;
    for (int i = 1; i < NUM_VMS; i++) {
        if (vms[i]->current_pages < vms[idlest]->current_pages) {
            idlest = i;
        }
    }
    return idlest;
}
 
/* ─────────────────────────────────────────────────────
   FUNCTION: find_pressured_vm
   Returns index of VM with highest pressure, or -1.
   ───────────────────────────────────────────────────── */
int find_pressured_vm(void)
{
    int worst = -1, worst_p = PRESSURE_NONE;
    for (int i = 0; i < NUM_VMS; i++) {
        if (vms[i]->pressure > worst_p) {
            worst_p = vms[i]->pressure;
            worst   = i;
        }
    }
    return worst;
}
 
/* ─────────────────────────────────────────────────────
   FUNCTION: inflate_vm
   Tells one VM to grow its balloon by INFLATE_STEP.
   ───────────────────────────────────────────────────── */
void inflate_vm(int idx)
{
    struct balloon_shm *vm = vms[idx];
    int vm_num = idx + 1;
 
    int new_target = vm->current_pages + INFLATE_STEP;
 
    /* Never inflate past user's chosen max */
    if (new_target > MAX_PAGES) {
        printf("[HOST] VM%d at max (%d pages), skipping inflate\n",
            vm_num, MAX_PAGES);
        return;
    }
 
    printf("[HOST] Inflating VM%d: %d → %d pages (%dKB → %dKB)\n",
        vm_num,
        vm->current_pages, new_target,
        vm->current_pages * (PAGE_SIZE_SIM/1024),
        new_target        * (PAGE_SIZE_SIM/1024));
 
    /* Write command to whiteboard */
    vm->target_pages = new_target;
    vm->command      = CMD_INFLATE;
 
    /* Wait for guest to respond (up to 10 seconds) */
    for (int t = 0; t < 10 && vm->current_pages < new_target; t++) {
        sleep(1);
    }
 
    printf("[HOST] VM%d now holds %d pages (%dKB)\n\n",
        vm_num,
        vm->current_pages,
        vm->current_pages * (PAGE_SIZE_SIM/1024));
}
 
/* ─────────────────────────────────────────────────────
   FUNCTION: deflate_vm
   Tells one VM to empty its balloon completely.
   ───────────────────────────────────────────────────── */
void deflate_vm(int idx)
{
    struct balloon_shm *vm = vms[idx];
    int vm_num = idx + 1;
 
    if (vm->current_pages == 0) {
        printf("[HOST] VM%d already empty\n", vm_num);
        return;
    }
 
    int had = vm->current_pages;
 
    printf("[HOST] Deflating VM%d (%d pages, pressure=%d)\n",
        vm_num, had, vm->pressure);
 
    vm->command      = CMD_DEFLATE;
    vm->target_pages = 0;
 
    /* Wait for guest to finish freeing */
    for (int t = 0; t < 10 && vm->current_pages > 0; t++) {
        sleep(1);
    }
 
    printf("[HOST] VM%d empty! Reclaimed %dKB\n\n",
        vm_num, had * (PAGE_SIZE_SIM/1024));
}
 
/* ─────────────────────────────────────────────────────
   FUNCTION: print_dashboard
   Prints a live summary of all VMs.
   ───────────────────────────────────────────────────── */
void print_dashboard(int cycle)
{
    int total = 0;
    for (int i = 0; i < NUM_VMS; i++) total += vms[i]->current_pages;
 
    printf("┌─────────────────────────────────────────────────┐\n");
    printf("│  BALLOON DRIVER — CYCLE %-3d                     │\n", cycle);
    printf("│  Config: %d VMs | max %d pages | step %d          │\n",
        NUM_VMS, MAX_PAGES, INFLATE_STEP);
    printf("├─────────────────────────────────────────────────┤\n");
 
    for (int i = 0; i < NUM_VMS; i++) {
        int pages    = vms[i]->current_pages;
        int kb       = pages * (PAGE_SIZE_SIM/1024);
        int pct      = (MAX_PAGES > 0) ? (pages * 20 / MAX_PAGES) : 0;
        int pressure = vms[i]->pressure;
 
        /* Build bar: 20 chars wide */
        char bar[21];
        for (int j = 0; j < 20; j++) bar[j] = (j < pct) ? '#' : '.';
        bar[20] = '\0';
 
        const char *p_str = "OK  ";
        if (pressure == PRESSURE_LOW)      p_str = "LOW ";
        if (pressure == PRESSURE_CRITICAL) p_str = "CRIT";
 
        printf("│ VM%-2d [%s] %4dp %5dKB  %s │\n",
            i+1, bar, pages, kb, p_str);
    }
 
    printf("├─────────────────────────────────────────────────┤\n");
    printf("│ Total reclaimed: %4d pages = %6dKB           │\n",
        total, total * (PAGE_SIZE_SIM/1024));
    printf("└─────────────────────────────────────────────────┘\n\n");
}
 
/* ─────────────────────────────────────────────────────
   SIGNAL HANDLER + CLEANUP
   ───────────────────────────────────────────────────── */
static int running = 1;
void handle_signal(int s) { (void)s; running = 0; }
 
void cleanup(void)
{
    printf("\n[HOST] Cleaning up...\n");
 
    /* Deflate all VMs */
    for (int i = 0; i < NUM_VMS; i++) {
        if (vms[i] && vms[i]->guest_ready) {
            vms[i]->command = CMD_DEFLATE;
        }
    }
    sleep(2);
 
    /* Unmap and unlink all VM shared memory */
    for (int i = 0; i < NUM_VMS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "%s%d", SHM_NAME_PREFIX, i+1);
        if (vms[i])      munmap(vms[i], sizeof(struct balloon_shm));
        if (shm_fds[i])  close(shm_fds[i]);
        shm_unlink(name);
    }
 
    /* Unmap and unlink config */
    if (cfg)    munmap(cfg, sizeof(struct balloon_config));
    if (cfg_fd) close(cfg_fd);
    shm_unlink(SHM_CONFIG_NAME);
 
    printf("[HOST] Done!\n");
}
 
/* ─────────────────────────────────────────────────────
   MAIN
   ───────────────────────────────────────────────────── */
int main(void)
{
    signal(SIGINT, handle_signal);
 
    /* Step 1: Ask user for config */
    ask_user_config();
 
    /* Step 2: Create VM whiteboards */
    printf("[HOST] Creating %d shared memory region(s)...\n", NUM_VMS);
    for (int i = 0; i < NUM_VMS; i++) {
        vms[i] = create_vm_shm(i+1, &shm_fds[i]);
        printf("[HOST]   Created /balloon_vm%d\n", i+1);
    }
    printf("\n");
 
    /* Step 3: Wait for all guests */
    wait_for_guests();
 
    /* Step 4: Main control loop */
    int cycle = 0;
    while (running) {
        cycle++;
        printf("══ CYCLE %d ══════════════════════════════\n\n", cycle);
 
        /* Check for pressure — deflate if needed */
        int pressured = find_pressured_vm();
        if (pressured != -1) {
            printf("[HOST] VM%d under pressure! Deflating first.\n",
                pressured+1);
            deflate_vm(pressured);
        }
 
        /* Inflate the most idle VM */
        int idlest = find_idlest_vm();
        inflate_vm(idlest);
 
        /* Print dashboard */
        print_dashboard(cycle);
 
        /* Every 3 cycles: deflate the fullest VM */
        if (cycle % 3 == 0) {
            int fullest = 0;
            for (int i = 1; i < NUM_VMS; i++) {
                if (vms[i]->current_pages > vms[fullest]->current_pages)
                    fullest = i;
            }
            printf("[HOST] Periodic deflate of VM%d\n\n", fullest+1);
            deflate_vm(fullest);
            print_dashboard(cycle);
        }
 
        sleep(LOOP_DELAY);
    }
 
    cleanup();
    return 0;
}