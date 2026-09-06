# squiroll instrumentation contract

Every diagnostic in the netcode has a cost, and several of them used to be on by
default in the hot path or, worse, silently changed the program's behaviour. This
file is the contract: what each switch does, what it costs, and when to use it.

Modelled on the GDC 2018 talk *8 Frames in 16ms* (`C:\dev\mvc3\rollback\docs\gdc_talk`):
§7 says stop running non-essential systems inside the rollback window, and §13
says the primary desync tool is capturing enough to diagnose **after the fact**,
not hashing everything every frame.

## The three profiles

| Profile | Env | Use |
|---|---|---|
| **Ship / perf** | nothing set | Normal play and any performance measurement. |
| **Soak** | `SQUIROLL_CHECKSUM_EVERY=1` | Long automated runs hunting desyncs. Costs ~3 ms/frame. |
| **Forensic** | `SQUIROLL_TRACE=1` plus the specific probe you need | Reproducing one known failure. Slow on purpose. |

Never mix a forensic switch into a perf measurement, and never leave one set in a
rig you are using to judge stability: several of them change timing enough to
cause peer timeouts that look like netcode bugs.

## Rules the hard way

These are not style preferences; each one cost real debugging time.

1. **A diagnostic must not change behaviour.** `SQUIROLL_CLGUARD_RECOVER` used to
   be unconditional. Its execute-at-NULL path pops only the return address, but
   every th155 vtable slot it fires on is `__stdcall`/`__thiscall`, so the callee
   never cleans its arguments and the caller resumes on a shifted stack. A missing
   `_Delete_this` vtable slot therefore surfaced as "0xC0000374 heap corruption"
   several frames away instead of a clean fault at the real call site. Default off.
2. **No blocking I/O in a hook.** The InputSingle destructor probe called
   `log_flush()` — a synchronous disk write — on every destruction. Now
   `SQUIROLL_ISDTOR=1`, and the hook is not installed unless asked for.
3. **No syscall on a per-draw path.** The first D3D resource check called
   `VirtualQuery` (a syscall) for every sprite and cost about 20% of the frame
   rate. It is now change-triggered: remember the last pointer per resource id and
   revalidate only when it differs. Steady state is two compares.
4. **Validate a detector before trusting it.** `SQUIROLL_DESYNC_INJECT=<frame>`
   corrupts one peer's checksum for exactly one frame. The simulation is untouched,
   so it exercises only the detection path. The first run of it found that the rig
   leaked environment variables from host to client, which would have silently
   ruined every one-sided experiment.
5. **A detector that reports nothing you can act on is not done.** The desync text
   ring was gated behind diag, so a desync in a normal run printed
   `dump: 0 frames`. It is kept by default now and dumps the canonical text of the
   diverging frame on both peers, which a plain `diff` turns into a field name.

6. **A validator must not be able to kill the run.** `HeapValidate` locks a
   *serialized* heap, so walking the process heap while the game runs is safe.
   Walking every heap `GetProcessHeaps` returns is not: some are created
   `HEAP_NO_SERIALIZE` and the graphics driver mutates its own from another
   thread, so the walk races the owner and faults inside ntdll. Level 1 briefly
   did that and killed a run at f=450 with an access violation that reads exactly
   like a game crash until you symbolize it and find our own scanner on the stack.
   Levels 1 and 2 now walk only the process heap; level 3 opts into the full sweep
   and may fault.

## Switch reference

### Always safe to leave on
| Switch | Default | Cost | Notes |
|---|---|---|---|
| `SQUIROLL_CHECKSUM_EVERY` | 4 | ~3 ms per sampled frame | Cross-peer structural checksum. `1` = every frame, `0` = detection off. Both peers must agree; the value keys off the frame number so they sample identically. |
| `SQUIROLL_D3DRES` | on | ~2 compares per draw | Validates the D3D resource behind a Map. `0` disables. |
| `SQUIROLL_RECYCLE` | on | none | Quarantined arena block reuse. `0` leaks instead, for A/B. |

### Diagnostics — opt in, and expect them to be slow
| Switch | Cost | What it is for |
|---|---|---|
| `SQUIROLL_TRACE=1` | ~1.4 log lines/frame | Per-frame `[netstat] [save] [load] [igx] [runone]` traces. Needed to reconstruct a desync. |
| `SQUIROLL_DIAG=1` | ~3x slower | Full structural text dumps per save. |
| `SQUIROLL_HEAPCHECK=1` | full heap walk every 30 frames | Gross heap damage. |
| `SQUIROLL_HEAPCHECK=2` | **starves the sim to ~10 fps and causes false peer timeouts** | Process heap, every advance and around the render pass. Brackets a corruptor to one frame and phase. Bracketing only — never a soak. |
| `SQUIROLL_HEAPCHECK=3` | as level 2, **and can fault** | Every heap in the process, including ones another thread owns. See rule 6. |
| `SQUIROLL_ISDTOR=1` | blocking flush per InputSingle destruction | InputSingle lifetime. |
| `SQUIROLL_FREE_JOURNAL=1` | holds ~16 MB | Defers real-heap frees so a rollback cannot re-enter one. Has never fired; kept for a suspected rollback use-after-free. |
| `SQUIROLL_CLGUARD_RECOVER=1` | corrupts stacks, see rule 1 | Only when surviving a known fault is genuinely the point. |
| `SQUIROLL_DESYNC_INJECT=<frame>` | one frame | Proves the desync detector end to end. Set on ONE peer. Never in a real match. |

7. **A test hook that forges simulation state must be driven by a frame number
   both peers agree on.** `SQUIROLL_ROUND_FRAMES` wrote `::battle.time` from each
   peer's local state machine. The peers forged the timer on different simulated
   frames, timed out on different frames, and one entered the round-end demo
   while the other did not — 3 of 4 runs "desynced" with no netcode fault. Worse,
   exempting `time` from the checksum made the divergence grow to 96% of the
   state, because the timer drives the transition. It is now keyed on the gekko
   frame inside `advance_one_frame`, which both peers agree on and a re-sim
   reproduces.

## Reading the numbers

`[perf] per-call us:` prints the average microseconds for advance, save (split
into the small-blob serialization and the page capture) and load, per 240 saves.
`[perf-sect]` splits the small blob by section. `[perf] save_battle` is the
structural checksum walk.

Current cost per frame on the dual rig at 55 ms delay / 20 ms jitter / 6% loss,
ship config (no HEAPCHECK, no TRACE), medians over 10+ measurement windows:

| Phase | Cost | Largest component |
|---|---|---|
| advance | ~1.3 ms | the game's own tick |
| save | ~2.3 ms | `battle_pools::save` ~1.4 ms |
| load (rollback only) | ~3.1 ms | reverse-apply ~1.1 ms, `battle_pools::load` ~1.0 ms |

Save was 7.3 ms at the start of this work. What is left, measured:

| item | µs | why it is hard |
|---|---|---|
| `battle_pools::save` free-list walk | ~700 | dependent pointer chase over ~43,000 free nodes |
| `battle_pools::save` slot copy | ~315 | 3,000 live slots, already cheap |
| reverse-apply on load | ~1140 | real restore work, scales with rollback distance |
| `battle_pools::load` | ~1040 | relinks the free list |
| dirty page copy | ~400 | ~450 pages, two memcpys each |
| GetWriteWatch (capture+restore) | ~800 | 106 MB queried; cpp arena's bump is 70 MB |
| boost pools | ~250 save / ~450 load | |

**The next real win is incremental pool tracking.** The pools hold ~46,400 slots
(~13.6 MB) of which only ~3,000 are live, so both save and load spend most of
their time rediscovering a free list that changed by a handful of entries.
Hooking the pool allocate/free to maintain the live set and free-list order as
they change removes the ~700 µs walk, the ~1 ms relink, and ~172 KB of the
~400 KB blob. Two alternatives are already ruled out by measurement: copying
whole blocks would make the blob 13.6 MB per save, and collapsing the two
free-list chases into one made it *worse* because the second chase rides warm
cache.

## The rig

`th155_alt/probe_stall.sh <first_run> <count>` runs the two-instance rig
repeatedly, archives each run under `runs/stall_N/`, and stops at the first
anomaly. Both launcher batches use `setlocal` so the two peers cannot share
environment variables — see rule 4.
