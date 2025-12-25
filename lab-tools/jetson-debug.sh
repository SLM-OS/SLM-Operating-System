#!/bin/bash
#
# Connect to Jetson debug serial port (USB-C debug console)
#
# Usage:
#     jetson-debug.sh [port]
#
# Examples:
#     jetson-debug.sh              # Use default /dev/ttyS4
#     jetson-debug.sh /dev/ttyS5   # Use specific port
#

DEFAULT_PORT="/dev/ttyS4"
BAUD=115200

PORT="${1:-$DEFAULT_PORT}"

echo "Connecting to Jetson debug port: $PORT @ ${BAUD}bps"
echo "Exit: Ctrl-A Ctrl-X"
echo ""

/usr/bin/picocom -b "$BAUD" "$PORT"
