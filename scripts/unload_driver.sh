#!/bin/bash
#
# Unload SPI SDMA Driver Script
#

MODULE_NAME="spi_sdma"

echo "=== Unloading SPI SDMA Driver ==="

if ! lsmod | grep -q "^${MODULE_NAME}"; then
    echo "Module not loaded"
    exit 0
fi

echo "Removing module: ${MODULE_NAME}"
rmmod ${MODULE_NAME}

if [ $? -ne 0 ]; then
    echo "Error: Failed to unload module"
    echo "Try: rmmod -f ${MODULE_NAME}"
    exit 1
fi

echo "Module unloaded successfully!"
