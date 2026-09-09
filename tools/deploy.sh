#!/bin/bash
# Deploy firmware and/or the web UI to a PeriphNet board over the tunnel.
#
# THE TWO ARTIFACTS ARE INDEPENDENT AND HAVE DIFFERENT RISK.
#   periphnet_fwu.pnfw  reboots the board, runs unconfirmed, and rolls back to
#                       the golden image if it is not confirmed.  This is the
#                       dangerous one: the board drives a live inverter with a
#                       live battery behind it.
#   periphnet_ui.pnui   lands in external flash, reboots nothing, and cannot
#                       roll anything back.  Safe at any time.
#
# THIS SCRIPT NEVER CONFIRMS ON ITS OWN unless you pass --confirm.  Confirming
# is what disarms the only mechanism that recovers a board you cannot reach:
# an unconfirmed image reboots itself every ~15 min unless kicked, and three
# unattended expiries put the golden image back in about 45 minutes.  The
# default is to KICK -- which buys testing time without committing -- and then
# print the confirm command for you to run once you believe the board.
#
# ORDER MATTERS ON A FIRST DEPLOYMENT.  `POST /api/ui` and the nvdbUser_webUi
# area only exist from the firmware that introduced them, so on a board still
# running an older image the UI upload has nowhere to go.  Firmware therefore
# goes first, and the UI only after the board is back up.
#
# Usage:
#   tools/deploy.sh <host|sodas|zaliakalnis> [options]
#
# Options:
#   --fw-only        firmware only
#   --ui-only        web UI only (no reboot, no confirm cycle)
#   --confirm        confirm automatically IF every health check passes
#   --no-kick        do not reload the confirmation countdown after install
#   --build-dir DIR  where the artifacts are (default: ./build)
#   --timeout SEC    how long to wait for the board to come back (default 120)
#
# Exit codes: 0 ok, 1 usage/precondition, 2 upload/install failed,
#             3 board did not come back, 4 health check refused to confirm.

set -o pipefail

HOST=""; DO_FW=1; DO_UI=1; DO_CONFIRM=0; DO_KICK=1
BUILD_DIR="build"; WAIT_TIMEOUT=120

die()  { echo "ERROR: $1" >&2; exit "${2:-1}"; }
say()  { echo "[$(date +%H:%M:%S)] $*"; }
step() { echo; echo "=== $* ==="; }

# --- named boards --------------------------------------------------------
# Tunnel addresses, from docs/reference_wireguard_hub.md.  Board access is
# tunnel-only; if these do not answer, the tunnel is the problem, not the board.
resolve_host() {
    case "$1" in
        sodas)       echo "10.77.0.5"  ;;   # PeriphNet_2, the board with the Solis
        zaliakalnis) echo "10.77.0.64" ;;   # PeriphNet_1
        *)           echo "$1"         ;;
    esac
}

while [ $# -gt 0 ]; do
    case "$1" in
        --fw-only)   DO_UI=0 ;;
        --ui-only)   DO_FW=0 ;;
        --confirm)   DO_CONFIRM=1 ;;
        --no-kick)   DO_KICK=0 ;;
        --build-dir) BUILD_DIR="$2"; shift ;;
        --timeout)   WAIT_TIMEOUT="$2"; shift ;;
        -h|--help)   sed -n '2,40p' "$0"; exit 0 ;;
        -*)          die "unknown option: $1" ;;
        *)           [ -z "$HOST" ] || die "more than one host given"; HOST="$1" ;;
    esac
    shift
done

[ -n "$HOST" ] || { sed -n '2,40p' "$0"; exit 1; }
HOST=$(resolve_host "$HOST")

FW_BLOB="$BUILD_DIR/periphnet_fwu.pnfw"
UI_BLOB="$BUILD_DIR/periphnet_ui.pnui"

api()      { curl -s -m 20 "http://$HOST$1"; }
api_post() { curl -s -m 30 -X POST "http://$HOST$1"; }
# Renders JSON booleans as `true`/`false`, not Python's True/False -- the
# comparisons below read like the JSON an operator sees with curl.
# Accepts a dotted path ("heap.free") as well as a plain key, so a nested
# field can be read without a second parser.
jget()     { python3 -c "import sys,json
try:
    v=json.load(sys.stdin)
    for k in '$1'.split('.'):
        v=v[k]
    print('true' if v is True else 'false' if v is False else v)
except Exception:
    print('')" 2>/dev/null; }

# --- preconditions -------------------------------------------------------
step "Target $HOST"
[ "$DO_FW" = 1 ] && { [ -f "$FW_BLOB" ] || die "missing $FW_BLOB (build it first)"; }
[ "$DO_UI" = 1 ] && { [ -f "$UI_BLOB" ] || die "missing $UI_BLOB (build it first)"; }

BEFORE=$(api /api/fwu/status)
[ -n "$BEFORE" ] || die "board did not answer /api/fwu/status -- is the tunnel up?" 1
RUNNING_BEFORE=$(echo "$BEFORE" | jget running_version)
CONFIRMED_BEFORE=$(echo "$BEFORE" | jget confirmed)
say "running $RUNNING_BEFORE  confirmed=$CONFIRMED_BEFORE"

# REFUSE to install over an image that has not been confirmed.  Doing so would
# discard the only known-good rollback target the board still has.
if [ "$DO_FW" = 1 ] && [ "$CONFIRMED_BEFORE" = "false" ]; then
    die "the running image is UNCONFIRMED -- confirm or let it roll back before
       installing another. Installing now would spend the board's last safe
       state on an image nobody has vouched for.
       Check:   curl http://$HOST/api/fwu/status
       Confirm: curl -X POST http://$HOST/api/fwu/confirm" 1
fi

# --- firmware ------------------------------------------------------------
if [ "$DO_FW" = 1 ]; then
    step "Uploading firmware ($(stat -c%s "$FW_BLOB") B)"
    say "the board pins its CPU at 100% for the duration -- this is expected"
    R=$(curl -s -m 300 -X POST -H "X-Filename: periphnet_fwu.pnfw" \
             --data-binary @"$FW_BLOB" "http://$HOST/api/image/upload")
    [ "$(echo "$R" | jget status)" = "stored" ] || die "upload failed: $R" 2
    say "stored: $(echo "$R" | jget version)"

    step "Arming install + reboot"
    R=$(api_post /api/fwu/install)
    [ -z "$(echo "$R" | jget error)" ] || die "install refused: $R" 2
    say "${R:-armed}"

    step "Waiting for the board to come back (timeout ${WAIT_TIMEOUT}s)"
    sleep 5
    BACK=0
    for _ in $(seq 1 "$WAIT_TIMEOUT"); do
        S=$(curl -s -m 3 "http://$HOST/api/fwu/status")
        if [ -n "$S" ]; then BACK=1; break; fi
        sleep 1
    done
    [ "$BACK" = 1 ] || die "board did not answer within ${WAIT_TIMEOUT}s.
       It may still roll back on its own -- that is the design.  Watch:
         watch -n5 curl -s http://$HOST/api/fwu/status" 3

    AFTER=$(api /api/fwu/status)
    RUNNING_AFTER=$(echo "$AFTER" | jget running_version)
    ATTEMPTS=$(echo "$AFTER" | jget attempts_remaining)
    LASTRES=$(echo "$AFTER" | jget last_fwu_result)
    say "running $RUNNING_AFTER  attempts_remaining=$ATTEMPTS  last_fwu_result=$LASTRES"

    if [ "$RUNNING_AFTER" = "$RUNNING_BEFORE" ]; then
        die "version did not change ($RUNNING_AFTER) -- the install did not take.
       last_fwu_result=$LASTRES tells you why (see eFwuRes)." 2
    fi

    if [ "$DO_KICK" = 1 ]; then
        api_post "/api/fwu/kick?window_sec=1800" >/dev/null
        say "countdown reloaded to 30 min -- kick again while you test"
    fi
fi

# --- web UI --------------------------------------------------------------
if [ "$DO_UI" = 1 ]; then
    step "Uploading web UI ($(stat -c%s "$UI_BLOB") B)"
    R=$(curl -s -m 60 -X POST --data-binary @"$UI_BLOB" "http://$HOST/api/ui")
    if [ "$(echo "$R" | jget status)" = "stored" ]; then
        say "stored: $(echo "$R" | jget size) B gzip, crc $(echo "$R" | jget crc32)"
    elif [ -z "$R" ] || [ -n "$(echo "$R" | jget error)" ]; then
        die "the board has no /api/ui -- its firmware predates the stored UI.
       Deploy the firmware first, then run again with --ui-only." 2
    else
        die "UI upload refused: $R" 2
    fi
    V=$(api /api/ui)
    [ "$(echo "$V" | jget present)" = "true" ] || die "board reports no page stored: $V" 2
    say "verified present, served gzipped at http://$HOST/"
fi

# --- health + confirm ----------------------------------------------------
if [ "$DO_FW" = 1 ]; then
    step "Health"
    SYS=$(api /api/system/status)
    # The field names here are the ones /api/system/status actually emits:
    # uptime_sec, cpu_load_permille and a NESTED heap.free.  This line asked
    # for uptime/cpu_load/heap_free and so printed "uptime=s cpu=permille
    # heap_free=" on every deployment this script has ever done -- the one
    # health line you read after an install, empty.
    say "uptime=$(echo "$SYS" | jget uptime_sec)s cpu=$(echo "$SYS" | jget cpu_load_permille)permille heap_free=$(echo "$SYS" | jget heap.free) stale=$(echo "$SYS" | jget tasks_stale)"
    CRASH=$(api /api/crash/latest)
    if [ "$(echo "$CRASH" | jget valid)" = "true" ]; then
        say "WARNING: a crash log is present -- read it before confirming:"
        say "  curl http://$HOST/api/crash/latest"
        HEALTHY=0
    else
        HEALTHY=1
    fi

    if [ "$DO_CONFIRM" = 1 ] && [ "$HEALTHY" = 1 ]; then
        step "Confirming"
        R=$(api_post /api/fwu/confirm)
        say "${R:-confirmed}"
        say "stored image promoted to golden; countdown stopped"
    elif [ "$DO_CONFIRM" = 1 ]; then
        die "refusing to confirm: the board is reporting a crash.
       Confirming would promote this image to golden and disarm the rollback." 4
    else
        step "NOT CONFIRMED -- this is deliberate"
        cat <<EOF
The image is running but unconfirmed.  It will reboot itself when the countdown
expires, and after three such reboots the bootloader restores the golden image.
That is the safety net; it is armed right now.

  Keep testing:   curl -X POST http://$HOST/api/fwu/kick
  Check state:    curl http://$HOST/api/fwu/status
  When satisfied: curl -X POST http://$HOST/api/fwu/confirm
EOF
    fi
fi

step "Done"
