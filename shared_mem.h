// both, the guest and host need to agree on one structure of
// shared memory cuz it is the only way they can communicate with each other.
// this file defines the structure of the shared memory, which is used for communication between the host and the guest.
// analogous to virtqueues, the shared memory is a circular buffer that is used for communication between the host and the guest.

#ifndef SHARED_MEM_H
#define SHARED_MEM_H

#include <stdint.h>

#define PAGE_SIZE_SIM = 4096 // SIM needed cuz it might clash with PAGE_SIZE variable defined by the OS
#define MAX_VMS = 8
#define MAX_PAGES = 2048 // each page is 4KB, so 2048 pages = 8MB of memory per VM

// shared memory names

#define SHM_CONFIG_NAME "/balloon_config"
#define SHM_NAME_PREFIX "/balloon_vm"

// cmd types
// host writes a number to the cmd field of the shared memory, and the guest reads it and performs the corresponding action.
#define CMD_IDLE 0
#define CMD_INFLATE 1
#define CMD_DEFLATE 2

// pressure lvls
// similar to cmd types, the host writes a number to the pressure field of the shared memory, and the guest reads it and performs the corresponding action.
#define PRESSURE_NONE 0
#define PRESSURE_LOW 1
#define PRESSURE_CRITICAL 2

struct balloon_sshm
{
   //host writes these 2
    int32_t command;      //idle, inflate, deflate
    int32_t target_pages; //pages balloon should hold after this command is executed

  //guest writes these 4
    int32_t current_pages; //how many pages balloon holds right now     
    int32_t guest_ready;   // 0=not started  1=alive and listening       
    int32_t vm_id;         // which VM number             
    int32_t pressure;      // PRESSURE_NONE / LOW / CRITICAL             
};

#endif