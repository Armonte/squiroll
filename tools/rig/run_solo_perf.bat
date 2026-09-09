@echo off
REM Solo rollback rig with the battle_pools round-trip validator forced on.
REM
REM docs/INSTRUMENTATION.md rule: develop any change to state serialisation
REM here FIRST. SQUIROLL_BPVALIDATE=1 re-serialises the pools after every
REM restore and byte-compares against the blob that was restored, for the
REM whole session instead of the first 24 loads, so a wrong free set or a
REM wrong live set is a "[bp] ROUND-TRIP FAIL" line within seconds -- not a
REM cross-peer desync, or an eip inside the pool region, minutes into a dual
REM run. The solo session rolls back 8 frames every frame, so it hits the
REM save/load path far harder than netplay does.
setlocal
set SQUIROLL_AUTO_CONNECT=solo
set SQUIROLL_SKIP_INTRO=1
set SQUIROLL_LOG_NAME=aocf_solo_perf.log
set SQUIROLL_DEVICE_ID=0
set SQUIROLL_GEKKO_ENABLED=1
set SQUIROLL_SNAP_EFFECT=0
set SQUIROLL_FAKE_INPUT=1
set SQUIROLL_TURBO=1
set SQUIROLL_ROUND_FRAMES=0
set SQUIROLL_INPUT_SEED=0x1111AAAA
REM Match the dual stall rig's engine settings so the [perf-bp] numbers here
REM are comparable with the ones measured there.
set SQUIROLL_D3D_PROBE=0
set SQUIROLL_GPU_SYNC=0
set SQUIROLL_NO_TF4A=1
set SQUIROLL_HEAPCHECK=0
set SQUIROLL_EXIT_SECONDS=90
REM validator OFF: this bat measures. Round-trip checking distorts the load
REM timer (it runs a whole extra save per restore) and pollutes cache.
start "" th155r.exe
