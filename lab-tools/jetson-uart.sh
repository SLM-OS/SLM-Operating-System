#!/bin/bash
#
# Connect to Jetson 40-pin header UART (UARTA - pins 8/10)
# This is the primary serial port for SLM-OS debugging.
#
# Usage:
#     jetson-uart.sh [port]
#
# Examples:
#     jetson-uart.sh              # Use port from lab-settings.cfg
#     jetson-uart.sh /dev/ttyUSB1 # Use specific port
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_FILE="$SCRIPT_DIR/lab-settings.cfg"

# Load config
if [ -f "$CONFIG_FILE" ]; then
    source "$CONFIG_FILE"
fi

# Defaults (if not in config)
DEFAULT_PORT="${JETSON_UART_PORT:-/dev/ttyUSB0}"
BAUD="${SERIAL_BAUD:-115200}"

PORT="${1:-$DEFAULT_PORT}"

# Check if port exists
if [ ! -e "$PORT" ]; then
    echo "Error: Serial port $PORT not found"
    echo ""
    echo "Available serial ports:"
    ls -la /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || echo "  (none found)"
    echo ""
    echo "Check lab-settings.cfg or specify port as argument"
    exit 1
fi

echo "Connecting to Jetson 40-pin UART: $PORT @ ${BAUD}bps"
echo "Exit: Ctrl-A Ctrl-X"
echo ""

sudo picocom -b "$BAUD" "$PORT"
