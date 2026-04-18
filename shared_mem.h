// Both the guest and host need to agree on one structure of
// shared memory because it is the only way they can communicate with each other.
// This file defines the structure of the shared memory, which is used for communication between the host and the guest.
// Analogous to virtqueues, the shared memory struct acts as the communication channel between host and guest.

#ifndef SHARED_MEM_H
#define SHARED_MEM_H

#include <stdint.h>

#define PAGE_SIZE_SIM 4096   // SIM needed cuz it might clash with PAGE_SIZE variable defined by the OS
#define MAX_VMS_LIMIT 8
#define MAX_PAGES_LIMIT 2048 // each page is 4KB, so 2048 pages = 8MB of memory per VM

// shared memory file paths (using /tmp/ for WSL compatibility)
// on native Linux you could use shm_open + /dev/shm/ instead

#define SHM_CONFIG_NAME "/tmp/balloon_config"
#define SHM_NAME_PREFIX "/tmp/balloon_vm"

// cmd types
// host writes a number to the cmd field of the shared memory, and the guest reads it and performs the corresponding action.
#define CMD_IDLE 0
#define CMD_INFLATE 1
#define CMD_DEFLATE 2

// pressure lvls
// the guest writes a pressure level to signal the host about its memory state.
// the host reads this and decides whether to deflate the balloon.
#define PRESSURE_NONE 0
#define PRESSURE_LOW 1
#define PRESSURE_CRITICAL 2

// per-VM shared memory region
// one of these exists for each VM, used for bidirectional communication
struct balloon_shm
{
   //host writes these 2
    int32_t command;      //idle, inflate, deflate
    int32_t target_pages; //pages balloon should hold after this command is executed

  //guest writes these
    int32_t current_pages;   //how many pages balloon holds right now
    int32_t guest_ready;     // 0=not started  1=alive and listening
    int32_t vm_id;           // which VM number
    int32_t pressure;        // PRESSURE_NONE / LOW / CRITICAL
    int32_t peak_pages;      // highest balloon size ever reached
    int32_t total_inflated;  // lifetime count of pages inflated
    int32_t total_deflated;  // lifetime count of pages deflated
};

// global config region — host writes once at startup, guests read
struct balloon_config
{
    int32_t num_vms;      // how many VMs to expect
    int32_t max_pages;    // max balloon size per VM (in pages)
    int32_t inflate_step; // pages to grab per inflate command
    int32_t loop_delay;   // seconds between host decisions
    int32_t vm_weights[MAX_VMS_LIMIT]; // weight assigned to each VM (1-10)
    int32_t config_ready; // 0=not ready, 1=host finished writing config
};

#endif