# M4 Handoff — Structural Rollback: minimal serialization

> ## ⚡ 2026-07-06 RESOLUTION — the godlike-fast-correct path already exists (RAW mode)
> The "structural SAVE mode" (`SQUIROLL_ARENA_ROLLBACK=0`) that produced the 9MB state was a **detour**. The correct design was already implemented in **raw mode** (default, `=1`):
> - **save/restore = `snapshot_ring` dirty-page** (~24KB/frame churn for bullet, KB total) → GekkoNet state = a tiny frame handle, **NOT 9MB**.
> - **cross-peer desync checksum = the structural `::battle` hash** (`gekko_bridge.cpp:1436-1486`, `final_cs`, `!g_solo` only) — pointer-independent, canonical, matches byte-for-byte between peers. This is the ONE job structural is uniquely good at.
> - Today's **method-name omission fix** (`raw_ser_*`) directly accelerated that checksum: `call_squirrel_save` blob 170KB→38KB, `save_battle` ~4.7ms→**~3.1ms**.
>
> **PROOF (`run_both_notf4a.bat`, raw dual, 6% loss + 20ms jitter):** both peers ran the **full 90s, ZERO desync, clean exit** (`f=3531/3526`), **~39fps**. Dirty-page sustains dual; the structural checksum agrees cross-peer. Correctness = DONE.
>
> **REMAINING (not correctness):** (1) perf headroom 39→60fps under stress = task #25 (the ~3.1ms checksum + snapshot_ring capture; consider hashing a minimal sim-field set instead of the full ::battle walk). (2) validate the REAL online flow (lobby→CSS→battle) not just auto_connect (#24). (3) M5 (#36): decide whether to keep/retire the `=0` structural-save path (now only useful as the checksum's serializer + a dev oracle).
>
> ### Perf (task #25/#26) + shipping-hardening status (2026-07-06)
> - **60fps: MET on good connections** — `run_both_good.bat` (15ms delay, 0 loss/jitter) = 59.8fps both peers, 0 rollbacks, 0 desync. Bad-connection torture (`run_both_notf4a.bat`, 6% loss) degrades gracefully to ~48fps. Perf commits: `bc43714` (Lever 1 diag strip, 39→46 — the big win), `f64e694` (Lever 2b/3a checksum load-only skip, →48). Remaining save ~6.5ms = genuine work (checksum walk + dirty-page capture + pool serialize); further trimming only helps the bad-conn margin (diminishing).
> - **#27 GC crash: MITIGATED + verified** — GC suppressed while armed (sound: refcounting keeps live flat ~8.5MB, cyclic leak ~750KB/match vs 64MB cap, 0 crash/OOM). Proper fix (snapshot the GC-chain head) → M5 #36.
> - **#21 rollback-SFX: HANDLED** — audio write skipped during re-sim (cl_iter_guard.cpp:206), no double-play.
> - **GENUINE REMAINING GAPS (untested/unbuilt):** #24 real lobby→CSS→battle flow (only auto_connect tested), #29 peer-disconnect → unwind to menu (currently closes), #23 mod-plugin round-end guards, #22 multi-round CSS loop.
>
> The sections below are the PRE-resolution plan (kept for context). The structural-SAVE minimization work (M4a/b/c/d) is **moot for shipping** — raw dirty-page already wins. Structural serialization lives on ONLY as the checksum.

---


**Status: M4 in progress. Structural serialization is PROVEN CORRECT for PvP. The remaining work is a performance redesign: make the save MINIMAL (KB, not MB) so it's faster than the raw dirty-page path.** This resolves the dual real-time stall and delivers the "render is never rewound" goal at speed.

All M4 work is gated behind `SQUIROLL_ARENA_ROLLBACK=0` — the raw/default build is byte-for-byte unchanged and unaffected. Committed: `7057e50` (+ uncommitted diagnostics, see §6).

---

## 1. Why M4 exists (the through-line)

The whole rollback effort's root problem: the raw-page savestate **rewinds th155's render machinery** along with the sim, so any plugin HUD (and th155's own render driven forward-only) gets corrupted on rollback. Proven this session by the native-HUD experiment: native/forward-only render survives 2401 rollbacks; th155-render-reuse dies on rollback #1. **The fix is structural serialization: capture ONLY sim state, never touch render → render is never rewound.** The user called this from the start.

## 2. What's PROVEN (don't re-litigate)

- **Structural restore works.** `load_into` correctly restores actor state (instrumented + confirmed).
- **Dual PvP structural = ZERO desync.** The structural checksum round-trips correctly for the real target (both sides human; the `com_*` CPU-AI divergences are solo-only and don't exist in PvP).
- **Solo structural** restores correctly to f=12 (only CPU-AI `com_*` desyncs).
- Cosmetic/forward-only state (the `count` animation counters, `__setTable`/`_staticTable`) is excluded at the correct layer — the **native serializer** `__gekko_cpp_ser` (`raw_is_skip_key` + `raw_ser_table`), NOT the Squirrel `ser()` (which is the unused fallback).

## 3b. MEASURED ROOT CAUSE (2026-07-06) — the stall is TIME, and it's the method-name noise

Solo struct harness, per-call timing (perf logs, first 20 calls):
- **`save_battle` ≈ 4.7ms/call**, **`load_battle` ≈ 15.9ms/call.** Solo stress rolls back 8f/frame → 8×16ms = 128ms/frame ≈ the observed **~5fps** (`[runone]`=241 in 45s). In dual, even a 2-frame rollback = 32ms > the 16.6ms budget → **the stall**. This is why dropping bullet's 7.6MB did nothing — the cost was never memcpy size.
- Per-section blob SIZE (measured, `[secsize]`): pools 410KB, engine(.data) 293KB, boostpools 249KB, **::battle text 171KB**, rng 61KB, input 8B → ~1.18MB total (bullet skipped by the diagnostic).
- **THE `::battle` text is 88% skipped-method-name noise.** `aocf_txt` analysis: 150,857 / 170,646 bytes are `sN:name;?;` runs; **7,119 of 8,726 keys (82%) are skipped methods** (`constructor`, `Update`, `ConnectRenderSlot`, `weakref`…) — static class metadata re-serialized *and re-parsed* every frame.

**FIX (implemented, validating):** `raw_ser_instance` + `raw_ser_table` in `gekko_bridge.cpp` now **OMIT** methods / skip-keys entirely (no `sN:name;?;`) and shrink the emitted member `count` to match. Safe because the deser is count-driven + keyed and `load_into` leaves any key absent from `data` untouched — behaviorally identical to the old `?;` skip-sentinel, ~8× smaller. Expect blob 171KB→~20KB and save/load time to drop proportionally. **[PENDING] post-fix `[perf]` numbers + solo desync-clean confirm.**

## 3. THE CORRECTED PROBLEM (the active work)

The current "structural" path is **too big/slow to hold 60fps lockstep** — that's why dual stalls. Real serialization captures only meaningful sim state and is *faster* than raw dirty-page. **BUT the per-section size table below was ASSUMED, and reading the code proved one assumption already wrong — so STEP 1 IS TO MEASURE, not to optimize a guess.**

| Chunk | Assumed | Reality after reading code | Fix |
|---|---|---|---|
| `engine_snap` `.data` | 293KB | fixed region `[0x498000,+0x47AA4)`≈293KB — this one is real/measured | DELTA-ENCODE vs arm-baseline (§4) |
| `battle_pools` | 640KB | **ALREADY live-only** (skips free slots, `battle_pools.cpp:344`). BUT writes the **free list = 4B × every free slot**; pregrow makes ~2016 slots/pool × 20 → free list ≈ up to 160KB. Live actor bytes are separate. **Actual split unmeasured.** | shrink pregrow TARGET and/or don't serialize the full free list (only the alloc-window prefix) — AFTER measuring |
| `::battle` text | 175KB | measured from the `aocf_txt` dump file sizes — real | compact BINARY; checksum = hash of it |

Target: **single-digit KB/frame → faster than raw dirty-page**, keeping the sim-only / render-never-rewound win.

## 4. NEXT STEPS (the M4 finish — MEASURE FIRST, then cut the real biggest chunk)

0. **MEASURE per-section byte size + save-time (DO FIRST — task #39 pivoted here).** `gekko_bridge.cpp`'s `put_section` knows each section's length; add a one-line `[secsize] name=NNNN` log per section (guard `!g_arena_rollback && frame<=3`), run `runtest_struct.bat` once, read the real sizes. This replaces every assumption below with a fact. Optimize the measured biggest chunk, not a guess. Also log total save-time (`[perf]`). **Do NOT skip this — the pools assumption was already proven wrong by reading the code.**
1. **Then cut the measured biggest chunk.** Candidates, by likely payoff:
   - `::battle` text → **compact binary** (task #40): `raw_ser_*` already walks the structs — emit binary not verbose ASCII (`i362;`/`s5:count;`). Checksum = hash of the canonical binary.
   - `battle_pools` free list (task #39): already live-only, but serializes 4B × every free slot (~160KB at 2016-slot pregrow). Options: shrink pregrow `TARGET` (floor = avoid mid-battle block growth → fwd/resim mismatch, see `pregrow()` comment) and/or serialize only the alloc-window prefix of the free list, not all of it.
   - `engine_snap` `.data` → **DELTA-ENCODE vs arm-baseline** (task #38, NOT hand-pick — the 293KB catch-all is DELIBERATE: th155 keeps pool/free-list heads in static `.data`, e.g. `0x4DCD00`, that mutate every frame; author warns hand-picking is "a losing game"). memcmp vs baseline (~µs), emit only changed `[off,len,bytes]` runs. NUANCE: restore cost (not just save size) drives the stall — the restore either re-applies the baseline (293KB copy) or walks the changed set; think it through.
2. **bullet / cpp (task #41)** — bullet: minimal-serialize OR selective `snapshot_ring` dirty-page (deep pointer graph). cpp sim-satellites = `AnimController`/`Actor2DGroup` `std::vector`s keyed on actor identity; render objects never serialized = the render-never-rewound GOAL. (Also REVERT the bullet-skip diagnostic, §6.)
3. **Validate (task #42):** re-measure `[perf]` save-time each step (target < 16ms); re-run `run_both_struct.bat` → the dual stall (#17) should evaporate → **PvP structural DONE**.
4. **M5 (#36)** — delete the raw path.

## 5. Harness + method

- **Run:** `th155/runtest_struct.bat` (solo, `SQUIROLL_ARENA_ROLLBACK=0` + `SNAP_EFFECT=1`), `th155/run_both_struct.bat` (dual PvP).
- **Pinpoint divergence:** the `[M4-diag]` dump writes `aocf_txt_fN_rbM.txt` (fwd rb=0 / resim rb>0) per frame (`frame<=15`); `cmp aocf_txt_fD_rb0.txt aocf_txt_fD_rb1.txt` → the byte offset → `dd` context → the exact diverging FIELD. This has pinpointed every gap this session.
- **Frame-counter caveat:** `[adv] >>> frame=` is PER-ROUND (resets); `[hb] forward f=` is true forward progress; the `EXIT_SECONDS ... (f=N)` line is authoritative.
- The `[runone]` hook (`cpp_arena.cpp`, logs `this/rb/f`) shows RunOneFrame calls.

## 6. Uncommitted diagnostics to CLEAN UP

- **REVERT:** the bullet-skip in structural (`gekko_bridge.cpp`, `if (g_arena_rollback) put_section("bullet"...)` + the OFF restore chain minus bullet) — it was a diagnostic; bullet must be restored for physics correctness (via §4.4).
- **KEEP:** the `[runone]` frame log (`cpp_arena.cpp`), the `[M4-diag]` text dump.
- The Squirrel `ser()` table-walk edit (`gekko_state.nut`) is on the UNUSED fallback path — harmless, leave or revert.

## 7. Key files

- `src/Netcode/gekko_bridge.cpp` — `init_arena_rollback()`, `gekko_state_size()` (1MB raw / 16MB struct), the OFF save+restore wiring, `raw_ser_*` native serializer + `raw_is_skip_key`, `[M4-diag]` dump.
- `src/Netcode/engine_snap.cpp` — **the §4.1 target** (`.data` catch-all + individual globals).
- `src/Netcode/battle_pools.cpp` — §4.2 target.
- `src/Netcode/embed/gekko_state.nut` — `save_battle`/`load_battle`/`load_into`/`_skip_keys`.
- `docs/STRUCTURAL_ROLLBACK_PLAN.md`, `docs/SIM_STATE_MANIFEST.md` — the plan + Phase-0 manifest.
- Memory: `project_squiroll_structural_rollback.md` (the persistent detailed log).

## 8. Prior milestones (context, mostly done)

- **M2 (tf4A drop):** validated — solo 16201 rollbacks / 0 divergence, dual neutral. `SQUIROLL_NO_TF4A`. Pending only a human visual check before flipping the default.
- **M3 (render/plugin domain) + #28 crash + #30 plugin HUD:** DONE. The #28 nvwgf2um crash is defeated; the rollback-safe plugin HUD is the native immediate-mode `::hud` API (overlay.cpp; survives 2401 rollbacks). `render_arena` is the non-sim domain. (The AoCF-font-atlas RE for a nicer HUD is captured in memory but is optional — M4 makes plain UI.Core.Text plugins work anyway.)
- **#37 LiquidFun:** cosmetic/dormant, no capture needed. Physics coverage = Bullet only.
