/*
 * host.c — The Host Daemon (hypervisor side)
 *
 * Simulates a virtio-balloon host controller. Asks the user
 * for config, creates shared memory regions, then runs a loop
 * that inflates/deflates guest balloons based on a simple policy.
 *
 * Build:  gcc -Wall -Wextra -g host.c -o host -lrt
 * Run:    ./host
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

//user-configurable values 
static int num_vms      = 0;
static int max_pages    = 0;
static int inflate_step = 0;
static int loop_delay   = 0;

//per-VM tracking 
static struct balloon_shm *vms[MAX_VMS_LIMIT];
static int                 shm_fds[MAX_VMS_LIMIT];

//config shared memory 
static struct balloon_config *cfg    = NULL;
static int cfg_fd = -1;

static int running = 1;

// timestamped logging 
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

//safe integer input 
int read_int(const char *prompt, int min, int max)
{
    int value;
    while (1) {
        printf("%s (%d–%d): ", prompt, min, max);
        fflush(stdout);

        if (scanf("%d", &value) == 1) {
            while (getchar() != '\n'); 
            if (value >= min && value <= max)
                return value;
            printf("  Out of range, try again.\n");
        } else {
            while (getchar() != '\n');
            printf("  Not a number, try again.\n");
        }
    }
}

/* ── interactive setup ── */
void ask_user_config(void)
{
    printf("\n");
    printf("╔══════════════════════════════════════════╗\n");
    printf("║   MEMORY BALLOON DRIVER — SETUP          ║\n");
    printf("╚══════════════════════════════════════════╝\n\n");

    /* question 1 */
    printf("─── How many VMs? ────────────────────────\n");
    printf("Each VM runs as a separate ./guest process.\n");
    num_vms = read_int("Number of VMs", 1, MAX_VMS_LIMIT);
    printf("  %d VM(s)\n\n", num_vms);

    /* question 2 */
    printf("─── Max balloon size per VM (pages)? ─────\n");
    printf("1 page = 4KB — try 200 for a quick demo.\n");
    max_pages = read_int("Max pages", 10, MAX_PAGES_LIMIT);
    printf(" %d pages = %dKB per VM\n\n",
           max_pages, max_pages * (PAGE_SIZE_SIM / 1024));

    /* question 3 */
    printf("─── Inflate step size? ───────────────────\n");
    printf("Pages grabbed per inflate\n");
    int step_max = max_pages / 2;
    if (step_max < 1) step_max = 1;
    inflate_step = read_int("Step (pages)", 1, step_max);
    printf("  %d pages per inflate = %dKB\n\n",
           inflate_step, inflate_step * (PAGE_SIZE_SIM / 1024));

    /* question 4 */
    printf("─── Seconds between host decisions? ──────\n");
    printf("1 = fast, 5 = slow and easy to follow.\n");
    loop_delay = read_int("Delay (sec)", 1, 10);
    printf("  %d second(s)\n\n", loop_delay);

    /* summary box */
    printf("╔══════════════════════════════════════════╗\n");
    printf("║   YOUR CONFIG                            ║\n");
    printf("╠══════════════════════════════════════════╣\n");
    printf("║  VMs:          %-2d                        ║\n", num_vms);
    printf("║  Max balloon:  %-4d pages (%4dKB)        ║\n",
           max_pages, max_pages * (PAGE_SIZE_SIM / 1024));
    printf("║  Step:         %-4d pages (%4dKB)        ║\n",
           inflate_step, inflate_step * (PAGE_SIZE_SIM / 1024));
    printf("║  Delay:        %-2d sec                    ║\n", loop_delay);
    printf("║  Total max:    %6dKB across all VMs    ║\n",
           num_vms * max_pages * (PAGE_SIZE_SIM / 1024));
    printf("╚══════════════════════════════════════════╝\n\n");

    /* write config to shared memory so guests can pick it up */
    cfg_fd = open(SHM_CONFIG_NAME, O_CREAT | O_RDWR, 0666);
    if (cfg_fd == -1) { perror("open config"); exit(1); }

    ftruncate(cfg_fd, sizeof(struct balloon_config));

    cfg = mmap(NULL, sizeof(struct balloon_config),
               PROT_READ | PROT_WRITE, MAP_SHARED, cfg_fd, 0);
    if (cfg == MAP_FAILED) { perror("mmap config"); exit(1); }

    cfg->num_vms      = num_vms;
    cfg->max_pages    = max_pages;
    cfg->inflate_step = inflate_step;
    cfg->loop_delay   = loop_delay;
    cfg->config_ready = 1;

    host_log("Config published to %s\n\n", SHM_CONFIG_NAME);
}

/* Create one VM's shared memory region 
Its of type balloon_shm, defined in shared_mem.h, and returns a pointer to it.
This is where the host and guest will communicate about commands, current pages, pressure, etc.
*/

struct balloon_shm *create_vm_shm(int vm_num, int *fd_out)
{
    char name[64];
    snprintf(name, sizeof(name), "%s%d", SHM_NAME_PREFIX, vm_num); //this takes the prefix("/balloon_vm") and the vm number and creates a unique name for each vm
    int fd = open(name, O_CREAT | O_RDWR, 0666); //create a file-backed shared memory region for "/tmp/balloon_vmX"
    //open() creates the file if it doesn't exist (O_CREAT)
    //O_RDWR = open for reading and writing
    //0666 = file permissions (rw-rw-rw-)
    //MAP_SHARED (used in mmap below) is what prevents copy-on-write, not permissions
    if (fd == -1) { perror("open vm"); exit(1); }

    ftruncate(fd, sizeof(struct balloon_shm)); //this is used to set the size of the shared memory object

    struct balloon_shm *shm = mmap(NULL, sizeof(struct balloon_shm),
                                   PROT_READ | PROT_WRITE,
                                   MAP_SHARED, fd, 0);
    //mmap is used to map the shared memory object to the address space of the process (in this case, the host process)
    //PROT_READ | PROT_WRITE = the shared memory object is mapped for reading and writing
    //MAP_SHARED = the shared memory object is mapped for sharing between processes
    //fd = the file descriptor of the shared memory object
    //0 = offset from the beginning of the shared memory object
    if (shm == MAP_FAILED) { perror("mmap vm"); exit(1); }

    memset(shm, 0, sizeof(struct balloon_shm));
    //memset is used to fill the shared memory object with zeros
    //this is done to ensure that the shared memory object is initialized to zero
    //before it is used by the guest process

    *fd_out = fd;
    return shm;
}

/* Block until every guest has set guest_ready = 1 */
void wait_for_guests(void)
{
    host_log("Waiting for %d guest(s)...\n", num_vms);
    host_log("Start them in separate terminals:\n");
    //separate terminals are needed because each guest is its own interactive process
    //with its own stdout, so they each need their own terminal window
    for (int i = 1; i <= num_vms; i++)
        printf("         ./guest %d\n", i);
    printf("\n");

    while (1) {
        int ready = 0;
        for (int i = 0; i < num_vms; i++) {
            if (vms[i]->guest_ready == 1) ready++;
        }
        if (ready == num_vms) break;
        printf("\r");
        host_log("%d/%d guests connected...", ready, num_vms);
        fflush(stdout);
        sleep(1);
    }
    printf("\n");
    host_log("All guests connected!\n\n");
}

/* Return index of VM with fewest pages — best candidate to inflate */
int find_idlest_vm(void)
{
    int best = 0;
    for (int i = 1; i < num_vms; i++) {
        if (vms[i]->current_pages < vms[best]->current_pages)
            best = i;
    }
    return best;
}

/* Return index of VM with highest pressure, or -1 if nobody's stressed */
int find_pressured_vm(void)
{
    int worst = -1, worst_p = PRESSURE_NONE;
    for (int i = 0; i < num_vms; i++) {
        if (vms[i]->pressure > worst_p) {
            worst_p = vms[i]->pressure;
            worst   = i;
        }
    }
    return worst;
}

/* Return index of VM holding the most pages */
int find_fullest_vm(void)
{
    int best = 0;
    for (int i = 1; i < num_vms; i++) {
        if (vms[i]->current_pages > vms[best]->current_pages)
            best = i;
    }
    return best;
}

// Tell a VM to inflate by one step 
// the logic is to increase the target pages by the inflate step size
// and then set the command to CMD_INFLATE
// the guest will then try to allocate memory to reach the target pages
// and the host will wait for the guest to reach the target pages
// if the guest does not reach the target pages within 10 seconds, the host will give up
// and the guest will be left with the current number of pages  

void inflate_vm(int idx)
{
    struct balloon_shm *vm = vms[idx];
    int vm_num = idx + 1;

    int new_target = vm->current_pages + inflate_step;

    if (new_target > max_pages) {
        host_log("VM%d is at max (%d/%d pages), skipping\n",
                 vm_num, vm->current_pages, max_pages);
        return;
    }

    host_log("Inflating VM%d: %d → %d pages\n",
             vm_num, vm->current_pages, new_target);

    vm->target_pages = new_target;
    vm->command      = CMD_INFLATE;

    /* give the guest up to 10s to finish */
    for (int t = 0; t < 10 && vm->current_pages < new_target; t++)
        sleep(1);

    host_log("VM%d now holds %d pages (%dKB)\n",
             vm_num, vm->current_pages,
             vm->current_pages * (PAGE_SIZE_SIM / 1024));
}

/*
 * Tell a VM to deflate — either partially or fully.
 * If target is 0, it frees everything. Otherwise it
 * shrinks to half its current size for a gentler release.
 */
void deflate_vm(int idx, int full)
{
    struct balloon_shm *vm = vms[idx];
    int vm_num = idx + 1;

    if (vm->current_pages == 0) {
        host_log("VM%d already empty\n", vm_num);
        return;
    }

    int had = vm->current_pages;
    int target = full ? 0 : (had / 2);

    const char *kind = full ? "full" : "partial";
    host_log("%s deflate VM%d: %d → %d pages (pressure=%d)\n",
             kind, vm_num, had, target, vm->pressure);

    vm->target_pages = target;
    vm->command      = CMD_DEFLATE;

    for (int t = 0; t < 10 && vm->current_pages > target; t++)
        sleep(1);

    host_log("VM%d now at %d pages (freed %d)\n",
             vm_num, vm->current_pages, had - vm->current_pages);
}

/* Print a live dashboard showing all VMs */
void print_dashboard(int cycle)
{
    int total_pages = 0;
    for (int i = 0; i < num_vms; i++)
        total_pages += vms[i]->current_pages;

    printf("\n");
    printf("┌──────────────────────────────────────────────────────────┐\n");
    printf("│  BALLOON DASHBOARD — CYCLE %-4d                          │\n", cycle);
    printf("├──────────────────────────────────────────────────────────┤\n");

    for (int i = 0; i < num_vms; i++) {
        int pages    = vms[i]->current_pages;
        int kb       = pages * (PAGE_SIZE_SIM / 1024);
        int pct      = (max_pages > 0) ? (pages * 25 / max_pages) : 0;
        int pressure = vms[i]->pressure;
        int peak     = vms[i]->peak_pages;

        /* visual bar, 25 chars wide */
        char bar[26];
        for (int j = 0; j < 25; j++)
            bar[j] = (j < pct) ? '#' : '.';
        bar[25] = '\0';

        const char *p_str = "OK  ";
        if (pressure == PRESSURE_LOW)      p_str = "LOW ";
        if (pressure == PRESSURE_CRITICAL) p_str = "CRIT";

        printf("│ VM%-2d %s %4dp %5dKB  %s  peak:%d │\n",
               i + 1, bar, pages, kb, p_str, peak);
    }

    printf("├──────────────────────────────────────────────────────────┤\n");
    printf("│  Total reclaimed: %5d pages = %7dKB                │\n",
           total_pages, total_pages * (PAGE_SIZE_SIM / 1024));
    printf("│  Capacity used:   %3d%% of total                        │\n",
           (num_vms * max_pages > 0)
               ? (total_pages * 100 / (num_vms * max_pages))
               : 0);
    printf("└──────────────────────────────────────────────────────────┘\n\n");
}

//signal handler for graceful shutdown
//this is used to catch the SIGINT signal (Ctrl+C) and set the running flag to 0
//this will cause the main loop to exit and the program to terminate
void handle_signal(int s) { (void)s; running = 0; }

//cleanup function — deflates all VMs, unmaps shared memory,
//closes file descriptors, and unlinks shm objects so they don't leak
void cleanup(void)
{
    printf("\n");
    host_log("Shutting down — deflating all VMs...\n");

    for (int i = 0; i < num_vms; i++) {
        if (vms[i] && vms[i]->guest_ready)
            deflate_vm(i, 1);
    }
    sleep(2);

    /* print final stats */
    host_log("═══ FINAL STATS ═══\n");
    for (int i = 0; i < num_vms; i++) {
        host_log("  VM%d: inflated %d pages, deflated %d, peak %d\n",
                 i + 1,
                 vms[i]->total_inflated,
                 vms[i]->total_deflated,
                 vms[i]->peak_pages);
    }

    for (int i = 0; i < num_vms; i++) {
        char name[64];
        snprintf(name, sizeof(name), "%s%d", SHM_NAME_PREFIX, i + 1); 
        if (vms[i])     munmap(vms[i], sizeof(struct balloon_shm));//unmap the shared memory object
        if (shm_fds[i]) close(shm_fds[i]);//close the file descriptor
        unlink(name);//delete the shared memory file
    }

    if (cfg)    munmap(cfg, sizeof(struct balloon_config));//unmap the shared memory object
    if (cfg_fd) close(cfg_fd);//close the file descriptor
    unlink(SHM_CONFIG_NAME);//delete the config file

    host_log("Done!\n");
}

//main function
int main(void)
{
    signal(SIGINT, handle_signal);

    /* step 1: get config from user */
    ask_user_config();

    /* step 2: create per-VM shared regions */
    host_log("Creating %d shared memory region(s)...\n", num_vms);
    for (int i = 0; i < num_vms; i++) {
        vms[i] = create_vm_shm(i + 1, &shm_fds[i]);
        host_log("  Created %s%d\n", SHM_NAME_PREFIX, i + 1);
    }
    printf("\n");

    /* step 3: wait for guests to come online */
    wait_for_guests();

    /* step 4: main control loop */
    int cycle = 0;

    while (running) {
        cycle++;
        printf("══════════════════════════════════════════\n");
        host_log("Cycle %d\n", cycle);
        printf("══════════════════════════════════════════\n\n");

        /* priority 1: relieve any VM under pressure */
        int pressured = find_pressured_vm();
        if (pressured != -1) {
            int p = vms[pressured]->pressure;
            if (p == PRESSURE_CRITICAL) {
                host_log("VM%d CRITICAL — full deflate!\n", pressured + 1);
                deflate_vm(pressured, 1);
            } else {
                host_log("VM%d under low pressure — partial deflate\n", pressured + 1);
                deflate_vm(pressured, 0);
            }
        }

        /* priority 2: inflate the idlest VM */
        int idlest = find_idlest_vm();
        inflate_vm(idlest);

        /* dashboard */
        print_dashboard(cycle);

        /* every 4 cycles: trim the fullest VM to keep things balanced */
        if (cycle % 4 == 0) {
            int fullest = find_fullest_vm();
            if (vms[fullest]->current_pages > 0) {
                host_log("Periodic rebalance — trimming VM%d\n", fullest + 1);
                deflate_vm(fullest, 0);
                print_dashboard(cycle);
            }
        }

        sleep(loop_delay);
    }

    cleanup();
    return 0;
}