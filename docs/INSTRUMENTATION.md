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

7. **A test hook that forges simulation state must be driven by a frame number
   both peers agree on.** `SQUIROLL_ROUND_FRAMES` wrote `::battle.time` from each
   peer's local state machine. The peers forged the timer on different simulated
   frames, timed out on different frames, and one entered the round-end demo
   while the other did not — 3 of 4 runs "desynced" with no netcode fault. Worse,
   exempting `time` from the checksum made the divergence grow to 96% of the
   state, because the timer drives the transition. It is now keyed on the gekko
   frame inside `advance_one_frame`, which both peers agree on and a re-sim
   reproduces.

8. **Check what the rig is actually running.** `build.sh` deployed to `th155/`
   and `thcrap/bin/` but not to `th155_alt/`, which is where every rig runner
   lives. A whole measurement pass was run against a two-day-old DLL before the
   numbers stopped making sense. It deploys to both now.
9. **A crash counter that matches your own clean-exit line is worse than no
   counter.** Both probes counted `FASTFAIL via ExitProcess(0) [clean exit]` and
   `FASTFAIL via TerminateProcess` (our own `taskkill`) as crashes, so healthy
   runs reported `crashlog=2`. Match the issuer, not the word FASTFAIL.
10. **A rate question needs a probe that does not stop at the first failure.**
   `probe_stall.sh` halts on the first anomaly, which is right when you want the
   failing run's logs and useless when the question is "is this class more
   common than before my change?". `PROBE_KEEP=1` keeps going.

11. **Compare stability arms at equal simulated FRAMES, not equal wall time.**
   The dual rig is time-boxed (`SQUIROLL_EXIT_SECONDS=60`), so making save/load
   twice as fast made every run reach ~3210 frames instead of ~2600 — past the
   third round transition instead of stopping before it. Measured against the
   slow arm the fast build looked like a stability regression (12 anomalies in
   15 runs vs 3 in 14) and it was not: re-run the slow arm at 75 s, so it
   reaches the same frames, and both arms fail at 7 in 8. The failures are a
   pre-existing class concentrated past the second round transition. An hour
   went into chasing a regression that was a measurement artifact; the tell was
   that the crash signatures (`__purecall`, `vtable=008451CC`, `ret=0046D9D7`,
   `region=none`) were identical in both arms.

12. **Weight save and load by how often they actually run.** There are about
   SEVEN saves per load (`[perf] nsave=240 nload=~33`), because a rollback
   re-simulates and re-saves every frame it replays. Optimising a per-CALL
   number is therefore misleading: the first cut of the boost-pool live-slot
   format won 342 us on load and lost 80 us on save, which reads as a big win
   per call and is a ~220 us/frame LOSS. Always convert to per-frame before
   deciding, and prefer the save side when they conflict.

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
| `SQUIROLL_BPVALIDATE=1` | ~2x the bp section | Round-trip self-test on every restore for the whole session, plus a full free-list walk cross-checked against the hot/cold partition. **The oracle for any change to `battle_pools.cpp`.** |
| `SQUIROLL_BPCANON=0` | slower | A/B arm: the pre-2026-09-08 free-list format and no hot/cold partition, in the same binary. |
| `SQUIROLL_ARENAMAP=1` | one walk | Reports how much of the cpp arena's write-watch query holds live blocks. |
| `SQUIROLL_BPMPCANON=0` | slower | A/B arm: whole-block boost-pool records instead of live-slot + bitmap. |
| `SQUIROLL_MPCENSUS=N` | one walk per N saves | Per boost pool: blocks, slots, live, bytes, region, and whether its free list is ascending. |
| `SQUIROLL_WWBENCH=1` | one-shot at arm | GetWriteWatch cost curve: the same region split into 1..64 calls, read-only and RESET. |
| `SQUIROLL_PAGEHIST=N` | one counter per dirty page | Per-page dirty-frequency histogram, dumped every N forward saves with sq_arena block attribution. |

## Reading the numbers

`[perf] per-call us:` prints the average microseconds for advance, save (split
into the small-blob serialization and the page capture) and load, per 240 saves.
`[perf-sect]` splits the small blob by section. `[perf] save_battle` is the
structural checksum walk.

Current cost per call on the DUAL rig (55 ms delay / 20 ms jitter / 6% loss),
medians over 240-save windows, ship config:

| | start of this work | now |
|---|---|---|
| save | 3353 µs | **~1150 µs** (best window 992) |
| load | 3383 µs | **~1500 µs** (best window 1371) |

Where the save goes now: GetWriteWatch ~290, dirty page copy ~220, small-blob
copy ~100, battle pools ~180, boost pools ~90, engine ~25. The load adds restore
step 0 (~350 of it GetWriteWatch) and the reverse-apply.

`[perf-bp]` splits the pool section, `[perf-sect]`/`[perf-load]` the small blob,
`[perf-restore]` the restore, `[snapshot_ring] dirty/cap` the capture, `[wwcost]`
the write-watch queries, `[mirrorpool]` the page-pool headroom.

Four changes got it there, in order of size:

*The mirror is a page pool.* Every dirty page used to be copied twice per
capture and twice per reverse-apply. `Arena::mirror` is now an array of page
indices, so capture hands the outgoing mirror page to the delta record (it IS
the pre-image) and installs a fresh one, and the reverse-apply adopts the
pre-image page as the new mirror. **1.49-1.74 → 0.85-0.88 µs per dirty page**,
normalised per page so machine drift cannot flatter it. Delta records went from
4100 bytes to 8, which also took the ring from 992 MB of committed buffers to
320 MB. `SQUIROLL_MIRRORCHK=N` audits page ownership, which is the one thing
that can go wrong and the one thing nothing else would catch.

*Boost pools, live-slot where it pays.* 1,092 KB copied whole every save and
load; only ~135 KB live, and two pools hold 448 KB with zero live slots. Now
live slots plus a bitmap — but only for stride >= 64, because saves outnumber
loads ~7:1 and whole-pool conversion lost 80 µs on save to win 342 on load.
**save 277 → 182, load 552 → 256.**

*Battle pools: canonical free list, then a hot/cold partition.* See below.

**The pool subsystem is no longer the problem.****The pool subsystem is no longer the problem.** It went from 1654 µs to ~200 µs
on save and 1157 µs to 50 µs on the load-side free-list rebuild, by two changes:

*Canonical free list.* The free list's ORDER is real state — it decides which
slot the next allocation returns and a re-simulation has to hand out the same
ones — but it is order we are free to CHOOSE, as long as both peers and both
timelines choose identically. `save()` rewrites it into ascending slot-index
order (index, not address: the index is peer-independent by construction), which
makes the list a pure function of the free SET. The blob carries a bitmap
instead of ~43,000 addresses and the load relinks with one ascending pass.

*Hot/cold partition.* `pregrow()` sizes every pool to ~2016 slots so it never has
to grow mid-match, but `[bppeak]` shows 21 of the 22 pools peak at 168 live or
fewer. Each pool now carries a high-water index `w` with the invariant that
every slot above `w` is free and still linked in the ascending chain the last
canonicalisation wrote, so the chase stops at the cold tail. Chase nodes went
43,239 -> 3,152. `w` is rolled-back state (it is in the blob) so both timelines
partition identically; three independent conditions have to hold to take the
fast path, because a false fast path would be a wrong free set — the failure
mode that got the previous incremental-tracking attempt reverted.

**The trap that blocked this for two attempts:** th155 `0x37C30` is not a plain
`Grow`. Its last three statements pop the new block's first slot and return it —
it is the allocation slow path, grow AND allocate. `pregrow()` called it to
pre-size the pools and threw the return value away, leaking one permanently-live
slot at the base of every block. Slot index runs oldest-block-first (Grow links
new blocks at the head), so those leaked slots sit at the TOP of each pool and
`maxlive` came back as exactly 992 — the newest block's base index — in all 22
pools, pinning the boundary at the end and making the partition worth 21%
instead of 7x. The IDB now calls it `TF4__TPoolAllocator__GrowAndAlloc`.

**One hazard the partition introduced, and the shape of it is worth keeping.**
The per-pool cache that holds the boundary is OUR bookkeeping and is not rolled
back, so "the cache says the cold tail is drawn at w, and the blob says w"
is NOT sufficient to reuse the tail on a restore. If the hot range was exhausted
between the save being restored and the restore itself — a round-end burst does
exactly that — the game allocated out of the cold tail, and those slots hold
live objects. Relinking the hot chain's tail onto slot w+1 then splices the free
list through a live object and hands the same memory out twice, which surfaces
as a garbage vtable and `__purecall` hundreds of frames later, on the client
only, near a round transition. `load()` now verifies the tail sentinel and its
first link before trusting it, exactly as `save()`'s chase does, and rewrites
the whole chain when either fails. Generalised: **any state derived from the
simulation that is cached outside the snapshot has to be re-validated against
memory after a restore, not against a matching version number.**

**Dual-rig A/B for the pool pass, same binary, `SQUIROLL_BPCANON=0` as the slow
arm** (55 ms delay / 20 ms jitter / 6% loss, medians over 240-save windows):

| | legacy | canonical + partition |
|---|---|---|
| save | 3353 µs | **2033 µs** |
| load | 3383 µs | **2420 µs** |
| small blob | ~2000 µs | **~690 µs** |
| restore reverse-apply | ~1290 µs | ~730 µs |

**The write-watch query was the obvious next win and it is NOT one** — but the
first explanation of why was wrong, and the corrected one is more useful.

Narrowing the cpp arena's query from 91 MB in one range to 35 MB in 21 ranges
took `getww` from 380 µs to **1344 µs**. The original conclusion — "GetWriteWatch
costs ~52 µs per call" — came from solving two equations against two
whole-system configurations. `SQUIROLL_WWBENCH=1` times the syscall directly and
says something different: on a region with **no dirty pages**, it costs about
**6 µs per call plus ~3 ns per page**, and `WRITE_WATCH_FLAG_RESET` costs the
same as a read-only query.

The live path (`[wwcost]`) is four times that:

| arena | calls | pages scanned | dirty | µs/call |
|---|---|---|---|---|
| sq | 1 | 3,637 | ~275 | 54-92 |
| bullet | 1 | 1,887 | ~7 | 23-27 |
| cpp | 2 | 23,493 | ~47 | 80-94 |

So the cost is roughly **20-25 µs fixed per call, ~3 ns per page scanned, and
~1 µs per page actually found dirty** — and the fixed part is what the bench
missed, because a clean region has no dirty bits to clear and so needs no TLB
shootdown. That is the real reason narrowing lost: the per-call flush is paid
once per range. Treat ~380 µs capture + ~370 µs step 0 as the floor. The only
remaining lever would be co-locating the three arenas in one write-watch
reservation to turn 4 calls into 1, which trades ~150 µs of call cost against
scanning the gaps between them — worth trying only if something else has already
run out.

**The dirty-page churn is diffuse — there is no targeted fix.**
`SQUIROLL_PAGEHIST` over 600 forward saves: the sq arena has **557 distinct
pages ever dirty and ~283 dirty per capture**, the hottest page is dirty in
essentially every capture, and the top 24 pages account for only **8%** of the
churn. The blocks on those pages are 44-128 byte objects of a handful of
recurring classes. So ~1 MB of Squirrel VM state genuinely changes every frame
and no single structure is responsible; the way to make it cheaper is to halve
the per-page cost (the mirror indirection), not to reduce the page count.

## Stability — and a correction to every number above it

**The "crash class past ~3100 frames" was not past 3100 frames and was not a
netcode failure.** Every stability figure quoted during this work counted it, so
they are all wrong in the same direction.

The fatal `__purecall` lands on the LAST FRAME OF THE RUN, whatever that number
happens to be. Measured across four different exit budgets:

| purecall | last frame | | purecall | last frame |
|---|---|---|---|---|
| 3210 | 3210 | | 2309 | 2310 |
| 3181 | 3180 | | 2341 | 2340 |
| 3202 | 3180 | | 3174 | 3150 |

~3200 is just where a 60 s run lands. **desync was 0 in every run**, across
dozens of runs and 2,300-3,570 frames each. The simulation is deterministic; the
crash is in teardown.

Three fixes were tried and none of them moved it: requesting a clean loop exit
instead of `ExitProcess` from inside the tick, tearing the rollback stack down
before `WM_DESTROY`, and enabling `SQUIROLL_GL_PIN`. The first two are kept as
correct hygiene regardless.

`SQUIROLL_GL_PIN` deserves its own warning: over 8 runs it looked like it halved
the crash rate (4 of 8 against 7 of 8), and when the arms were **alternated** it
showed no effect at all. That is rule 11's trap, walked into a second time in
the same body of work. Alternate the arms.

What the forensics did establish — the lead worth having — is that there are at
least two signatures and one of them is **th155's own networking thread running
on rolled-back memory**:

    __purecall: this=34A5E710 vtable=0078B680 region=sim ret=0057770A
    arena block payload=34A5E710 size=16 alloc_rva=00175E4B
    boost thread_start -> thread_start_function -> ref_TF4::UDP_
      -> reactor_process_or_initialize -> iocp_reactor_process_event
      -> init_NetworkClient_obj_40 -> __purecall

th155's boost.asio IOCP UDP reactor is neither the sim thread nor a
deterministic worker, so anything of its that lives in `cpp_arena` is rewound
underneath it by every rollback. It gets there through the pre-gate window:
before `set_sim_thread`, `arena_alloc` admits every thread, which is exactly
where another subsystem's long-lived objects get captured for the whole match.
Excluding that one alloc site did not change the rate, so it is not the whole
story.

13. **Fix the anomaly criterion before quoting another stability number.**
   `probe_stall.sh` flags a run when a peer does not log a clean exit or when
   the crash log has any fatal signature — and the end-of-run teardown crash
   trips both, in about half of all runs, regardless of what the netcode did.

## The rig

`th155_alt/probe_stall.sh <first_run> <count>` runs the two-instance rig
repeatedly, archives each run under `runs/stall_N/`, and stops at the first
anomaly (`PROBE_KEEP=1` keeps going — see rule 10). Both launcher batches use
`setlocal` so the two peers cannot share environment variables — see rule 4.

`th155_alt/probe_solo.sh [seconds] [bat]` runs the SOLO rollback stress session,
which rolls back 8 frames every frame in one process. This is where any change
to state serialisation gets developed, with `run_solo_val.bat`
(`SQUIROLL_BPVALIDATE=1`): a mistake is a validator line within seconds instead
of a cross-peer desync three minutes into a dual run. `run_solo_perf.bat` is the
same rig with the validator off, for measurement.
