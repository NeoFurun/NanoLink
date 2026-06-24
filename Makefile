CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g -Iinclude -Wno-address-of-packed-member
TARGET  = bin/nanolink

SRCS = main.c                    \
       src/osal.c               \
       src/mbuf.c               \
       src/timer.c              \
       src/netif.c              \
       src/driver.c             \
       src/tap.c                \
       src/ll_dispatch.c        \
       src/arp.c                \
       src/ip.c                 \
       src/icmp.c               \
       src/route.c              \
       src/udp.c                \
       src/socket.c             \
       src/event_bus.c          \
       src/tcp.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $(SRCS)

bin:
	mkdir -p $@

clean:
	rm -rf $(TARGET)
