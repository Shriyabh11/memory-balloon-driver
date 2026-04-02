CC      = gcc
CFLAGS  = -Wall -Wextra -g
LDFLAGS =

all: host guest monitor
	@echo ""
	@echo "Build successful! Run in separate terminals:"
	@echo "  Terminal 1:  ./host"
	@echo "  Terminal 2:  ./guest 1"
	@echo "  Terminal 3:  ./guest 2       (if you chose 2+ VMs)"
	@echo "  Terminal 4:  ./monitor       (optional live dashboard)"
	@echo ""

host: host.c shared_mem.h
	$(CC) $(CFLAGS) host.c -o host $(LDFLAGS)

guest: guest.c shared_mem.h
	$(CC) $(CFLAGS) guest.c -o guest $(LDFLAGS)

monitor: monitor.c shared_mem.h
	$(CC) $(CFLAGS) monitor.c -o monitor $(LDFLAGS)

clean:
	rm -f host guest monitor
	rm -f /tmp/balloon_*

.PHONY: all clean