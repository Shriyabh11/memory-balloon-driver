

CC     = gcc
CFLAGS = -Wall -Wextra -g

all: host guest
	@echo ""
	@echo "Done! Now run:"
	@echo "  Terminal 1: ./host          (enter your config)"
	@echo "  Terminal 2: ./guest 1"
	@echo "  Terminal 3: ./guest 2       (if you chose 2+ VMs)"
	@echo "  Terminal 4: ./guest 3       (if you chose 3+ VMs)"

host: host.c shared_mem.h
	$(CC) $(CFLAGS) host.c -o host

guest: guest.c shared_mem.h
	$(CC) $(CFLAGS) guest.c -o guest

clean:
	rm -f host guest
	rm -f /dev/shm/balloon_*    # clean up any leftover shared memory