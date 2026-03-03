# Userspace Application Makefile

# Cross-compilation setup for IMX6
CROSS_COMPILE ?= arm-linux-gnueabihf-

CC := $(CROSS_COMPILE)gcc
CFLAGS := -std=gnu99 -Wall -O2 -I../common
LDFLAGS := -lpthread

TARGET := zerocopy_gateway

SRCS := main.c netlink_client.c dma_mapper.c epoll_server.c
OBJS := $(SRCS:.c=.o)

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)
	@echo "Build complete: $(TARGET)"

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
	@echo "Clean complete"

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/
	@echo "Installed to /usr/local/bin/$(TARGET)"

help:
	@echo "Userspace Application Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all     - Build the application"
	@echo "  clean   - Clean build files"
	@echo "  install - Install to /usr/local/bin"
	@echo "  help    - Show this help"
