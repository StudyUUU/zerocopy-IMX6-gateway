#!/bin/bash
#
# Start Zero-Copy Gateway Server
#

SERVER_BIN="./userspace/zerocopy_gateway"
TCP_PORT=${1:-8888}
DEVICE_PATH=${2:-"/dev/spi_sdma"}

echo "=== Starting Zero-Copy Gateway Server ==="
echo "TCP Port: ${TCP_PORT}"
echo "Device: ${DEVICE_PATH}"
echo ""

# Check if server binary exists
if [ ! -f "${SERVER_BIN}" ]; then
    echo "Error: Server binary not found: ${SERVER_BIN}"
    echo "Please build the server first: cd userspace && make"
    exit 1
fi

# Check if device exists
if [ ! -c "${DEVICE_PATH}" ]; then
    echo "Error: Device not found: ${DEVICE_PATH}"
    echo "Please load the driver first: sudo ./scripts/load_driver.sh"
    exit 1
fi

# Run server
echo "Starting server..."
${SERVER_BIN} ${TCP_PORT} ${DEVICE_PATH}
