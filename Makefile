# Top-level Makefile

.PHONY: all driver userspace clean help install

all: driver userspace

driver:
	@echo "Building kernel driver..."
	$(MAKE) -C driver
	@echo ""

userspace:
	@echo "Building userspace application..."
	$(MAKE) -C userspace
	@echo ""

clean:
	@echo "Cleaning all..."
	$(MAKE) -C driver clean
	$(MAKE) -C userspace clean
	@echo "Clean complete"

install: all
	@echo "Installing driver..."
	$(MAKE) -C driver install
	@echo "Installing userspace..."
	$(MAKE) -C userspace install
	@echo "Install complete"

help:
	@echo "Zero-Copy IMX6 Gateway - Top-Level Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all       - Build driver and userspace (default)"
	@echo "  driver    - Build kernel driver only"
	@echo "  userspace - Build userspace application only"
	@echo "  clean     - Clean all build files"
	@echo "  install   - Install driver and application"
	@echo "  help      - Show this help"
	@echo ""
	@echo "Usage:"
	@echo "  make              # Build everything"
	@echo "  make clean        # Clean"
	@echo "  make install      # Install (requires root)"
