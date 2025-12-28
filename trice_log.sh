#!/bin/bash
# ==============================================================================
# Trice Log Viewer Script
# ==============================================================================
# This script connects to the device's Trice TCP server and displays
# decoded log messages in real-time
# ==============================================================================

set -e

TRICE_TOOL="./tools/trice"
TIL_JSON="./til.json"
LI_JSON="./li.json"
DEVICE_IP="10.42.0.203"
DEVICE_PORT="61486"

# Colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${BLUE}=== Trice Log Viewer ===${NC}"
echo "Connecting to ${DEVICE_IP}:${DEVICE_PORT}..."
echo -e "${YELLOW}Press Ctrl+C to exit${NC}"
echo ""

# Check if trice tool exists
if [ ! -f "$TRICE_TOOL" ]; then
    echo "Error: Trice tool not found at $TRICE_TOOL"
    exit 1
fi

# Check if device is reachable
if ! ping -c 1 -W 1 $DEVICE_IP >/dev/null 2>&1; then
    echo "Warning: Device $DEVICE_IP is not responding to ping"
    echo "Attempting connection anyway..."
fi

# Connect to device and display decoded logs
$TRICE_TOOL log \
    -p TCP4 \
    -args "${DEVICE_IP}:${DEVICE_PORT}" \
    -i $TIL_JSON \
    -li $LI_JSON \
    -color default \
    -showID "ID:%5d " \
    -ts "ms" \
    -prefix "time: "
