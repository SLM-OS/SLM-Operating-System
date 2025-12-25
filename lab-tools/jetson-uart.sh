#!/bin/bash
#
# Connect to Jetson 40-pin header UART (UARTA - pins 8/10)
#
# Usage:
#     jetson-uart.sh [port]
#
# Examples:
#     jetson-uart.sh              # Use default /dev/ttyS5
#     jetson-uart.sh /dev/ttyS6   # Use specific port
#

DEFAULT_PORT="/dev/ttyS5"  # Update when second converter arrives
BAUD=115200

PORT="${1:-$DEFAULT_PORT}"

echo "Connecting to Jetson 40-pin UART: $PORT @ ${BAUD}bps"
echo "Exit: Ctrl-A Ctrl-X"
echo ""

/usr/bin/picocom -b "$BAUD" "$PORT"
