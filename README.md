# Memory Balloon Driver

A **userspace simulation of a virtio-style memory balloon driver** that dynamically inflates and deflates guest memory across multiple virtual machines. Built to demonstrate how real hypervisors (QEMU/KVM, VMware ESXi, GCP Compute Engine) reclaim and return memory using `alloc_page()`, `__free_page()`, and memory pressure signals — all implemented with POSIX shared memory and standard C.

---

## Table of Contents

- [What is Memory Ballooning?](#what-is-memory-ballooning)
- [Why It Matters](#why-it-matters)
- [Architecture](#architecture)
- [File Structure](#file-structure)
- [Shared Memory Design](#shared-memory-design)
- [Host Policy Engine](#host-policy-engine)
- [Guest Driver Internals](#guest-driver-internals)
- [Monitor Dashboard](#monitor-dashboard)
- [How to Build & Run](#how-to-build--run)
- [Configuration Guide](#configuration-guide)
- [Mapping to Real Kernel Concepts](#mapping-to-real-kernel-concepts)
- [Real-World Usage](#real-world-usage)
- [Cleanup](#cleanup)

---

## What is Memory Ballooning?

In virtualization, a **balloon driver** is a cooperative mechanism for the hypervisor to reclaim physical memory from guest VMs without crashing them. The core idea:

1. **Inflate** — The host instructs the guest to allocate pages and "trap" them inside a balloon. Those pages become unavailable to the guest's applications, effectively shrinking its usable memory. The hypervisor can then reassign that physical memory to other VMs.

2. **Deflate** — The host instructs the guest to free those trapped pages back, returning memory to the guest when it needs it again.

3. **Memory Pressure** — If the guest is running low on memory (its applications need more than what's available after inflation), it signals the host. The host can then deflate the balloon to relieve pressure.

This is the exact mechanism used by `virtio_balloon` in the Linux kernel, `vmmemctl` in VMware, and the balloon driver in Hyper-V.

---

## Why It Matters

### The Overcommitment Problem

A physical host with 128GB RAM running 40 VMs each allocated 4GB would need 160GB — more than it has. But most VMs don't use their full allocation simultaneously. Memory ballooning enables **memory overcommitment**: the hypervisor dynamically reclaims unused RAM from idle VMs and gives it to busy ones.

### Production Impact

- **Google Cloud Platform** uses `virtio-balloon` in E2 machine types to reclaim unused guest memory, enabling higher VM density and lower pricing
- **VMware vSphere** uses balloon drivers as the first line of defense before resorting to memory swapping or compression
- **KVM/QEMU** includes `virtio_balloon` as a standard device for all VMs in OpenStack deployments
- **GKE** uses "balloon pods" (low-priority placeholder pods) as a scheduling-level analogy — same concept applied to container orchestration

---

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                          HOST DAEMON (host.c)                    │
│                                                                  │
│  ┌─────────────────┐   Policy Engine:                            │
│  │ /balloon_config  │   • find_idlest_vm()    → inflate target   │
│  │ (global config)  │   • find_pressured_vm() → deflate target   │
│  └────────┬────────┘   • find_fullest_vm()    → rebalance        │
│           │                                                      │
│  ┌────────┴──────────────────────────────────┐                   │
│  │         POSIX Shared Memory Regions        │                   │
│  │                                            │                   │
│  │  /balloon_vm1    /balloon_vm2    /balloon_vm3                  │
│  │  ┌────────────┐  ┌────────────┐  ┌────────────┐               │
│  │  │ command     │  │ command     │  │ command     │              │
│  │  │ target      │  │ target      │  │ target      │  host→guest │
│  │  │─────────────│  │─────────────│  │─────────────│             │
│  │  │ cur_pages   │  │ cur_pages   │  │ cur_pages   │             │
│  │  │ pressure    │  │ pressure    │  │ pressure    │  guest→host │
│  │  │ guest_ready │  │ guest_ready │  │ guest_ready │             │
│  │  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘            │
│  └─────────┼────────────────┼────────────────┼───────┘           │
├────────────┼────────────────┼────────────────┼───────────────────┤
│            ▼                ▼                ▼                    │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐            │
│  │  Guest 1      │  │  Guest 2      │  │  Guest 3      │         │
│  │  (guest.c)    │  │  (guest.c)    │  │  (guest.c)    │         │
│  │               │  │               │  │               │         │
│  │  malloc/free  │  │  malloc/free  │  │  malloc/free  │         │
│  │  = page alloc │  │  = page alloc │  │  = page alloc │         │
│  └──────────────┘  └──────────────┘  └──────────────┘            │
│                                                                  │
│  ┌──────────────────────────────────────────────────────┐        │
│  │  Monitor (monitor.c) — read-only dashboard            │       │
│  │  Opens all shm regions with PROT_READ only            │       │
│  └──────────────────────────────────────────────────────┘        │
└──────────────────────────────────────────────────────────────────┘
```

The system uses a **multi-process architecture** where each component runs as a separate OS process. Communication happens exclusively through POSIX shared memory (`shm_open` + `mmap`), simulating how real virtio devices use shared ring buffers (virtqueues) mapped between host and guest address spaces.

---

## File Structure

| File | Lines | Role |
|------|-------|------|
| `shared_mem.h` | ~60 | Shared struct definitions — the contract between host and guest. Defines `balloon_shm` (per-VM communication) and `balloon_config` (global settings). Analogous to the virtio device specification. |
| `host.c` | ~460 | Host daemon (hypervisor side) — interactive config, creates shared memory regions, runs the inflate/deflate policy loop with pressure-aware scheduling. |
| `guest.c` | ~330 | Guest balloon driver (VM side) — connects to shared memory, obeys inflate/deflate commands by calling `malloc`/`free` to simulate `alloc_page`/`__free_page`, generates synthetic memory pressure. |
| `monitor.c` | ~170 | Read-only live dashboard — opens all shared memory regions without write access, displays a color-coded per-VM view refreshing every second. |
| `makefile` | ~27 | Build system — compiles all three binaries and includes a cleanup target for leaked shared memory. |

---

## Shared Memory Design

### Why Shared Memory?

In a real hypervisor, the host and guest communicate through **virtqueues** — PCI-mapped shared ring buffers. We simulate this with POSIX shared memory (`/dev/shm/`) which gives us:
- Zero-copy bidirectional communication
- Memory-mapped access from multiple processes
- Persistence beyond process lifetime (important for crash recovery)

### `struct balloon_shm` — Per-VM Communication Region

Each VM gets its own shared memory object (`/balloon_vm1`, `/balloon_vm2`, etc.):

```c
struct balloon_shm
{
    // Host → Guest (host writes, guest reads)
    int32_t command;          // CMD_IDLE, CMD_INFLATE, or CMD_DEFLATE
    int32_t target_pages;     // desired balloon size after command completes

    // Guest → Host (guest writes, host reads)
    int32_t current_pages;    // actual balloon size right now
    int32_t guest_ready;      // 0 = not started, 1 = alive and listening
    int32_t vm_id;            // which VM number this is
    int32_t pressure;         // PRESSURE_NONE / LOW / CRITICAL
    int32_t peak_pages;       // highest balloon size ever reached
    int32_t total_inflated;   // lifetime pages inflated
    int32_t total_deflated;   // lifetime pages deflated
};
```

### `struct balloon_config` — Global Configuration

A single shared region (`/balloon_config`) written once by the host at startup:

```c
struct balloon_config
{
    int32_t num_vms;          // how many VMs to expect
    int32_t max_pages;        // ceiling per VM (in pages)
    int32_t inflate_step;     // pages grabbed per inflate command
    int32_t loop_delay;       // seconds between host decisions
    int32_t config_ready;     // flag: 1 = host finished writing
};
```

### Memory Permissions

| Region | Host | Guest | Monitor |
|--------|------|-------|---------|
| `/balloon_config` | `PROT_READ \| PROT_WRITE` | `PROT_READ` | `PROT_READ` |
| `/balloon_vmN` | `PROT_READ \| PROT_WRITE` | `PROT_READ \| PROT_WRITE` | `PROT_READ` |

All regions use `MAP_SHARED` (not `MAP_PRIVATE`) to ensure writes are immediately visible across processes without copy-on-write semantics.

---

## Host Policy Engine

The host runs a decision loop every `loop_delay` seconds with the following priority:

### Priority 1: Relieve Pressure
```
find_pressured_vm() → VM with highest pressure level
  CRITICAL → full deflate (target = 0 pages)
  LOW      → partial deflate (target = current / 2)
```

### Priority 2: Inflate the Smallest Balloon
```
find_idlest_vm() → VM with fewest current_pages
  inflate by inflate_step pages (up to max_pages)
```

### Priority 3: Periodic Rebalance (every 4 cycles)
```
find_fullest_vm() → VM holding the most pages
  partial deflate to half its current size
```

This creates a natural **round-robin** effect: after inflating VM1, it has more pages than VM2, so VM2 becomes the "idlest" and gets inflated next cycle. The rebalance prevents any single VM from dominating the memory budget.

### Timeout Handling

Both inflate and deflate operations wait up to **10 seconds** for the guest to comply. If the guest doesn't reach the target in time, the host moves on. This prevents a hung guest from blocking the entire system.

---

## Guest Driver Internals

### Page Allocation (Inflate)

```c
void *page = malloc(PAGE_SIZE_SIM);     // simulates alloc_page()
memset(page, 0xAB, PAGE_SIZE_SIM);      // force physical backing
pages_held[page_count++] = page;        // track in balloon
```

The `memset` is critical — without it, `malloc` returns a lazy virtual mapping that isn't backed by physical memory. Writing to the page forces the OS to actually allocate a physical frame, which is what `alloc_page()` does in the real kernel.

### Page Deallocation (Deflate)

```c
free(pages_held[--page_count]);         // simulates __free_page()
pages_held[page_count] = NULL;
```

### Pressure Simulation

Real VMs detect memory pressure through Linux's shrinker callbacks or `/proc/meminfo` watermarks. We simulate this probabilistically:

| Balloon Fullness | CRITICAL chance | LOW chance |
|-----------------|----------------|------------|
| > 80% | 10% | 20% |
| > 50% | 5% | 10% |
| ≤ 50% | 0% | 0% |

This creates realistic pressure spikes that trigger the host's deflation policy.

---

## Monitor Dashboard

The monitor is a **read-only observer** — it opens all shared memory regions with `PROT_READ` only and never writes. It provides:

- ANSI color-coded status (green = online, red = offline/critical)
- Per-VM progress bars showing balloon fill level
- Real-time page counts, pressure levels, and active commands
- Aggregate statistics across all VMs
- 1-second refresh with terminal clear (`\033[2J\033[H`)

---

## How to Build & Run

### Prerequisites

- GCC (or any C99 compiler)
- POSIX-compliant OS (Linux, macOS — uses `shm_open`, `mmap`)
- `librt` (POSIX realtime extensions)

### Build

```bash
make
```

### Run (requires multiple terminals)

```bash
# Terminal 1: Start the host daemon (interactive config)
./host

# Terminal 2: Start guest VM 1
./guest 1

# Terminal 3: Start guest VM 2 (if configured 2+ VMs)
./guest 2

# Terminal 4 (optional): Live monitoring dashboard
./monitor
```

> **Note:** Start the host first — it creates the shared memory regions. Guests will retry for up to 10 seconds if the region doesn't exist yet.

---

## Configuration Guide

The host prompts for configuration interactively at startup:

| Parameter | What it controls | Range | Recommended |
|-----------|-----------------|-------|-------------|
| Number of VMs | Guest processes to coordinate | 1–8 | 2–3 for demos |
| Max pages per VM | Ceiling for each balloon (1 page = 4KB) | 10–2048 | 200 for quick, 800 for thorough |
| Inflate step | Pages grabbed per inflate command | 1–max/2 | 25 for smooth, 100 for fast |
| Loop delay | Seconds between host decisions | 1–10 | 2–3 for readable output |

### Example Config

```
VMs: 2, Max: 200 pages (800KB each), Step: 25, Delay: 3s
→ Total memory budget: 1600KB across 2 VMs
→ Each inflate grabs 100KB
→ Full inflate takes ~8 cycles (~24 seconds)
```

---

## Mapping to Real Kernel Concepts

| This Project | Real Linux Kernel / Hypervisor |
|---|---|
| `malloc(PAGE_SIZE_SIM)` | `alloc_page()` — allocate one 4KB page frame |
| `free(page)` | `__free_page()` — return page to buddy allocator free list |
| `memset(page, 0xAB, ...)` | Faulting the page to force physical frame allocation |
| `shm->pressure = PRESSURE_CRITICAL` | Shrinker callback / PSI (Pressure Stall Information) |
| `shm->command = CMD_INFLATE` | Virtqueue notification (host → guest kick) |
| `struct balloon_shm` | Virtio shared ring / virtqueue descriptor table |
| `shm_open()` + `mmap(MAP_SHARED)` | PCI BAR mapping between host and guest address spaces |
| `host.c` policy loop | `virtio_balloon` host-side driver in QEMU |
| `guest.c` command loop | `virtio_balloon.ko` kernel module inside the VM |
| `monitor.c` | `virsh dommemstat` / vCenter performance graphs / GCP balloon metrics |
| `find_idlest_vm()` | Hypervisor memory scheduler (fair-share allocation) |
| Periodic rebalance | VMware's memory tax / GCP E2 memory reclamation |

---

## Real-World Usage

### Google Cloud Platform (GCP)
- **E2 VMs** use `virtio-balloon` to reclaim unused guest memory, enabling higher density and lower pricing on shared-core instances
- GCP exposes the metric `compute.googleapis.com/instance/memory/balloon/ram_size` to track reclaimed memory
- **GKE** uses "balloon pods" — low-priority placeholder pods that are evicted when real workloads need scheduling. Same concept at the container orchestration layer

### VMware vSphere
- `vmmemctl` (balloon driver) is the **first** memory reclamation technique tried before swapping or compression
- Admins monitor balloon activity in vCenter performance graphs
- Excessive ballooning triggers alerts for host memory overcommitment

### KVM/QEMU
- `virtio_balloon` is a standard virtio device compiled into the Linux kernel
- OpenStack Nova uses it for live memory management across compute nodes
- Balloon stats accessible via `virsh dommemstat <domain>`

### Live Migration
- Inflate balloons before migration to reduce VM memory footprint → less data to transfer → faster migration
- Deflate on the destination host after migration completes

---

## Cleanup

If a process crashes and leaves shared memory behind:

```bash
make clean              # removes binaries + /dev/shm/balloon_*

# or manually:
rm -f /dev/shm/balloon_*
```

Shared memory objects persist in `/dev/shm/` until explicitly unlinked. The host's `cleanup()` function handles this on graceful shutdown (Ctrl+C), but a crash may leave orphaned regions.