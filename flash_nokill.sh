#!/bin/bash
# Flash with automatic process termination after verification
# Avoids J-Link clone popup by killing process when done

SCRIPT_FILE="$1"
MAX_WAIT=30

if [ -z "$SCRIPT_FILE" ] || [ ! -f "$SCRIPT_FILE" ]; then
    echo "Usage: $0 <jlink_script_file>"
    exit 1
fi

echo "=== Flash with Auto-Kill ==="
echo "Script: $SCRIPT_FILE"
echo ""

TMPFILE=$(mktemp)
trap "rm -f $TMPFILE" EXIT

JLinkExe -CommandFile "$SCRIPT_FILE" > "$TMPFILE" 2>&1 &
JLINK_PID=$!

echo "JLinkExe started (PID: $JLINK_PID)"
echo "Monitoring for completion..."
echo ""

VERIFIED=0
COMPLETED=0
START_TIME=$(date +%s)

while kill -0 $JLINK_PID 2>/dev/null; do
    if grep -q "Verify successful" "$TMPFILE" 2>/dev/null; then
        if [ $VERIFIED -eq 0 ]; then
            echo "✓ Verification successful"
            VERIFIED=1
        fi
    fi

    if grep -q "Script processing completed" "$TMPFILE" 2>/dev/null; then
        if [ $COMPLETED -eq 0 ]; then
            echo "✓ Script processing completed"
            COMPLETED=1
            sleep 1
            echo "✓ Terminating JLinkExe (avoiding popup hang)"
            kill -9 $JLINK_PID 2>/dev/null
            break
        fi
    fi

    ELAPSED=$(($(date +%s) - START_TIME))
    if [ $ELAPSED -gt $MAX_WAIT ]; then
        echo "⚠ Timeout after ${MAX_WAIT}s, forcing termination"
        kill -9 $JLINK_PID 2>/dev/null
        break
    fi

    sleep 0.2
done

wait $JLINK_PID 2>/dev/null

echo ""
if [ $VERIFIED -eq 1 ] && [ $COMPLETED -eq 1 ]; then
    echo "=== Flash completed successfully ==="
    exit 0
else
    echo "=== Flash may have failed ==="
    cat "$TMPFILE"
    exit 1
fi
