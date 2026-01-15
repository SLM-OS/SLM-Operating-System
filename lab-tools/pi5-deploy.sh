#!/bin/bash
#
# pi5-deploy.sh - Deploy kernel to Raspberry Pi 5 via SD-Wire
#
# Usage: ./pi5-deploy.sh [kernel_binary]
#        Default kernel: build/kernel/slmos.bin
#

set -e

# Configuration
SDWIRE_SERIAL="sd-wire_1"
PI5_POWER_HOST="192.168.4.89"
MOUNT_POINT="/mnt/pi-boot"
SD_DEVICE="/dev/sdd1"
KERNEL_NAME="kernel_2712.img"

# Default kernel path
KERNEL_BIN="${1:-build/kernel/slmos.bin}"

# Get script directory for finding other tools
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
POWER_SCRIPT="$SCRIPT_DIR/jetson-power.py"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

info() { echo -e "${GREEN}[INFO]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

# Check kernel exists
if [ ! -f "$KERNEL_BIN" ]; then
    error "Kernel binary not found: $KERNEL_BIN"
fi

KERNEL_SIZE=$(stat -c%s "$KERNEL_BIN")
info "Deploying: $KERNEL_BIN ($KERNEL_SIZE bytes)"

# Step 1: Power off Pi 5
info "Powering off Pi 5..."
"$POWER_SCRIPT" --host "$PI5_POWER_HOST" off 2>/dev/null || true
sleep 1

# Step 2: Switch SD card to test server
info "Switching SD card to test server..."
sudo sd-mux-ctrl --device-serial="$SDWIRE_SERIAL" --ts
sleep 2

# Step 3: Wait for device to appear
info "Waiting for SD card device..."
for i in {1..10}; do
    if [ -b "$SD_DEVICE" ]; then
        break
    fi
    sleep 1
done

if [ ! -b "$SD_DEVICE" ]; then
    error "SD card device $SD_DEVICE not found"
fi

# Step 4: Mount boot partition
info "Mounting boot partition..."
sudo mount "$SD_DEVICE" "$MOUNT_POINT"

# Step 5: Deploy kernel
info "Copying kernel to SD card..."
sudo cp "$KERNEL_BIN" "$MOUNT_POINT/$KERNEL_NAME"
sudo sync

# Verify
DEPLOYED_SIZE=$(stat -c%s "$MOUNT_POINT/$KERNEL_NAME")
if [ "$KERNEL_SIZE" != "$DEPLOYED_SIZE" ]; then
    sudo umount "$MOUNT_POINT"
    error "Size mismatch after copy! Expected $KERNEL_SIZE, got $DEPLOYED_SIZE"
fi

# Step 6: Unmount
info "Unmounting..."
sudo umount "$MOUNT_POINT"

# Step 7: Switch SD card to DUT
info "Switching SD card to Pi 5..."
sudo sd-mux-ctrl --device-serial="$SDWIRE_SERIAL" --dut
sleep 1

# Step 8: Power on Pi 5
info "Powering on Pi 5..."
"$POWER_SCRIPT" --host "$PI5_POWER_HOST" on

echo ""
info "Deploy complete! Pi 5 is booting."
echo "  Kernel: $KERNEL_BIN"
echo "  Size:   $KERNEL_SIZE bytes"
