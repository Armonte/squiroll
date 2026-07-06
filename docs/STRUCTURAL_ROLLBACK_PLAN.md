# Structural Serialization Rollback — Design & Migration Plan

**Status:** proposal / not started · **Owner:** squiroll netcode · **Audience:** squiroll + plugin authors (incl. upstream/Daze)

This document plans the replacement of squiroll's raw-memory-snapshot rollback core with **structural serialization of the battle sim state**. It exists because the current approach has a structural flaw that no amount of patching fixes, and because a robust rollback is a prerequisite for a rollback-safe *plugin* ecosystem.

---

## 0. The invariant

> **A savestate must contain only deterministic sim state. Anything that is a function of non-sim input — render objects, plugin HUD, wall-clock, network stats, D3D driver resources — must never be in the savestate, by construction, not by exclusion.**

Everything below is in service of enforcing this one rule at the architectural level instead of hunting violations one at a time.

---

## 1. Why the raw-page snapshot is a dead end

The current core (`snapshot_ring.cpp`, 1403 LoC) captures **raw memory pages** of the whole battle heap — `sq_arena` (0x24M), `bullet_arena` (0x2AM), `cpp_arena` (0x30M), `tf4A` mspace (0x3AM) — and `memcpy`s them back on restore. Those arenas are *shared*: the same heap holds deterministic game-logic state **and** forward-only render/plugin state:

- `Manbow::String` glyph-vertex CPU buffers
- `TF4::D3D11VertexBuffer` control blocks (the object the async D3D driver dereferences)
- `boost::signals2` render-dispatch connection nodes
- camera-follow / interpolation caches
- **every plugin's HUD state** (ping_display's `Text` objects, etc.)

When rollback rewinds the arena, it rewinds all of that too. But the re-sim never rebuilds it (it isn't a function of the sim), so it goes stale and corrupts the render path / D3D driver (the `nvwgf2um` exec-at-heap crash, #28).

**The evidence this is structural, not a bug:**

- Solo rollback (which rewinds *harder* and more often) never crashed; **dual-only** did — because the only difference is a plugin (ping_display) that solo never loads. The crash tracks *plugin state in the savestate*, nothing else.
- Every "fix" to date is an *exclusion*: `desync_registry`, `engine_snap` carve-outs, the render-region head exclusions, `gl_pin`, and finally `render_arena`. Each removes one leaked object. The list has no end: it grows with every render feature and every plugin.
- Daze's own read: ping_display is the *simplest* plugin and it already broke the snapshot; the more intensive plugins (even offline-only ones, should they ever run under rollback) are guaranteed to.

The raw-page snapshot is doing the wrong thing *correctly*: it faithfully captures a heap that was never meant to be captured wholesale.

---

## 2. Current architecture (what we actually have today)

The savestate is already a **hybrid** — it is not purely raw pages. From `gekko_bridge.cpp` save/restore:

| Section | Component | Kind | Contents |
|---|---|---|---|
| Raw pages | `snapshot_ring::capture/restore` | **raw memcpy** | sq/bullet/cpp/tf4A arenas — *sim + render mixed* (the problem) |
| Pools | `battle_pools::save/load` | structured | TF4 `TPoolAllocator` pools (Actor2D, InputCommand/Multi/Single, …) |
| Boost pools | `battle_pools::boostpool_load` | structured | Sqrat math `boost::pool`s |
| Engine | `engine_snap::save/load` | structured | fixed `.data` regions + heap-chased (effects, sTask, scheduler) w/ render-cache carve-outs |
| Input | `input_rec_load` / `input_hist` | structured | input recorder + history |
| RNG | `engine_snap::rng_save/load` | structured | `Ew::sRandom` SFMT state (restore-but-not-checksum) |
| Checksum | `call_squirrel_save` → `save_battle` | **structural serialize** | canonical text walk of `::battle` for cross-peer desync detection |

Two facts fall out of this table and shape the whole plan:

1. **We already do targeted structured capture** of the most important sim subsystems (pools, engine regions, RNG, input). The raw-page snapshot is the *catch-all* for "everything else in the arenas we haven't structured yet."
2. **`save_battle` already structurally serializes the Squirrel `::battle` tree** — with cycle handling (`_seen`/`_next_id`), depth limits, and an explicit `_skip_keys` set for process-local (non-rollback) fields. It emits *canonical text* today (for hashing), but it is a real, working traversal of the sim's script state.

So "Plan A" is not a from-scratch rewrite. It is: **grow the structured captures to total coverage, convert `save_battle` from a hash-emitter into a save/restore serializer, and delete the raw-page snapshot.**

---

## 3. Target architecture

```
                    ┌───────────────────────────────────────────┐
  SAVE(frame) ─────▶│  sim-state serializers (registry)         │──▶ compact versioned blob
                    │   • squirrel battle tree  (save_battle²)   │      (ring-buffered per frame)
                    │   • C++ engine pools      (battle_pools)   │
                    │   • effect/task system    (engine_snap)    │
                    │   • RNG / scheduler / input                │
                    │   • physics (see §8)                       │
                    └───────────────────────────────────────────┘
                              ▲ walks ONLY sim state
                              │
     render / plugin / D3D / wall-clock  ─── never walked, never in the blob ───▶ forward-only, untouched

  RESTORE(frame) ──▶ same registry, deserialize + APPLY back into live structures
```

- The blob contains **only** sim state. There is nothing to exclude because nothing else is ever visited.
- Render/plugin/D3D live in their own domain (`render_arena` or plain forward-only heap) and are simply never enumerated.
- The cross-peer structural checksum is computed **from the same serializers** — so "what we hash" and "what we restore" are provably the same set (kills the class of "hashed but not restored" / "restored but not hashed" bugs we've hit).

---

## 4. Phase 0 — the sim-state manifest (do this first, it de-risks everything)

Before writing a serializer we produce a **complete manifest** of deterministic battle state: every field, its owner, its type, and its current capture path. This is mostly *consolidation* — the knowledge is already spread across `engine_snap`, `battle_pools`, the desync registry, and the IDB — plus a bounded RE effort to close the gaps the raw-page snapshot currently papers over.

**Deliverable:** `docs/SIM_STATE_MANIFEST.md` — a table keyed by memory region, each row tagged:
- `S` structured already (pools/engine/rng/input/save_battle)
- `R` currently only covered by the raw-page catch-all → **must be classified**: is it sim (→ write a serializer) or non-sim (→ segregate out)?
- `X` known non-sim (render/plugin) → segregate

**Method:** diff the raw-page arena contents against the structured captures. Anything in the arenas that the structured captures *don't* touch is either (a) sim state we've been getting "for free" from the raw copy, or (b) render/plugin garbage. A byte-provenance pass (which allocation sites populate the unstructured arena bytes) classifies each. The IDB + `ida-pro-mcp` gives us allocation-site → owner; the desync byte-diff stash already localizes which bytes actually change between frames.

This phase is agent-parallelizable (one agent per arena / subsystem, as we did for the render-object RE) and its output is the backbone of the migration order in §10.

---

## 5. Serialization framework

A small, versioned, self-describing framework shared by save, restore, **and** checksum:

- **Writer/Reader**: length-prefixed sections, each with a `(tag, version, size)` header. Unknown/newer tags are skippable (forward-compat for plugin-authored serializers, see §12).
- **Section registry**: each subsystem registers `{tag, save(Writer&), restore(Reader&), checksum(Hasher&)}`. `save_state_to_buf`/`load_state_from_buf` become "iterate the registry," which they nearly are already (`sect(&fn)` chain).
- **Determinism contract**: a serializer may only read/write sim state; it must be pure w.r.t. wall-clock/network. Enforced by review + the checksum oracle (§11), not by faith.
- **One source of truth**: `checksum()` walks the same fields `save()` writes. In debug builds, assert `hash(save()) == checksum()` so drift is caught immediately.

`save_battle` becomes the reference implementation of this contract for the Squirrel tree: today it has the writer (canonical emit) and the skip-set; we add the **reader/apply** and route its output through the framework instead of a text buffer.

---

## 6. The restore/apply side (the hard half — be honest about it)

Serializing (read fields → bytes) is the easy direction and we largely have it. **Deserializing (bytes → write fields back into live objects)** is the real work and the main risk. Per subsystem:

- **Squirrel battle tree**: reading `::battle` is done; *writing* it back means setting table/array/instance slots to saved values, recreating instances that were destroyed, and re-establishing identity for shared references (§7). This is the largest single new piece. Squirrel's C API (`sq_pushobject`, slot set, `sqstd` helpers) + Sqrat gives us the primitives; the challenge is completeness and instance re-identification.
- **C++ engine pools** (`battle_pools`): already round-trips (save+load exist). Keep as-is; it's a model for the pattern.
- **Effects/tasks** (`engine_snap`): already round-trips. Extend to cover any effect state currently riding the raw-page copy (Phase 0 output).
- **RNG / input / scheduler**: already round-trip.
- **Physics** (`bullet_arena`): see §8 — likely stays a *clean isolated raw snapshot*, which is fine under the invariant as long as that region is sim-only.

The migration order (§10) is deliberately chosen so the hard Squirrel-apply piece lands **last**, after the mechanical framework and the already-round-tripping subsystems are proven.

---

## 7. Pointers, handles, and identity

Raw snapshots preserve pointers trivially (addresses don't move). Structural serialization must not serialize raw pointers — it serializes **identity** and re-links on restore:

- **Assign stable IDs** to every sim object that can be cross-referenced (actors, effects, tasks, script instances). `save_battle` already does exactly this (`_seen` map → `_next_id`, `_live_by_id` for restore-time lookup) — generalize it.
- **Serialize references as IDs**, not addresses. On restore, build an `id → live object` map in a first pass, then patch references in a second pass (classic two-phase deserialize).
- **Object lifetime**: restoring a frame where object X existed but X was since freed means *recreating* X (allocate from its pool, assign the saved ID). Restoring a frame where X did *not* yet exist means *destroying* the live X. The pool allocators (`battle_pools`) make this tractable for C++ actors; the Squirrel side needs instance create/destroy through the VM.
- **Handles to non-sim resources** (a `Text`'s D3D vertex buffer, a font resource): these are **not serialized**. On restore the sim object holds a reference that the *forward render pass* will lazily rebuild — because those resources live in the non-snapshotted domain and were never rewound. This is the crux of why structural fixes the crash: the stale-handle problem disappears when the handle's target was never in the savestate to begin with.

---

## 8. The genuinely-hard subsystems

Not everything is worth serializing structurally. The invariant permits a **clean isolated raw snapshot** of a region *provided that region contains only sim state*. Decision per subsystem:

| Subsystem | Approach | Rationale |
|---|---|---|
| Squirrel `::battle` tree | **structural** | mixes sim + must be cross-peer-canonical anyway; `save_battle` foundation exists |
| C++ actor pools | **structural** (done) | already round-trips; pool identity is clean |
| Effects / tasks | **structural** (done, extend) | already round-trips |
| RNG / input / scheduler | **structural** (done) | tiny, fixed |
| **Physics (LiquidFun/Bullet)** | **clean isolated raw** | bodies have deep internal pointer graphs; structural serialization is high-cost/low-value *if* `bullet_arena` can be made sim-only. Verify no render/plugin allocs land there (Phase 0), then raw-snapshot just that region. |
| **Squirrel VM exec state** (stack/frames) | **clean isolated raw** (existing `sq_vm_snap`) | malloc'd outside arenas; already handled; keep as an isolated sim-only capture |

The point: "Plan A" ≠ "structural-serialize literally everything." It's "**the savestate contains only sim state**," achieved by structural serialization where the state is entangled with non-sim (the arenas) and clean isolated raw snapshots where a region is already sim-pure (physics, VM stack). Both honor the invariant.

---

## 9. Non-sim segregation (bounding the walk)

For structural serialization to be *complete and cheap*, sim and non-sim allocations should be physically separated so the serializer's walk is bounded and the clean-raw regions stay pure:

- **Plugins**: squiroll owns the plugin runtime, so wrap all plugin execution so plugin allocations land in `render_arena` (native side) and a segregated non-snapshotted Squirrel sub-domain (script side). Plugin state then never appears in `::battle`'s serialized subtree (the `_skip_keys` mechanism generalizes to "the plugin domain").
- **th155 render objects** (`Manbow::String` glyphs, VB control blocks): route to `render_arena` (the existing hooks are the seed of this) so they leave the snapshotted arenas entirely — which *also* shrinks the sim arenas toward sim-only, making the eventual clean-raw fallback for any residual C++ state safe.

`render_arena` is therefore not throwaway scaffolding — it's the **non-sim domain** of the target architecture. What changes is intent: it stops being "the place we move things after they crash" and becomes "where all forward-only state is allocated by policy."

---

## 10. Migration plan (incremental, always shippable, oracle-gated)

We never do a big-bang cutover. We run structural and raw **side by side**, cutting over one subsystem at a time, with the cross-peer checksum + determinism soaks as the gate at every step.

- **M0 — Framework + manifest.** Phase 0 manifest (§4) + the serialization framework (§5) landed with the existing structured sections ported onto it (no behavior change). *Ship: identical to today.*
- **M1 — Dual-write + verify.** For each already-structured subsystem, additionally compute its structural checksum every frame and assert it matches the raw-page bytes for its region. Flushes out hidden coupling. *Ship: identical to today, with debug asserts.*
- **M2 — Cut over the pure subsystems.** RNG, input, scheduler, effects, pools already round-trip — drop them from the raw-page set (raw snapshot stops copying those regions). Validate with distance=10 soaks + cross-peer. *Ship: smaller raw snapshot, same behavior.*
- **M3 — Segregate render + plugins (§9).** Route all render/plugin allocation out of the sim arenas into `render_arena`/plugin-domain. Now the sim arenas trend sim-only. *Ship: the rollback-safe plugin domain — this alone re-enables ping_display and any HUD plugin safely, delivering value before Squirrel-apply is done.*
- **M4 — Squirrel structural round-trip.** Build `save_battle`'s reader/apply (§6, §7). Cut `::battle`'s script state over from raw pages to structural. This is the big one; it lands after everything mechanical is proven. Validate hard.
- **M5 — Delete the raw-page snapshot.** Whatever sim-pure regions remain (physics, VM stack) become explicit clean isolated raw captures (§8). `snapshot_ring`'s whole-arena copy is removed. The invariant now holds by construction.
- **M6 — Perf pass.** Structural save is likely *cheaper* than the current ~8.7ms raw-page copy (we stop copying tens of MB of arena), but validate and optimize the Squirrel walk (it's the hot path). Ties into existing task #25.

At every milestone the build is shippable and the netcode works; we are only ever *removing* regions from the raw snapshot as their structural replacement is proven.

---

## 11. Verification & the completeness oracle

The reason this is *tractable* despite the scope: **we already have the oracle.** The cross-peer structural checksum (`save_battle` → desync detection) tells us, every frame, whether two independent peers agree on the serialized sim state. That is exactly the completeness test for structural serialization:

- If the structural blob is **incomplete** (missed some sim state), determinism breaks and the desync counter fires — loudly, in the existing soaks.
- If the structural blob **over-captures** (grabbed non-sim state), cross-peer diverges immediately (per-process render pointers differ) — also caught.

So the migration is guarded by a test we already run. Add:
- **Round-trip identity assert**: `restore(save(S)) == S` for each subsystem (structural, in debug).
- **Save/checksum agreement assert**: `hash(save()) == checksum()` (§5).
- The existing **distance=10 solo soak** and **dual cross-peer soak** as the acceptance gate per milestone.

---

## 12. Plugin API payoff (why this matters beyond fixing a crash)

Once the invariant holds, we can offer plugin authors a real contract — the thing this whole effort is *for*:

- **Forward-only by default**: a plugin's HUD/render state lives in the non-sim domain and is never rolled back. Plugins like ping_display "just work" with zero rollback awareness.
- **Opt-in rollback-safe state**: a plugin that *does* need per-frame sim-correlated state (e.g. a combo counter that must rewind) registers a serializer with the framework (§5) — a small, documented `save/restore/checksum` triple. Its state then rides the savestate correctly, cross-peer-checksummed like everything else.
- **The godfather move**: we publish this as *the* squiroll rollback plugin API, so the ecosystem (and upstream) builds on a foundation that can't silently corrupt the savestate. Daze's "the rest will cause problems" stops being true.

---

## 13. Risks / open questions / decision points

- **Squirrel-apply completeness** is the top risk. Mitigation: the checksum oracle catches incompleteness immediately; M4 lands last on a proven base; instance re-identification reuses the `_seen`/`_live_by_id` machinery already in `save_battle`.
- **Physics: structural vs clean-raw.** Decision deferred to Phase 0 evidence (is `bullet_arena` sim-pure?). Default: clean-raw.
- **VM exec state**: keep `sq_vm_snap` as clean-raw, or structuralize? Default: keep raw (small, isolated, already works).
- **Perf of the Squirrel walk** at rollback depth (M6, task #25). Likely a net win vs raw-page copy but must be measured.
- **Effort**: this is the largest single piece of the rollback project — realistically multi-week for M0–M3 (which already delivers the plugin-safe payoff) and a further chunk for M4–M5. M3 is the natural "ship a real win, reassess" checkpoint.

---

## 14. Relationship to existing tasks

- Supersedes the per-object exclusion approach behind #28/#30 (render/plugin garbage) — those become "verify segregation" instead of "hunt the leak."
- #25 (save-time perf) folds into M6.
- #26 (strip debug scaffolding) — the `d3d_probe`/watchpoint scaffolding retires as the crash class is designed out.
- #23 (harden mod-plugin round-end guards) becomes the **plugin API** work (§12) instead of ad-hoc guards.
- The `render_arena` (already built) is promoted from scaffolding to the non-sim domain (§9).

---

## Phase 0 outcomes (2026-07-06 — see `SIM_STATE_MANIFEST.md` for the full manifest)

Four parallel RE agents classified the four snapshotted arenas. The problem came out **smaller and better-shaped** than this plan assumed:

- **tf4A (0x3AM, the biggest arena) is render/asset, not sim → it DROPS from the savestate at M2.** The sim objects are in mspace B, already structural via `battle_pools`. Corrects §4/§8's "tf4A is sim" premise. Large perf win (~8MB/frame), gated by the soak; not a crash fix.
- **bullet_arena is Bullet (not LiquidFun), sim-pure → stays clean-isolated-raw** (§8 default confirmed; already cross-peer-checksummed, desync=0).
- **cpp_arena is the ONE arena that must be structural** — sim↔render pointer coupling (signals2 dispatch nodes inside the sim's dispatch list) means its render residue is excluded by the M4 walk, not re-homed. M3 makes it only *mostly* sim-pure; raw-copy deletion is gated on M4.
- **sq_arena: `save_battle` SAVE is done + checksummed; the APPLY (reader) is M4.** Plugin **Squirrel** segregation is a real prerequisite (§9/M3), not checksum cosmetics.
- **The #27 GC crash is DESIGNED OUT** — structural never touches GC bookkeeping; the suppression hack retires at M5.
- **NEW GAP: LiquidFun `b2ParticleSystem` is entirely uncaptured** on an un-hooked CRT allocator — invisible to the checksum. Must be classified (cosmetic-X vs gameplay-R) before physics coverage can be called complete.

**Net:** the real structural work concentrates in two spots — cpp_arena's `AnimationController2D`/`Actor2DGroup` vector buffers + Actor2D task nodes (the **R** items), and sq_arena's `save_battle` reader (M4). Migration order §10 holds; M2 gains a concrete first target (drop tf4A).

### TL;DR of the plan
Grow the structured serializers we already have (`battle_pools`, `engine_snap`, `save_battle`) to total sim coverage, physically segregate all render/plugin state into a non-snapshotted domain, convert `save_battle` from a hash-emitter into a save/restore serializer, and delete the raw-page arena copy — migrated one subsystem at a time, gated at every step by the cross-peer checksum oracle we already run. The payoff is a savestate that contains only sim state *by construction*, ending the whack-a-mole permanently and giving plugin authors a rollback-safe API.
