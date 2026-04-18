# Kernel Balloon Driver — Demo Guide

**Time required:** 10–15 minutes  
**Audience:** Professor / TA evaluating the project  
**Environment:** Any Linux machine or VM (Ubuntu 22.04+ recommended)

---

## Prerequisites (one-time setup, ~2 minutes)

```bash
# On Ubuntu/Debian:
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r) -y

# Verify:
ls /lib/modules/$(uname -r)/build   # should exist
gcc --version                        # should work
```

> **Note:** If using VirtualBox, a basic Ubuntu VM works perfectly.  
> If the VM doesn't have kernel headers, install them with the command above.

---

## Quick Start (Demo Path)

### 1. Build everything (~30 seconds)

```bash
cd kernel/
make
```

You should see:
```
  Build successful!
  Quick start:
    sudo insmod balloon_kmod.ko
    sudo ./host_kmod
```

### 2. Load the kernel module

```bash
sudo insmod balloon_kmod.ko
# Or with custom max pages:
sudo insmod balloon_kmod.ko max_pages=500
```

**Verify it loaded:**
```bash
lsmod | grep balloon          # shows "balloon_kmod"
ls -la /dev/balloon_ctl       # device exists
cat /proc/balloon              # shows status with 0 pages
```

### 3. Open three terminals

| Terminal | Command | What it shows |
|----------|---------|---------------|
| Terminal 1 | `sudo dmesg -w` | **Kernel logs** — real `printk` output from ring 0 |
| Terminal 2 | `sudo ./host_kmod` | **Host daemon** — interactive policy engine |
| Terminal 3 | `./monitor_kmod` | **Live dashboard** — real-time balloon stats |

**Terminal 1** — Watch kernel messages:
```bash
sudo dmesg -w
```
You'll see messages like:
```
[  142.5] balloon: INFLATE → now holding 25 pages (100KB), grabbed 25 this round
[  145.3] balloon: INFLATE → now holding 50 pages (200KB), grabbed 25 this round
```

**Terminal 2** — Run the host daemon:
```bash
sudo ./host_kmod
```
It will:
1. Connect to `/dev/balloon_ctl`
2. Ask for inflate step size and delay
3. Start the policy loop (inflate, check pressure, deflate)

**Terminal 3** — Live monitoring:
```bash
./monitor_kmod
```
Shows a real-time dashboard with balloon fill level, pressure, and system memory.

### 4. Watch the demo run

The host daemon will:
- **Inflate** the balloon step by step (grabbing real kernel pages)
- **Detect pressure** when system memory gets low
- **Deflate** automatically under pressure
- **Rebalance** every 4 cycles

Watch all three terminals simultaneously to see the interplay.

### 5. (Optional) Stress test — trigger real pressure

In a **4th terminal**, run:
```bash
# Install stress tool if needed
sudo apt install stress -y

# Eat up memory to trigger real pressure
stress --vm 1 --vm-bytes 256M --timeout 20s
```

Watch the monitor — pressure should change from `OK` to `LOW` or `CRITICAL`, and the host daemon should respond by deflating. This is **real kernel pressure detection**, not simulated!

### 6. Clean up

```bash
# In terminal 2: press Ctrl+C to stop host daemon
# It will auto-deflate and show final stats

# Then unload the module:
sudo rmmod balloon_kmod

# Verify it's gone:
lsmod | grep balloon     # should show nothing
cat /proc/balloon         # "No such file"
```

Check `dmesg` — you'll see the clean unload messages.

---

## Alternative: Automated Demo Script

If you prefer a scripted demo:

```bash
cd kernel/
make
sudo ./demo.sh           # automated walk-through with pauses
sudo ./demo.sh 500       # custom max_pages
```

The script loads the module, inflates/deflates via ioctl, shows status at each step, and cleans up. Press Enter to advance between steps.

---

## What's Different from the Userspace Version?

| Userspace (`../host.c` + `../guest.c`) | Kernel Module (`balloon_kmod.ko`) |
|---|---|
| `malloc(4096)` — userspace heap allocation | `alloc_page(GFP_KERNEL)` — kernel buddy allocator |
| `free(ptr)` — userspace free | `__free_page(page)` — kernel page free |
| `printf()` — stdout | `printk()` → `dmesg` — kernel ring buffer |
| `/tmp/balloon_*` shared memory files | `/dev/balloon_ctl` — ioctl character device |
| `pages_held[]` C array | `struct list_head` — kernel linked list |
| Random pressure simulation | `si_meminfo()` — real `/proc/meminfo` data |
| Runs as normal process (ring 3) | Runs as `.ko` module (ring 0) |
| `shm_open` / `mmap` for IPC | `misc_register` / `proc_create` for interfaces |

## Key Talking Points for the Demo

1. **"We're operating at ring 0"** — The balloon module runs inside the kernel. A bug here would crash the entire system (kernel panic), unlike the userspace version which just segfaults.

2. **"These are real physical pages"** — `alloc_page()` goes through the buddy allocator and grabs a real 4KB page frame. You can see system free memory decrease in real-time.

3. **"Pressure is real, not simulated"** — We call `si_meminfo()` which reads the same data as `/proc/meminfo`. When we inflate the balloon + run a stress test, the system genuinely runs low on memory.

4. **"ioctl is how real drivers communicate"** — VirtualBox guest additions use the exact same pattern: a kernel module registers a character device, and userspace tools send commands via `ioctl()`.

5. **"Check dmesg"** — All kernel messages go through `printk` to the kernel ring buffer. This is how real kernel drivers log — you'd see the same kind of output from the actual `virtio_balloon` module.

---

## Troubleshooting

| Problem | Fix |
|---------|-----|
| `make` fails: "no kernel headers" | `sudo apt install linux-headers-$(uname -r)` |
| `insmod` fails: "invalid module format" | Rebuild: `make clean && make` (headers must match running kernel) |
| `/dev/balloon_ctl` permission denied | Run with `sudo` |
| Module already loaded | `sudo rmmod balloon_kmod` first |
| Can't find `balloon_kmod.ko` | Make sure you're in the `kernel/` directory |

---

## File Structure

```
kernel/
├── balloon_ioctl.h    — Shared ioctl definitions (host ↔ kernel contract)
├── balloon_kmod.c     — THE KERNEL MODULE (ring 0, alloc_page, printk)
├── host_kmod.c        — Userspace host daemon (ioctl commands)
├── monitor_kmod.c     — Userspace live monitor
├── Makefile           — Builds .ko + userspace tools
├── demo.sh            — Automated demo script
└── DEMO_GUIDE.md      — This file
```
