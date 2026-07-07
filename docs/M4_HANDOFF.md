# M4 Handoff — Structural Rollback: minimal serialization

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

## 3. THE CORRECTED PROBLEM (the active work)

The current "structural" path is **full-region copies dressed as serialization** — ~1.1MB/frame — NOT real serialization. That's why dual stalls (peers can't keep 60fps lockstep). Real serialization captures only the meaningful sim state (a few KB) and is *faster* than raw dirty-page. The 1.1MB breaks down as:

| Chunk | Size | Reality | Fix |
|---|---|---|---|
| `engine_snap` `.data` | **293KB** | copies the WHOLE `.data` section; actual sim globals ≈ a dozen values (~100 bytes) | drop the catch-all, keep only the individual sim globals |
| `battle_pools` | **640KB** | serializes ALL pregrown slots (~2016 × ~20 pools) incl. empties; live state = a few actors (few KB) | live-objects-only |
| `::battle` text | **175KB** | verbose ASCII walk (`i362;`/`s5:count;`), whole tree re-walked | compact BINARY; checksum = hash of that canonical binary |

Target: **single-digit KB/frame → faster than raw dirty-page**, keeping the sim-only / render-never-rewound win.

## 4. NEXT STEPS (the M4 finish — reprioritized after reading engine_snap)

1. **`battle_pools` live-only (DO FIRST — biggest + cleanest win, task #39).** Serializes ALL pregrown slots (~2016 × ~20 pools = ~640KB) incl. empties; live state = a few actors (few KB). Serialize + restore only occupied slots. Clean, no whack-a-mole. The single biggest size chunk.
2. **`::battle` compact binary (task #40)** — verbose ASCII (~175KB) → compact binary of meaningful fields; checksum hashes the canonical binary. `raw_ser_*` already walks the structs — emit binary not text.
3. **`engine_snap` `.data` (task #38) — NOT hand-picking.** The 293KB whole-`.data` copy is DELIBERATE: th155 keeps many pool/free-list heads in static `.data` (e.g. Squirrel-instance pool head `0x4DCD00`) that mutate every frame; the author warns hand-picking is "a losing game." → **DELTA-ENCODE `.data` vs an arm-baseline** (memcmp current vs baseline ~µs, emit only changed `[offset,len,bytes]` runs — small since most `.data` is static). NUANCE: the *restore* needs the baseline (applying it = 293KB copy) or an incremental restore over the ever-changed set — the restore cost, not just save size, drives the dual stall. Think it through.
4. **bullet / cpp (task #41)** — bullet: minimal-serialize OR selective `snapshot_ring` dirty-page (deep pointer graph). cpp sim-satellites = the `AnimController`/`Actor2DGroup` `std::vector`s keyed on actor identity; render objects never serialized = the render-never-rewound GOAL. (Also REVERT the bullet-skip diagnostic, §6.)
5. **Validate (task #42):** measure `[perf]` save-time each step, target < 16ms frame budget; re-run `run_both_struct.bat` → the dual real-time stall (#17) should evaporate → **PvP structural DONE**.
6. **M5 (#36)** — delete the raw path.

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
