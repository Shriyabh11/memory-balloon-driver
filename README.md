# Memory Balloon Driver

A **virtio-style memory balloon driver** that dynamically inflates and deflates guest memory in a simulated VM environment. Built to teach how real hypervisors reclaim and return memory using `alloc_page()`, `__free_page()`, and memory pressure signals.

## What is a Memory Balloon?

In virtualization, a **balloon driver** is how the hypervisor steals memory back from a VM without the VM crashing. The idea:

1. **Inflate** — the host tells the guest to allocate pages and "trap" them inside a balloon. Those pages become unavailable to the guest, effectively shrinking its usable memory.
2. **Deflate** — the host tells the guest to free those pages back, returning memory to the guest.
3. **Pressure** — if the guest is running low on memory, it signals the host so the host can deflate and give memory back.

This is how real drivers like `virtio_balloon` in the Linux kernel work, and this project simulates the full flow in userspace using POSIX shared memory.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                         HOST DAEMON                         │
│                                                             │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐                  │
│  │ /balloon │  │ /balloon │  │ /balloon │  POSIX shared     │
│  │ _vm1     │  │ _vm2     │  │ _vm3     │  memory regions   │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘                  │
│       │              │              │                        │
├───────┼──────────────┼──────────────┼────────────────────────┤
│       ▼              ▼              ▼                        │
│  ┌─────────┐   ┌─────────┐   ┌─────────┐                   │
│  │ Guest 1 │   │ Guest 2 │   │ Guest 3 │   Guest processes  │
│  │ (VM)    │   │ (VM)    │   │ (VM)    │   simulate VMs     │
│  └─────────┘   └─────────┘   └─────────┘                   │
└─────────────────────────────────────────────────────────────┘
```

## Files

| File | Role |
|------|------|
| `shared_mem.h` | Shared struct definitions — the contract between host and guest |
| `host.c` | Host daemon — asks for config, creates shared memory, runs the control loop |
| `guest.c` | Guest driver — connects to shared memory, obeys inflate/deflate commands |
| `monitor.c` | Read-only live dashboard — watches all VMs without sending commands |
| `makefile` | Build system |

## How to Build & Run

```bash
# build
make

# terminal 1: start the host (it will ask you for config interactively)
./host

# terminal 2: start guest VM 1
./guest 1

# terminal 3: start guest VM 2 (if you configured 2+ VMs)
./guest 2

# terminal 4 (optional): live monitoring dashboard
./monitor
```

> The **monitor** is a read-only tool — it opens shared memory without write access and refreshes a color-coded dashboard every second. Great for demos or debugging.

## How It Maps to Real Kernel Concepts

| This Project | Real Linux Kernel |
|---|---|
| `malloc(PAGE_SIZE_SIM)` | `alloc_page()` — allocate one 4KB page |
| `free(page)` | `__free_page()` — return page to free list |
| `shm->pressure = PRESSURE_CRITICAL` | Shrinker callback / `/proc/meminfo` low watermark |
| `shm->command = CMD_INFLATE` | Virtqueue notification from host to guest |
| `struct balloon_shm` | Virtio shared ring / virtqueue descriptor |
| `shm_open()` + `mmap()` | Shared memory mapped between host and guest address spaces |

## Features

- **Interactive config** — choose number of VMs, balloon size, inflate step, loop speed
- **Live dashboard** — visual progress bars, pressure indicators, per-VM stats
- **Partial deflation** — host can shrink balloons gradually instead of dumping everything
- **Memory pressure simulation** — guests randomly signal LOW/CRITICAL based on balloon fullness
- **Per-VM stats** — tracks total pages inflated, deflated, and peak usage
- **Timestamped logs** — every log line shows `[HH:MM:SS]` for easy debugging across terminals
- **Graceful shutdown** — Ctrl+C deflates all balloons and prints final stats before exiting

## Configuration Guide

| Parameter | What it does | Recommended |
|-----------|-------------|-------------|
| Number of VMs | How many guest processes to run | 2-3 for a good demo |
| Max pages | Ceiling per VM (1 page = 4KB) | 200 for quick, 800 for thorough |
| Inflate step | Pages grabbed per inflate cycle | 25 for smooth, 100 for fast |
| Loop delay | Seconds between host decisions | 2-3 for readable output |

## Cleanup

If something crashes and leaves shared memory behind:

```bash
make clean          # removes binaries + /dev/shm/balloon_*
# or manually:
rm /dev/shm/balloon_*
```