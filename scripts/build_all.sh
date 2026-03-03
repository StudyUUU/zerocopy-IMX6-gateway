#!/bin/bash
#
# Build All Components
#

set -e

echo "=== Building Zero-Copy IMX6 Gateway ==="
echo ""

# Build driver
echo "[1/2] Building kernel driver..."
cd driver
make clean
make
cd ..
echo "Driver build complete!"
echo ""

# Build userspace
echo "[2/2] Building userspace application..."
cd userspace
make clean
make
cd ..
echo "Userspace build complete!"
echo ""

echo "=== Build Complete ==="
echo ""
echo "Next steps:"
echo "  1. Load driver:  sudo ./scripts/load_driver.sh"
echo "  2. Start server: sudo ./scripts/start_server.sh [port]"
