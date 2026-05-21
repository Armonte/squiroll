#!/usr/bin/env bash
# AFK test harness for dual-instance squiroll rollback.
#
# Launches host then client via PowerShell Start-Process (each .bat
# detached so it doesn't block the harness), waits WAIT_SECONDS for the
# test to run, then taskkills both th155r instances and prints both
# peer logs.
#
# Usage:
#   ./scripts/run_test.sh [wait_seconds]
#
# Defaults to 20 seconds of run time.

set -u

WAIT_SECONDS="${1:-20}"
TH155_DIR="/mnt/c/dev/aocf/th155"
TH155_DIR_WIN='C:\dev\aocf\th155'
HOST_LOG="$TH155_DIR/aocf_net_p1.log"
CLIENT_LOG="$TH155_DIR/aocf_net_p2.log"

# --- 0. Kill leftovers from a previous run -----------------------------------
echo "=== [harness] killing any leftover th155r.exe / cmd shells ==="
powershell.exe -NoProfile -Command \
    "Get-Process -Name th155,th155r -ErrorAction SilentlyContinue | Stop-Process -Force" \
    2>/dev/null
sleep 1

# --- 1. Truncate the logs so we only see fresh output -------------------------
echo "=== [harness] truncating logs ==="
: > "$HOST_LOG" 2>/dev/null || true
: > "$CLIENT_LOG" 2>/dev/null || true

# --- 2. Launch host via PowerShell Start-Process (detached) ------------------
echo "=== [harness] launching host ==="
powershell.exe -NoProfile -Command \
    "Start-Process -FilePath cmd.exe -ArgumentList '/c','run_host.bat' -WorkingDirectory '$TH155_DIR_WIN' -WindowStyle Hidden" \
    2>/dev/null

# Give the host's StartupServer time to bind 10800 before client connects.
sleep 3

echo "=== [harness] launching client ==="
powershell.exe -NoProfile -Command \
    "Start-Process -FilePath cmd.exe -ArgumentList '/c','run_client.bat' -WorkingDirectory '$TH155_DIR_WIN' -WindowStyle Hidden" \
    2>/dev/null

# --- 3. Wait while the test plays out ----------------------------------------
echo "=== [harness] running for ${WAIT_SECONDS}s ==="
sleep "$WAIT_SECONDS"

# --- 4. Snapshot, then kill the games ----------------------------------------
echo "=== [harness] th155r processes BEFORE kill ==="
powershell.exe -NoProfile -Command \
    "Get-Process -Name th155r -ErrorAction SilentlyContinue | Select-Object Id,StartTime | Format-Table -AutoSize" \
    2>/dev/null

echo "=== [harness] killing th155r.exe instances ==="
powershell.exe -NoProfile -Command \
    "Get-Process -Name th155,th155r -ErrorAction SilentlyContinue | Stop-Process -Force" \
    2>/dev/null
sleep 1

# --- 5. Dump the logs ---------------------------------------------------------
echo "=== [harness] HOST log ($HOST_LOG) ==="
if [ -s "$HOST_LOG" ]; then
    # squiroll's tee_printf memory-maps the log file → NUL-byte gaps
    # between written regions. Strip them so the output is readable.
    tr -d '\000' < "$HOST_LOG"
else
    echo "(host log is empty or missing)"
fi

echo ""
echo "=== [harness] CLIENT log ($CLIENT_LOG) ==="
if [ -s "$CLIENT_LOG" ]; then
    tr -d '\000' < "$CLIENT_LOG"
else
    echo "(client log is empty or missing)"
fi

echo ""
echo "=== [harness] done ==="
