#!/bin/bash
# demo.sh — Quick automated demo of the kernel balloon driver
#
# This script loads the module, runs a quick inflate/deflate cycle,
# and shows kernel logs. Perfect for a live demo.
#
# Usage: sudo ./demo.sh
#        sudo ./demo.sh 500      (custom max_pages)

set -e

MAX_PAGES=${1:-200}
STEP=25

# Colors
RED='\033[31m'
GREEN='\033[32m'
YELLOW='\033[33m'
CYAN='\033[36m'
BOLD='\033[1m'
RESET='\033[0m'

banner() {
    echo ""
    echo -e "${CYAN}${BOLD}$1${RESET}"
    echo "────────────────────────────────────────"
}

pause() {
    echo ""
    echo -e "${YELLOW}Press Enter to continue...${RESET}"
    read -r
}

# ── Check root ──
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}ERROR: Run as root: sudo ./demo.sh${RESET}"
    exit 1
fi

# ── Check module exists ──
if [ ! -f balloon_kmod.ko ]; then
    echo -e "${RED}ERROR: balloon_kmod.ko not found. Run 'make' first.${RESET}"
    exit 1
fi

banner "STEP 1: Loading kernel module (max_pages=$MAX_PAGES)"
echo "Command: insmod balloon_kmod.ko max_pages=$MAX_PAGES"
insmod balloon_kmod.ko max_pages=$MAX_PAGES
echo -e "${GREEN}✓ Module loaded!${RESET}"
echo ""
echo "Kernel log:"
dmesg | tail -8
pause

banner "STEP 2: Check /proc/balloon"
echo "Command: cat /proc/balloon"
echo ""
cat /proc/balloon
pause

banner "STEP 3: Check /dev/balloon_ctl"
echo "Command: ls -la /dev/balloon_ctl"
ls -la /dev/balloon_ctl
echo -e "${GREEN}✓ Device exists!${RESET}"
pause

banner "STEP 4: Inflate the balloon (${STEP} pages at a time)"
CURRENT=0
TARGET=$STEP
while [ $TARGET -le $MAX_PAGES ]; do
    echo "  Inflating to $TARGET pages..."
    ./host_kmod_cmd inflate $TARGET 2>/dev/null || true
    
    # Quick ioctl inflate via a tiny helper — but we can just show /proc
    # Actually, let's use a simple approach: write to the device via host_kmod
    sleep 0.5
    TARGET=$((TARGET + STEP))
    
    if [ $TARGET -gt 100 ] && [ $CURRENT -eq 0 ]; then
        break
    fi
    CURRENT=$TARGET
done

# Use the ioctl directly through a small C snippet
echo ""
echo "Sending inflate command via ioctl..."
# We'll use python for a quick ioctl call
python3 -c "
import fcntl, struct, os
fd = os.open('/dev/balloon_ctl', os.O_RDWR)
# BALLOON_IOC_INFLATE = _IOW('B', 1, int) 
# _IOW formula: direction(2)<<30 | size(14)<<16 | type(8)<<8 | nr(8)
# dir=1 (write), size=4 (sizeof int), type=0x42 ('B'), nr=1
cmd = (1 << 30) | (4 << 16) | (0x42 << 8) | 1
data = struct.pack('i', $STEP)
fcntl.ioctl(fd, cmd, data)
os.close(fd)
print('Inflated to $STEP pages via ioctl!')
" 2>/dev/null || echo "(Python not available — use ./host_kmod for interactive demo)"

echo ""
echo "Status after inflate:"
cat /proc/balloon
echo ""
echo "Kernel log:"
dmesg | tail -5
pause

banner "STEP 5: Inflate to 50% capacity"
python3 -c "
import fcntl, struct, os
fd = os.open('/dev/balloon_ctl', os.O_RDWR)
cmd = (1 << 30) | (4 << 16) | (0x42 << 8) | 1
target = $MAX_PAGES // 2
data = struct.pack('i', target)
fcntl.ioctl(fd, cmd, data)
os.close(fd)
print(f'Inflated to {target} pages!')
" 2>/dev/null || echo "(Use ./host_kmod for interactive demo)"
cat /proc/balloon
pause

banner "STEP 6: Deflate back to 25%"
python3 -c "
import fcntl, struct, os
fd = os.open('/dev/balloon_ctl', os.O_RDWR)
cmd = (1 << 30) | (4 << 16) | (0x42 << 8) | 2
target = $MAX_PAGES // 4
data = struct.pack('i', target)
fcntl.ioctl(fd, cmd, data)
os.close(fd)
print(f'Deflated to {target} pages!')
" 2>/dev/null || echo "(Use ./host_kmod for interactive demo)"
cat /proc/balloon
pause

banner "STEP 7: Full deflate and reset"
python3 -c "
import fcntl, struct, os
fd = os.open('/dev/balloon_ctl', os.O_RDWR)
# BALLOON_IOC_RESET = _IO('B', 4)
cmd = (0x42 << 8) | 4
fcntl.ioctl(fd, cmd, 0)
os.close(fd)
print('Balloon reset!')
" 2>/dev/null || echo "(Use ./host_kmod for interactive demo)"
cat /proc/balloon
pause

banner "STEP 8: Unloading module"
echo "Command: rmmod balloon_kmod"
rmmod balloon_kmod
echo -e "${GREEN}✓ Module unloaded cleanly!${RESET}"
echo ""
echo "Final kernel log:"
dmesg | tail -5

echo ""
echo -e "${GREEN}${BOLD}════════════════════════════════════════${RESET}"
echo -e "${GREEN}${BOLD}  Demo complete!${RESET}"
echo -e "${GREEN}${BOLD}════════════════════════════════════════${RESET}"
echo ""
