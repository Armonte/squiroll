#!/usr/bin/env bash
# Single-instance squiroll gekko stress harness.
#
# Launches ONE th155 instance with auto_connect=solo, which runs a
# GekkoStressSession (both players local, no networking; rolls back 8
# frames every frame). Waits WAIT_SECONDS, taskkills it, prints the log.
#
# This is the fast iteration rig for rollback determinism + perf work —
# no second process, no UDP handshake to wait on.
#
# Usage:
#   ./scripts/run_solo.sh [wait_seconds]
#
# Defaults to 20 seconds of run time.

set -u

WAIT_SECONDS="${1:-20}"
TH155_DIR="/mnt/c/dev/aocf/th155"
TH155_DIR_WIN='C:\dev\aocf\th155'
SOLO_LOG="$TH155_DIR/aocf_net_solo.log"

# --- 0. Kill leftovers from a previous run -----------------------------------
echo "=== [harness] killing any leftover th155 / th155r ==="
powershell.exe -NoProfile -Command \
    "Get-Process -Name th155,th155r -ErrorAction SilentlyContinue | Stop-Process -Force" \
    2>/dev/null
sleep 1

# --- 1. Truncate the log so we only see fresh output -------------------------
echo "=== [harness] truncating log ==="
: > "$SOLO_LOG" 2>/dev/null || true

# --- 2. Launch the solo instance (detached) ----------------------------------
echo "=== [harness] launching solo instance ==="
powershell.exe -NoProfile -Command \
    "Start-Process -FilePath cmd.exe -ArgumentList '/c','run_solo.bat' -WorkingDirectory '$TH155_DIR_WIN' -WindowStyle Hidden" \
    2>/dev/null

# --- 3. Wait while the stress run plays out ----------------------------------
echo "=== [harness] running for ${WAIT_SECONDS}s ==="
sleep "$WAIT_SECONDS"

# --- 4. Kill the game --------------------------------------------------------
echo "=== [harness] killing th155 / th155r ==="
powershell.exe -NoProfile -Command \
    "Get-Process -Name th155,th155r -ErrorAction SilentlyContinue | Stop-Process -Force" \
    2>/dev/null
sleep 1

# --- 5. Dump the log ---------------------------------------------------------
echo "=== [harness] SOLO log ($SOLO_LOG) ==="
if [ -s "$SOLO_LOG" ]; then
    tr -d '\000' < "$SOLO_LOG"
else
    echo "(solo log is empty or missing)"
fi

echo ""
echo "=== [harness] done ==="
