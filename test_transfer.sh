#!/usr/bin/env bash
# test_transfer.sh - Firmware image transfer cycle test
#
# For each file size: ping → upload (with bg ping) → ping → download (with bg ping) → validate
#
# Usage: ./test_transfer.sh [device_ip]
# Default device: 10.42.0.203

set -euo pipefail

DEVICE="${1:-10.42.0.203}"
PORT=80
BASE_URL="http://${DEVICE}:${PORT}"
UPLOAD_URL="${BASE_URL}/api/image/upload"
DOWNLOAD_URL="${BASE_URL}/api/image/download"
STATUS_URL="${BASE_URL}/api/image/info"

TMPDIR_BASE="$(mktemp -d /tmp/periph_test_XXXXXX)"
trap 'rm -rf "$TMPDIR_BASE"; kill_bg_pings' EXIT

# File sizes in bytes: 100, 256, 300, 512, 1K, 4K, 4.1K, 8K, 120K, 500K
SIZES=(100 256 300 512 1024 4096 4198 8192 122880 512000)
LABELS=("100B" "256B" "300B" "512B" "1K" "4K" "4.1K" "8K" "120K" "500K")

# Counters
PASS=0
FAIL=0
BG_PING_PIDS=()

# ─── Colour helpers ────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'

log()      { echo -e "${CYAN}[$(date +%H:%M:%S.%3N)]${RESET} $*"; }
ok()       { echo -e "${GREEN}  ✓ $*${RESET}"; }
fail()     { echo -e "${RED}  ✗ $*${RESET}"; }
section()  { echo -e "\n${BOLD}${YELLOW}══ $* ══${RESET}"; }
result()   { echo -e "${BOLD}$*${RESET}"; }

# ─── Helpers ──────────────────────────────────────────────────────────────────

kill_bg_pings() {
    for pid in "${BG_PING_PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null || true
    done
    BG_PING_PIDS=()
}

# Run a single ping, print RTT, return 0/1
do_ping() {
    local label="$1"
    local result
    if result=$(ping -c 1 -W 2 "$DEVICE" 2>/dev/null | grep -oP 'time=\K[\d.]+'); then
        ok "ping ${label}: ${result}ms"
        return 0
    else
        fail "ping ${label}: TIMEOUT"
        return 1
    fi
}

# Start background ping that logs with a prefix; stores PID in BG_PING_PIDS
start_bg_ping() {
    local label="$1"
    (
        while true; do
            rtt=$(ping -c 1 -W 2 "$DEVICE" 2>/dev/null | grep -oP 'time=\K[\d.]+' || echo "TIMEOUT")
            echo -e "    ${CYAN}[bg-ping ${label}]${RESET} ${rtt}ms"
            sleep 1
        done
    ) &
    BG_PING_PIDS+=($!)
}

stop_bg_pings() {
    kill_bg_pings
    sleep 0.1   # let final output flush
}

# Generate a deterministic test file of exactly $1 bytes
# Pattern: repeating 0x00..0xFF so corruptions are detectable
make_test_file() {
    local size="$1"
    local path="$2"
    # Use /dev/urandom but with a fixed seed via awk for reproducibility
    # Actually use a simple repeating byte pattern: offset % 256
    python3 -c "
import sys
size = $size
data = bytes(i % 256 for i in range(size))
sys.stdout.buffer.write(data)
" > "$path"
}

# Query /api/firmware/status, pretty-print and return the status field
get_status() {
    curl -sf --max-time 5 "$STATUS_URL" 2>/dev/null || echo '{"status":"unreachable"}'
}

# ─── Single cycle ─────────────────────────────────────────────────────────────

run_cycle() {
    local size="$1"
    local label="$2"
    local cycle_dir="${TMPDIR_BASE}/${label}"
    mkdir -p "$cycle_dir"

    local src="${cycle_dir}/upload.bin"
    local dst="${cycle_dir}/download.bin"
    local upload_log="${cycle_dir}/upload.log"
    local download_log="${cycle_dir}/download.log"
    local upload_resp="${cycle_dir}/upload_resp.json"

    section "Cycle: ${label} (${size} bytes)"

    # ── 1. Pre-upload ping ───────────────────────────────────────────────────
    log "Pre-upload ping..."
    if ! do_ping "pre-upload"; then
        fail "Device unreachable before upload — skipping cycle"
        (( FAIL++ )) || true
        return
    fi

    # ── 2. Generate test file ────────────────────────────────────────────────
    log "Generating ${label} test file..."
    make_test_file "$size" "$src"
    ok "File created: $(wc -c < "$src") bytes"

    # ── 3. Upload with background ping ──────────────────────────────────────
    log "Starting background ping during upload..."
    start_bg_ping "upload"

    log "Uploading ${label}..."
    local upload_start upload_end upload_ms upload_ok=0
    upload_start=$(date +%s%3N)

    if curl -sf \
            --max-time 120 \
            -X POST \
            -H "Content-Type: application/octet-stream" \
            --data-binary "@${src}" \
            "$UPLOAD_URL" \
            -o "$upload_resp" \
            -w "\nHTTP %{http_code} | %{size_upload}B sent | %{time_total}s\n" \
            2>"$upload_log"; then
        upload_ok=1
    fi

    upload_end=$(date +%s%3N)
    upload_ms=$(( upload_end - upload_start ))

    stop_bg_pings

    if [[ $upload_ok -eq 1 ]]; then
        ok "Upload complete in ${upload_ms}ms"
        local bps=$(( size * 1000 / (upload_ms > 0 ? upload_ms : 1) ))
        ok "Throughput: ~${bps} B/s"
        if [[ -f "$upload_resp" ]]; then
            local up_status
            up_status=$(python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('status','?'))" < "$upload_resp" 2>/dev/null || echo "?")
            ok "Upload response: status=${up_status}, $(cat "$upload_resp")"
        fi
    else
        fail "Upload FAILED after ${upload_ms}ms"
        [[ -f "$upload_log" ]] && cat "$upload_log"
        (( FAIL++ )) || true
        return
    fi

    # ── 4. Status check ──────────────────────────────────────────────────────
    log "Checking firmware status..."
    local status_json status_field
    status_json=$(get_status)
    status_field=$(echo "$status_json" | python3 -c "import json,sys; print(json.load(sys.stdin).get('status','?'))" 2>/dev/null || echo "?")
    ok "Device status: ${status_field} | ${status_json}"

    # ── 5. Post-upload ping ──────────────────────────────────────────────────
    log "Post-upload ping..."
    if ! do_ping "post-upload"; then
        fail "Device unreachable after upload"
        (( FAIL++ )) || true
        return
    fi

    # ── 6. Download with background ping ────────────────────────────────────
    log "Starting background ping during download..."
    start_bg_ping "download"

    log "Downloading ${label}..."
    local dl_start dl_end dl_ms dl_ok=0
    dl_start=$(date +%s%3N)

    if curl -sf \
            --max-time 120 \
            "$DOWNLOAD_URL" \
            -o "$dst" \
            -w "\nHTTP %{http_code} | %{size_download}B recv | %{time_total}s\n" \
            2>"$download_log"; then
        dl_ok=1
    fi

    dl_end=$(date +%s%3N)
    dl_ms=$(( dl_end - dl_start ))

    stop_bg_pings

    if [[ $dl_ok -eq 1 ]]; then
        local dl_size
        dl_size=$(wc -c < "$dst")
        ok "Download complete in ${dl_ms}ms (${dl_size} bytes)"
        local dl_bps=$(( dl_size * 1000 / (dl_ms > 0 ? dl_ms : 1) ))
        ok "Throughput: ~${dl_bps} B/s"
    else
        fail "Download FAILED after ${dl_ms}ms"
        [[ -f "$download_log" ]] && cat "$download_log"
        (( FAIL++ )) || true
        return
    fi

    # ── 7. Post-download ping ────────────────────────────────────────────────
    log "Post-download ping..."
    if ! do_ping "post-download"; then
        fail "Device unreachable after download"
        (( FAIL++ )) || true
        return
    fi

    # ── 8. Validation ────────────────────────────────────────────────────────
    log "Validating..."

    local src_size dst_size
    src_size=$(wc -c < "$src")
    dst_size=$(wc -c < "$dst")

    if [[ "$src_size" -ne "$dst_size" ]]; then
        fail "Size mismatch: uploaded=${src_size}B  downloaded=${dst_size}B"
        (( FAIL++ )) || true
        return
    fi
    ok "Size match: ${src_size}B"

    if cmp -s "$src" "$dst"; then
        ok "Content match: byte-for-byte identical"
        ok "CYCLE ${label} PASSED (upload=${upload_ms}ms, download=${dl_ms}ms)"
        (( PASS++ )) || true
    else
        fail "Content MISMATCH! First differing byte:"
        cmp "$src" "$dst" | head -1 || true
        # Show hex diff of first 64 bytes that differ
        python3 - "$src" "$dst" <<'PYEOF'
import sys
src = open(sys.argv[1],'rb').read()
dst = open(sys.argv[2],'rb').read()
shown = 0
for i,(a,b) in enumerate(zip(src,dst)):
    if a != b:
        start = max(0, i-4)
        print(f"  Offset 0x{i:04X} ({i}): expected 0x{a:02X}, got 0x{b:02X}")
        print(f"  Context src[{start}:{start+16}]: {src[start:start+16].hex()}")
        print(f"  Context dst[{start}:{start+16}]: {dst[start:start+16].hex()}")
        shown += 1
        if shown >= 5:
            print("  ... (more differences)")
            break
PYEOF
        (( FAIL++ )) || true
    fi
}

# ─── Main ─────────────────────────────────────────────────────────────────────

echo -e "${BOLD}"
echo "╔══════════════════════════════════════════════════════╗"
echo "║     PeriphNet Image Transfer Test Suite              ║"
echo "╚══════════════════════════════════════════════════════╝"
echo -e "${RESET}"
echo "  Device : ${DEVICE}"
echo "  Sizes  : ${LABELS[*]}"
echo "  Tmpdir : ${TMPDIR_BASE}"
echo ""

# Sanity check: is the device reachable at all?
log "Initial connectivity check..."
if ! ping -c 2 -W 2 "$DEVICE" &>/dev/null; then
    echo -e "${RED}ERROR: Device ${DEVICE} is not reachable. Aborting.${RESET}"
    exit 1
fi
ok "Device is reachable"

TOTAL_START=$(date +%s%3N)

for i in "${!SIZES[@]}"; do
    run_cycle "${SIZES[$i]}" "${LABELS[$i]}"
    echo ""
done

TOTAL_END=$(date +%s%3N)
TOTAL_MS=$(( TOTAL_END - TOTAL_START ))

# ─── Final report ─────────────────────────────────────────────────────────────
echo ""
echo -e "${BOLD}══════════════════════════════════════════════════════${RESET}"
result "TEST SUMMARY"
echo -e "${BOLD}══════════════════════════════════════════════════════${RESET}"
echo "  Total time : $((TOTAL_MS / 1000)).$((TOTAL_MS % 1000 / 10))s"
echo "  Cycles     : ${#SIZES[@]}"
echo -e "  Passed     : ${GREEN}${PASS}${RESET}"
echo -e "  Failed     : ${RED}${FAIL}${RESET}"
echo ""

if [[ $FAIL -eq 0 ]]; then
    echo -e "${GREEN}${BOLD}ALL CYCLES PASSED${RESET}"
    exit 0
else
    echo -e "${RED}${BOLD}${FAIL} CYCLE(S) FAILED${RESET}"
    exit 1
fi
