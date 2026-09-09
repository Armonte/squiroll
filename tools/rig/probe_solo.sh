#!/bin/bash
# Solo-rig runner for state-serialisation work.
#   probe_solo.sh [seconds] [bat]
# Runs the solo rollback stress session for N seconds, kills it, and reports
# the validator verdict plus the [perf-bp] / [perf] budget lines. Default bat
# is run_solo_val.bat (validator on); pass run_solo_perf.bat to measure.
cd /mnt/c/dev/aocf/th155_alt || exit 1
secs=${1:-90}; bat=${2:-run_solo_val.bat}
log=$(grep -oi 'SQUIROLL_LOG_NAME=[^ ]*' "$bat" | cut -d= -f2 | tr -d '\r')
taskkill.exe /F /IM th155r.exe >/dev/null 2>&1; sleep 1
rm -f "$log" aocf_crash.log
cmd.exe /c "$bat" >/dev/null 2>&1
sleep "$secs"
taskkill.exe /F /IM th155r.exe >/dev/null 2>&1; sleep 2
echo "=== $bat / ${secs}s / $log ==="
echo "frames:      $(grep -o '\[hb\] forward f=[0-9]*' "$log" | tail -1)"
echo "round-trip:  FAIL=$(grep -c 'ROUND-TRIP FAIL' "$log")  OK=$(grep -c 'round-trip OK' "$log")  bpvald=$(grep -c '\[bpvald\]' "$log")"
echo "aborts:      $(grep -c 'save aborted' "$log")   desync=$(grep -c DESYNC "$log")   crash=$(grep -c "CRASH (squiroll VEH)\|__purecall\|std::terminate\|FASTFAIL via abort\|FASTFAIL via __report\|FASTFAIL via __invoke\|FASTFAIL via RtlReportException" aocf_crash.log 2>/dev/null)"
grep 'ROUND-TRIP FAIL\|\[bpvald\]\|save aborted' "$log" | head -5
echo "--- [perf-bp] ---";   grep '\[perf-bp\]' "$log" | tail -6
echo "--- [perf-sect] ---"; grep '\[perf-sect\]' "$log" | tail -4
echo "--- [perf] ---";      grep '^\[perf\] per-call\|\[perf\] per-call' "$log" | tail -4
