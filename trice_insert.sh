#!/bin/bash
# ==============================================================================
# Trice ID Insertion Script
# ==============================================================================
# This script inserts unique IDs into TRICE() macros before compilation
# Updates til.json (Trice ID List) and li.json (Location Information)
# ==============================================================================

set -e

TRICE_TOOL="./tools/trice"
TIL_JSON="./til.json"
LI_JSON="./li.json"

# Colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}=== Trice ID Insertion ===${NC}"

# Check if trice tool exists
if [ ! -f "$TRICE_TOOL" ]; then
    echo "Error: Trice tool not found at $TRICE_TOOL"
    echo "Run: cd Middlewares/Third_Party/trice && ./buildTriceTool.sh"
    exit 1
fi

# Insert IDs into TRICE macros
echo "Inserting IDs into TRICE macros..."
$TRICE_TOOL insert \
    -src ./application \
    -src ./Core/Src \
    -i $TIL_JSON \
    -li $LI_JSON

echo -e "${GREEN}✓ Trice IDs inserted${NC}"
echo ""
echo "til.json and li.json updated with new TRICE macros"
echo "Ready to build firmware"
