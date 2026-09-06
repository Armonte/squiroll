# Two-instance rig runners

Copy these next to `th155r.exe` in the harness directory (`th155_alt/`) and run
them from there. They drive the paired launcher batches, archive every run, and
stop at the first anomaly so the failing run's logs are the ones left in place.

    ./probe_stall.sh    <first_run_no> <count>   # normal rollback rig
    ./probe_lockstep.sh <first_run_no> <count>   # same rig, prediction window 0

`probe_lockstep.sh` is the A/B arm: `SQUIROLL_PREDICTION_WINDOW=0` keeps the
snapshots, arenas and cross-peer checksum fully armed but never rolls back
(verified: `nload=0`). If a failure survives that, it is not rollback-caused.

Each run prints one line: frames reached per peer, whether each peer exited
cleanly, desyncs, guard hits, rollback count, crash-log hits and whether the
process had to be killed. A run is archived under `runs/<name>_N/`.

Two things the runners get right that cost time to learn:

* Both launcher batches must `setlocal`. `run_both_*.bat` uses `call`, so without
  it the client inherits every variable the host set, which silently ruins any
  one-sided experiment (found by `SQUIROLL_DESYNC_INJECT` reporting on both peers).
* The rollback count comes from the always-on `[perf] ... nload=` lines, not from
  the `[load]` trace, which is gated behind `SQUIROLL_TRACE=1`.

See `docs/INSTRUMENTATION.md` for what each environment switch costs.
