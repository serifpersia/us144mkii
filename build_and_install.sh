#!/bin/bash

# Exit immediately if a command exits with a non-zero status.
set -e

echo "--- Cleaning build directory ---"
make clean

echo "--- Compiling the driver ---"
make

echo "--- Installing the driver ---"
SUDO_CMD="sudo"
KERNEL_MODULE_DIR="/lib/modules/$(uname -r)/extra"

echo "Creating directory if it doesn't exist: $KERNEL_MODULE_DIR"
$SUDO_CMD mkdir -p "$KERNEL_MODULE_DIR"

echo "Copying snd-usb-us144mkii.ko to $KERNEL_MODULE_DIR"
$SUDO_CMD cp snd-usb-us144mkii.ko "$KERNEL_MODULE_DIR"

echo "--- Updating module dependencies ---"
$SUDO_CMD depmod -a

echo "--- Reloading the driver ---"
echo "Unloading old driver (if present)..."
$SUDO_CMD rmmod snd_usb_us144mkii -f || true

echo "Loading new driver..."
$SUDO_CMD modprobe snd-usb-us144mkii

echo "--- Driver build and installation complete! ---"