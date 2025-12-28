#!/bin/bash
# ==============================================================================
# Trice ID Cleaning Script
# ==============================================================================
# This script removes IDs from TRICE() macros after compilation
# Keeps til.json and li.json intact
# Useful for clean version control (commit code without IDs)
# ==============================================================================

set -e

TRICE_TOOL="./tools/trice"
TIL_JSON="./til.json"
LI_JSON="./li.json"

# Colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}=== Trice ID Cleaning ===${NC}"

# Check if trice tool exists
if [ ! -f "$TRICE_TOOL" ]; then
    echo "Error: Trice tool not found at $TRICE_TOOL"
    exit 1
fi

# Clean IDs from TRICE macros
echo "Removing IDs from TRICE macros..."
$TRICE_TOOL clean \
    -src ./application \
    -src ./Core/Src \
    -i $TIL_JSON \
    -li $LI_JSON

echo -e "${GREEN}✓ Trice IDs removed${NC}"
echo ""
echo "Source code returned to clean state"
echo "til.json and li.json still contain ID mappings"
