#!/bin/bash
# Probe batch for the alt rig: runs run_both_lockstep.bat N times, one run at a time,
# summarises each run and STOPS at the first anomaly (missing loop exit, VEH crash,
# purecall/abort/terminate, clguard recovery, heap-corruption report).
# usage: probe_stall.sh <first_run_no> <count>
cd /mnt/c/dev/aocf/th155_alt || exit 1
first=${1:-1}; count=${2:-5}
for ((r=first; r<first+count; r++)); do
  sed -i "s/aocf_ls_p1_[0-9]*\.log/aocf_ls_p1_${r}.log/" run_host_lockstep.bat
  sed -i "s/aocf_ls_p2_[0-9]*\.log/aocf_ls_p2_${r}.log/" run_client_lockstep.bat
  rm -f aocf_crash.log hangdump.now
  cmd.exe /c run_both_lockstep.bat >/dev/null 2>&1
  hung=0
  for ((i=0; i<70; i++)); do
    sleep 5
    if ! tasklist.exe 2>/dev/null | grep -q th155r.exe; then break; fi
  done
  if tasklist.exe 2>/dev/null | grep -q th155r.exe; then
    hung=1; touch hangdump.now; sleep 10
    powershell.exe -NoProfile -File _threads.ps1 > hang_threads_${r}.txt 2>&1
    taskkill.exe /F /IM th155r.exe >/dev/null 2>&1; sleep 2
  fi
  p1=aocf_ls_p1_${r}.log; p2=aocf_ls_p2_${r}.log
  f1=$(grep -o "\[hb\] forward f=[0-9]*" $p1 | tail -1 | grep -o "[0-9]*$")
  f2=$(grep -o "\[hb\] forward f=[0-9]*" $p2 | tail -1 | grep -o "[0-9]*$")
  e1=$(grep -c "frame loop exited\|exiting clean" $p1); e2=$(grep -c "frame loop exited\|exiting clean" $p2)
  d1=$(grep -c "DESYNC" $p1); d2=$(grep -c "DESYNC" $p2)
  rb1=$(grep -o "nload=[0-9]*" $p1 | grep -o "[0-9]*" | paste -sd+ | bc); rb1=${rb1:-0}
  g1=$(grep -c "insguard\|updeff\]" $p1); g2=$(grep -c "insguard\|updeff\]" $p2)
  cl=$(grep -c "CRASH (squiroll VEH)\|__purecall\|FASTFAIL via abort\|std::terminate\|RtlReportException\|FASTFAIL via __" aocf_crash.log 2>/dev/null)
  hk=$(grep -ch "heapchk\] !!\|\[ivec\] !!" $p1 $p2 | paste -sd+ | bc)
  if [ -z "$f1" ] && [ -z "$f2" ] && [ "${cl:-0}" = 0 ]; then
    if [ "${retried:-}" != "$r" ]; then
      echo "## run $r: never connected (rig startup flake) — retrying once"
      retried=$r; taskkill.exe /F /IM th155r.exe >/dev/null 2>&1; sleep 5
      r=$((r-1)); continue
    fi
  fi
  echo "## run $r: p1 f=$f1 exit=$e1 desync=$d1 guard=$g1 | p2 f=$f2 exit=$e2 desync=$d2 guard=$g2 | rollbacks=$rb1 crashlog_hits=$cl heapbad=$hk hung=$hung"
  mkdir -p runs/ls_$r; cp -f $p1 $p2 runs/ls_$r/ 2>/dev/null; cp -f aocf_crash.log runs/ls_$r/ 2>/dev/null
  if [ "$hung" = 1 ] || [ "$e1" = 0 ] || [ "$e2" = 0 ] || [ "${cl:-0}" != 0 ] || [ "${hk:-0}" != 0 ]; then
    echo "   >>> anomaly in run $r"
    grep -h "heapchk\] !!\|\[ivec\] !!\|insguard" $p1 $p2 | head -8
    grep -A3 "__purecall\|FASTFAIL via abort\|std::terminate\|CRASH (squiroll VEH)" aocf_crash.log | head -40
    break
  fi
done
echo "batch done"
