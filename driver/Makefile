# Kernel module Makefile for SPI SDMA Driver

KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# Module name
MODULE_NAME := spi_sdma

# Object files
obj-m := $(MODULE_NAME).o
$(MODULE_NAME)-objs := spi_sdma_driver.o netlink_comm.o

# Compiler flags
ccflags-y := -Wall -Werror

# Build targets
all:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules_install
	depmod -a

help:
	@echo "Makefile for SPI SDMA kernel driver"
	@echo ""
	@echo "Targets:"
	@echo "  all     - Build the kernel module"
	@echo "  clean   - Clean build files"
	@echo "  install - Install module to system"
	@echo "  help    - Show this help"
	@echo ""
	@echo "Variables:"
	@echo "  KERNEL_DIR - Path to kernel source (default: /lib/modules/\$$(uname -r)/build)"

.PHONY: all clean install help
