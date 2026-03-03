#!/bin/bash
#
# Load SPI SDMA Driver Script
#

MODULE_NAME="spi_sdma"
MODULE_PATH="./driver/${MODULE_NAME}.ko"
DEVICE_NAME="spi_sdma"

echo "=== Loading SPI SDMA Driver ==="

# Check if module is already loaded
if lsmod | grep -q "^${MODULE_NAME}"; then
    echo "Module already loaded, removing first..."
    rmmod ${MODULE_NAME}
fi

# Load the module
if [ ! -f "${MODULE_PATH}" ]; then
    echo "Error: Module file not found: ${MODULE_PATH}"
    echo "Please build the driver first: cd driver && make"
    exit 1
fi

echo "Loading module: ${MODULE_PATH}"
insmod ${MODULE_PATH}

if [ $? -ne 0 ]; then
    echo "Error: Failed to load module"
    exit 1
fi

# Wait for device node
sleep 1

# Check device node
if [ ! -c "/dev/${DEVICE_NAME}" ]; then
    echo "Warning: Device node /dev/${DEVICE_NAME} not found"
    echo "Checking dmesg for errors..."
    dmesg | tail -20
    exit 1
fi

# Set device permissions
chmod 666 /dev/${DEVICE_NAME}

echo "Module loaded successfully!"
echo "Device: /dev/${DEVICE_NAME}"
echo ""
echo "Kernel messages:"
dmesg | tail -10

echo ""
echo "Module info:"
lsmod | grep ${MODULE_NAME}
