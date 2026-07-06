# Sim-State Manifest (Phase 0)

**Status:** IN PROGRESS (2026-07-06) · companion to `STRUCTURAL_ROLLBACK_PLAN.md` (§4)

Goal: classify every piece of battle state the savestate touches, so the migration (§10) knows what to structuralize, what to segregate, and what to leave as clean-isolated-raw.

## Classification

| Class | Meaning | Action |
|---|---|---|
| **S** | Already structured & round-trips (battle_pools / engine_snap / rng / input / save_battle) | keep; port onto the framework (M0) |
| **R** | Sim state currently ONLY caught by the raw-page catch-all | write a structural serializer, OR clean-isolated-raw if the region is sim-pure (§8) |
| **X** | Non-sim (render / plugin / D3D / wall-clock) | segregate to render_arena / plugin domain (§9, M3); never serialize |

Method: `engine_snap` already captures the entire `.data` section (all fixed engine globals) and `battle_pools` captures the pooled TF4 objects — so the raw-page snapshot's *unique* contribution is the four arena **heaps**. Phase 0 classifies what lives in those heaps.

---

## Baseline: what is already structured (S) — authoritative, from our capture code

This is what round-trips **today** without the raw-page copy. Anything below is `S`; the arena sections that follow classify only what is NOT in this list.

### `engine_snap.cpp` — fixed regions
- **Entire th155.exe `.data` section** (RVA `0x498000`, ~293 KB) captured as committed sub-regions → *every* engine global in one shot: scheduler counters, frame counter (`0x4DACE0`), actor SetTask id (`0x4DB068`), actor serial ctr (`0x4DCEE8`), the Squirrel-instance object-pool free-list head (`0x4DCD00`), ScriptAPI/frame-driver ptrs, etc.
- **`Ew::sEffect`** (@ `*0x4DB0C8`, 0x218 B) — effect system live-groups head.
- **`Ew::sTask`** (@ `*0x4DB0B8`) — sub-regions: worker-frame counters (`+0x18014`, 0x10 B), 32× layer-task `{begin,end,cap}` triples (`+0x18024`), per-layer state, status flags (`+0x1AAC0`).
- **Battle PRNG** — Squirrel `rand()`/`srand()` via CRT rand PTD (`rng_save`/`rng_load`; restore-but-not-cross-peer-checksum).
- Keyboard state (`0x4DAF00`) — peer-local (excluded from checksum).

### `battle_pools.cpp` — TF4 `TPoolAllocator` pools (20 captured)
Actor2DManager/World2D (`0x49B370`), Actor2DProcGroup (`0x49B390`), Camera2D (`0x49B410`, render-tainted), Aura (`0x49B4F0`), cEftResChain (`0x49B590`), AnimCtrlTrail/Dynamic/Stencil/2D/3D (`0x49B5B0`–`0x49B650`), SqFunctionHolder (`0x49B630`), Actor2D (`0x49B670`), Actor2DGroup (`0x49B690`), Afterimage (`0x49B6B0`), Sensor (`0x49B6D0`), Camera3D (`0x49B6F0`, render-tainted), ActorCollisionData (`0x49B770`), EwActor (`0x49B790`), InputGlobal (`0x49B450`), InputSingle/Multi/Command (`0x49B4B0/4D0/510`, peer-local — excluded from checksum).
- **Deliberately excluded (X / netcode-owned):** render pools `0x49B2D0/310`; stage-layer pools `0x49B710/730/750` (built once at stage load); network pools `0x49B7B0/7D0/7F0` (netcode owns).
- Also `boostpool_load` — Sqrat math `boost::pool`s.

### `input` — recorder + history (`input_rec`, `input_hist`).

### `save_battle` (gekko_state.nut) — structural walk of the `::battle` Squirrel tree
Cycle-aware (`_seen`/`_next_id`), depth-limited (`_max_depth` 6), with `_skip_keys` for process-local fields. Currently emits canonical text for the **cross-peer checksum**; the reader/apply side does not exist yet (M4).

---

## Arena classifications (Phase 0 RE — filling in)

> The four sections below are produced by parallel RE agents (one per arena) classifying heap contents against the S baseline above. Merged as they complete.

### sq_arena (0x24M, Squirrel VM heap)

**What it is:** fixed-base bump + size-class allocator (`sq_arena.cpp`); the three Squirrel CRT-wrapper allocators (`0x186740/50/30`) route here only when the caller is inside the VM code range (`from_vm()`). Holds the whole Squirrel 3.0.6 object graph (`SQTable/Array/Instance/Class/Closure/FunctionProto/String/WeakRef/Outer/Generator`) for th155 battle scripts **and** squiroll plugins. Confirmed: `SQObject`=16B (SQUSEDOUBLE); **th155's Squirrel is GC-ON** (every collectable carries `_next`/`_prev`/`_uiRef`/`_sharedstate`); `SQSharedState` + SQVM exec state live **outside** the arena (`sq_vm_snap` clean-raw handles `_stack`/`_callstackdata`; `SQSharedState` is uncaptured/assumed-static).

**Authoritative restore today = raw memcpy.** With `g_arena_rollback` ON, `load_state_from_buf` restores via `sq_arena::load` and **returns before** `call_squirrel_load` — so `save_battle`/`load_battle` is currently the **cross-peer checksum emitter** (fast native `gekko_cpp_ser`) plus a **conservative apply prototype** run only in the arena-rollback-OFF fallback. The SAVE side is proven + checksummed every frame; the APPLY side is the M4 work.

| Allocation owner | Subsystem | Captured today by | Class | Plan |
|---|---|---|---|---|
| `::battle` round-machine scalars/arrays (`state`,`round`,`time`,`win`,`count`,`winner`,`match_num`,timers,flags) | Round machine | `save_battle._battle_fields` (save+apply) + raw | **S** | Round-trips in fallback; reference section. |
| `PlayerTeamData` (`::battle.team[0/1]`) — combo, gauges, target ref, phase flags | Team/combo | `save_battle` teams walk → `load_into` + raw (+ C++ core via `battle_pools`) | **S** | Drive restore via two-phase `_seen`→`_live_by_id` relink. |
| Actor instances `team[].master/.slave` **Squirrel-derived member slots** | Character sim (script) | `save_battle` depth-≤6 walk + raw | **S\*** | Extend `load_into` to write actor `_values` slots (raw, NOT via `_set`); re-bind wrapper to pooled C++ actor. |
| Actor **Sqrat/C++ payload** (pos/vel/HP/state via `_userpointer`) | Character sim (native) | `battle_pools` memcpy; `Vector3`/`InputGlobal` special-cased for checksum | **S** | Keep `battle_pools` — the model for C++ structural restore. |
| `::battle.task` table + `battleUpdate` closure + `infoActor` weakref array | Round scheduler / phase fn | **parked by-reference in `_keep` ring** (process-local) + raw | **R** | Fragile (relies on objects outliving the window). Structuralize: task instances id-recreate; closure → named-slot ref; weakrefs → ids. |
| Squirrel globals NOT under `::battle` (`::talk`, `::input_talk`, UI/menu/network singletons, script RNG wrappers) | Various script | **raw ONLY** | **R** | Root-table audit; add serializers for any battle-mutated global. Checksum oracle surfaces misses as desyncs. |
| `SQClass` defs, `_members`, `SQFunctionProto`/`SQClosure`/`_outervalues` | Script code image | raw (immutable during battle) | **R→X** (static) | Skip — recreate-on-restore moot; verify immutability. |
| Interned `SQString` bodies | VM string table | raw (bodies in-arena; `_stringtable` buckets out-of-arena) | **R→X** (static) | Skip — strings recreate when apply sets slots. |
| Live `SQGenerator`/`SQOuter` (if battle tasks `yield`) | Coroutine sim state | **raw ONLY** | **R (hard)** | Near-unserializable structurally; audit whether battle scripts suspend generators before committing. |
| Per-object GC bookkeeping (`_next`/`_prev`/`_uiRef`/`_sharedstate`) | VM GC/refcount | raw (in-arena links; `_gc_chain` **head out-of-arena**) | **X** | **Never serialize.** Recreate-on-restore re-runs `AddToChain`/refcounts. **Deletes the #27 GC-suppression hack.** |
| **Plugin Squirrel objects** — ping_display `Text` wrappers, plugin classes/tables, `rollback.nut` `last_snap`, frame/input-display HUD tables | Plugin/HUD | raw (**the bug**); `last_snap` excluded from *checksum* via `_skip_keys` | **X** | **Segregate** to a non-snapshotted Squirrel sub-domain (§9). `_skip_keys` only hides them from the checksum — they are STILL rewound by raw restore (= the crash class). Prerequisite, not a nicety. |
| SQVM exec state (`_stack`, `_callstackdata`, `_roottable`, `ci`) | VM execution | `sq_vm_snap` clean-raw, **outside arena** | **R (clean-raw, out of scope)** | Keep; verify none lands in-arena (double-cover). |
| `SQSharedState` (root ptr, registry, consts, metamethods, delegates, `_stringtable`, `_gc_chain` head) | VM shared state | **not captured** (assumed static) | **X (static)/hazard** | Immutable in battle *except* `_gc_chain` head + string-table → the #27 source. Structural sidesteps entirely. |

\* **S\*** = SAVE proven + cross-peer-checksummed; APPLY (`load_into`) runs only in the arena-rollback-OFF fallback and is conservative (in-place, no create/destroy — see below).

**save_battle completeness:** covers `::battle`-rooted state — round-machine fields (save+apply), `PlayerTeamData` (depth-6, cycle-safe `_seen`/`_next_id`→`R<id>`, sorted-key emit), actor script member fields, and the `Vector3`/`InputGlobal` C++ payloads (special-cased since Sqrat native accessors are invisible to the member walk). **Skips:** `_skip_keys` (`device_id`/`input`/`last_snap`), all non-data types (functions/closures/classes/generators → `?;`), Sqrat accessors beyond Vector3/InputGlobal (**checksum blind spots**), depth >6, and everything not reachable from `::battle` (root globals, `::talk`, plugins, generators). `task`/`battleUpdate`/`infoActor` are parked *by reference* in `_keep`, not in the blob.

**GC verdict (major):** GC internal state must **never** be serialized, and structural **designs out the #27 GC crash entirely.** Raw-page restores in-arena `_next`/`_prev` but not the out-of-arena `_gc_chain` head → `MarkObject` (0x18BBE0) follows a stale link → the GC is suppressed while armed (#27). Structural never touches GC links: objects recreate through the normal VM path, re-running `AddToChain` against the live (never-rewound) `SQSharedState`. The whole GC-mismatch class + the suppression hack disappear (retire #27 at M5).

**Restore hard problems (M4):** `load_into` today is in-place only (type-matched scalar/table/instance recurse, `R<id>`→`_live_by_id`, leaves non-matching slots alone) — it does **not create/destroy objects**. The hard cases: (1) **recreate destroyed instances** — feasible via C API but intricate; de-risk by letting `battle_pools` own C++ actor lifetime and having the Squirrel side only **re-bind `_userpointer`**; (2) **destroy not-yet-born instances** (drop refs); (3) **true two-phase deserialize** (pass 1 create/lookup all ids, pass 2 patch ref slots) — today single-pass; (4) **write raw value slots, NOT via `_set` metamethods** (native accessors have engine-corrupting side effects — the "L3 bisect crash"); use the inverse of `raw_ser`'s struct mirrors.

**Risks/open Qs:** coverage of sim globals outside `::battle` (root-table audit); Sqrat accessor fields beyond Vector3/InputGlobal (checksum blind spots); instance create/destroy (the M4 hard piece); live generator/outer state (audit before committing to full structural); `_keep` parked refs fragile if the outlive-window invariant breaks; **plugin Squirrel segregation is a prerequisite** to actually fix the crash (not just hide it from the checksum); confirm the arena/exec-state boundary (no double-cover); depth-6 cap (no deeper legit sim graph).

### cpp_arena (0x30M, C++ operator-new heap)

**Layout:** 128 MB split into a SIM region `[Meta … 112 MB)` and a RENDER region `[112 … 128 MB)` (separate bumps). `hook_op_new` routes th155's scalar `operator new` (`operator_new_0` @0x2E15AB) here when armed + caller on sim/worker/pre-gate thread; `_free_base` (@0x312347) range-routes back. **No structural serializer exists — `cpp_arena::save/load` is a flat `memcpy`.** Almost nothing here is `S`: the structured objects (Actor2D, InputCommand, Camera2D) live in the TF4 pools; cpp_arena holds their **dynamic `std::vector`/`std::list` satellite buffers**, which nothing structures.

| Allocation owner (class / site) | Subsystem | Captured today by | Class | Plan |
|---|---|---|---|---|
| **AnimationController2D/…Dynamic + Actor2DGroup `std::vector` buffers** (container growth → `operator_new_0` 0x2E15AB) | Battle sim (per-actor anim) | raw SIM copy | **R** | The **confirmed within-peer divergence root**. Pure sim, operator-new-scattered (not pooled) → hard to clean-raw; needs a serializer keyed off the actor identity map. |
| **Actor2D task-list `std::list` nodes** (`_Buynode0` 0x13F10) | Sim scheduling | raw SIM copy | **R** | Serialize alongside `battle_pools` Actor2D. |
| **tf4_objpool (Squirrel-instance) slabs** re-homed via `raw_alloc` (`tf4_objpool_pop/grow` 0x45710/0x45D20) | Squirrel VM instance native backing | raw SIM copy | **R** | Native backing of `::battle` instances. Open Q: reconstructable from `save_battle` tree, or own serializer? (see risks) |
| **Sqrat instance-binding native side** for sim objects | Squirrel↔C++ bridge | raw copy (script side also in `save_battle`) | **R** (partial S) | Native wrapper raw-only; script fields ride `save_battle`. Classify per-field. |
| **boost::signals2 connection-list nodes** (`_Buynode0` 0x13F10 via `ConnectRenderSlot` 0x65680 / `insert_connection_locked` 0x43C80) | **Render dispatch** | raw SIM copy | **X** | Largest interleaved non-sim consumer of the SIM region (~1105 nodes at f=0, ~8–10 reconnects/frame). **Cannot be re-homed by site** — the sim's dispatch list *contains* these nodes. Exclude via structural sim-only walk (M4), not routing. |
| **boost::signals2 `shared_count` control blocks** (0x31BE0/0x31C00) | Render dispatch refcount | raw SIM copy | **X** | Same coupling. `gl_pin`+self-heal are the current band-aids. |
| **DrawCommandSlot render-dispatch signals** (`…create_and_bind` 0x56A50; `…reset` 0x57DC0) | Render dispatch | raw **RENDER** region (pinned) | **X** | M3: route to render_arena so they leave the snapshot; retires `dispatch_signal_offsets`+gl_pin. |
| **Manbow::String glyph-vertex CPU vector** (String+276, 0x67270) | UI render geometry | **already → render_arena** (`glyph_reserve_hook`) | **X** ✅ | Done for th155 UI. Extend to plugin `UI.Core.Text`. |
| **Front-render pass allocations** (under `g_render_front_scriptapi` 0x49B02C / `RenderFrontPass_lookup` 0x56710) | Front render (forward-only) | **already → render_arena** (`runone_hook` brackets `front_api`) | **X** ✅ | The clean phase-based cut (see below). |
| **TF4_Number HUD digit-quad geometry** (`set_number_digit_display` 0x158A40, self+84) | HUD number geometry | raw copy, pages excluded | **X** | M3: alloc from render_arena instead of the page bitset. |
| **Trail/ribbon vertex vectors** (`generate_trail_mesh` 0x159320, Trail+168) | Motion-trail geometry | raw copy or deterministic rebuild | **X** | Route persistent vec → render_arena, or keep rebuild. |
| **Effect-group/layer-task member vectors** (`cEftGroup*`, `Ew::tEft*` via `CreateEffectGroup` 0xEBEB0 + sTask layer pools) | Visual effects (fwd-divergent) | raw copy; .data triples are engine_snap restore-but-not-checksum | **X** | Proven to spawn at diverging counts fwd-vs-resim → non-sim. Route primitive alloc → render_arena; RNG/spawn *decisions* stay in sim. |
| **Plugin HUD objects** — ping_display `UI.Core.Text` native + Sqrat wrappers | **Plugin render** | raw copy | **X** | **The #28 crash trigger** (dual-only). M3: bracket the whole plugin runtime → render_arena (native) + segregated non-snapshotted Squirrel sub-domain. |

**Sim/render split & phase-gating verdict:** the 16 MB RENDER region is 100% non-sim. The 112 MB SIM region is *majority sim by bytes* (per-actor anim/group vectors + task nodes) but **materially contaminated** by the render-dispatch signals2 graph + effect primitives — and that residue is what crashes rollback. Phase-at-alloc-time (`this` vs sim `0x49B01C` / game-loop `0x49AFBC` / front-render `0x49B02C`) is a **partial** separator: front-render routes wholesale to render_arena cleanly (done); but routing the **game-loop phase broke the sim** ("it allocates sq-referenced state too"), and the **cJobThread worker path** allocates sim `grouped_list` inserts AND render-effect nodes under one phase. **Core finding: cpp_arena's contamination is removed by _not walking_ render objects (structural, M4), not by _not co-allocating_ them (routing, M3).**

**M3 sites to route (cleanly attributable):** DrawCommandSlot 0x56A50/0x57DC0; front-render 0x49B02C (verify plugin UI); glyph 0x67270 (extend to plugin); HUD digits 0x158A40; trails 0x159320; effect primitives 0xEBEB0; **the plugin-runtime bracket (M3 headline)**. **Do NOT re-home:** signals2 nodes (0x13F10 via 0x65680/0x43C80) + control blocks (0x31BE0/0x31C00) — every prior routing attempt split a timeline-consistent structure and crashed *earlier*; exclude via M4 structural walk.

**Risks / open Qs:** (1) **Sim↔render pointer coupling is the fundamental blocker** — the signals2 residue is intrinsically an M4 problem, so M3 makes cpp_arena *mostly* (not fully) sim-pure. (2) Worker-thread interleave can't be classified by thread. (3) The **R** rows (anim/group vectors) are scattered operator-new sim state needing an actor-identity-keyed serializer before the raw copy can die. (4) tf4_objpool / Sqrat native bindings: reconstructable from the script tree or own serializer? — needs a byte-provenance pass. (5) Plugin **Squirrel** sub-domain doesn't exist yet — render_arena only segregates the native side; M3's "re-enable ping_display safely" needs both halves. (6) **cpp_arena can never be clean-isolated-raw** (§8) — it is the one arena that genuinely requires the structural walk.

### tf4A mspace (0x3AM) — **NOT the sim heap; it's the render/asset heap**

**⚠️ Premise inversion (load-bearing, evidence-backed):** the sim objects we expected here (Actor2D, effects, tasks, anim controllers, cameras, math vectors) actually live in **mspace B (0x44M)**, and B's live contents are **already captured structurally by `battle_pools`**. **mspace A is the TF4 asset/render-geometry heap** — meshes, textures, animation-sets, decoded images, audio, per-frame re-skinned vertex data — **all class X.** So tf4A is not a "extend the serializer" job; it's a **"stop snapshotting it entirely"** job (§9/M2), and a big perf win (the raw copy currently duplicates up to ~8 MB/frame of dirty render geometry).

**Evidence (raw RVAs, IDB base 0):** `tf4_mspace_create` (0x331C0) → 128MB primary at `MSPACE_A_PTR` 0x4DB414 (→0x3A M), 32MB secondary at `MSPACE_B_PTR` 0x4DB48C (→0x44 M). `BeginStreaming` (0x37C30) — which `battle_pools::pregrow` calls to grow every `TPoolAllocator` — allocates via `PoolAlloc(mspace **B**)`, so **the Actor2D/Camera/AnimCtrl/Input/effect pool blocks are in B, not A**. All 70 xrefs to `MSPACE_A_PTR` are asset/render loaders (`AnimationSet2D::Create`, `load_mesh_*`, `decode_bmp/png`, `TF4_Ogg`, resource collections); no per-frame sim allocation site. Per-frame writer into A = the mesh re-skin (`apply_transform_and_update_mesh` 0x65760 → 0x66580 → `update_mesh_vertex_buffer` 0x164D60) — which is why snapshot_ring sees tf4A dirty only ~1 page/frame. `tf4_pool.cpp` already diverts sim-thread objpool slabs to cpp_arena and render VB slabs to render_arena, so A never even receives sim-thread pool slabs. D3D-probe (2026-07-06): the driver holds NO persistent reference into A; textures are copied out at `CreateTexture2D` time from a reused decode scratch (0x3A100220), all pre-frame-0.

| Region in A | Subsystem | Captured today by | Class | Plan |
|---|---|---|---|---|
| CPU mesh vertex/index buffers, per-frame re-skinned geometry (`update_mesh_vertex_buffer` 0x164D60, re-skin 0x65760/0x66580) | Render geometry | raw-page arena-3 (excluded from checksums) | **X** | **Drop.** Re-derived every advance from actor transform ⊕ anim (both rolled back in B/sq); a headless re-sim recomputes it identically. |
| Skinned-mesh data, textures, decoded images, anim-sets, takes, resource collections (load-once loaders above) | Render/asset | raw-page arena-3 | **X** | **Drop.** Load-once, address-stable, immutable mid-match. Sim objects in B/cpp/sq hold pointers *into* A but the targets never move/free during the rolled-back window → refs stay valid without rewinding A (§7). |
| Ogg audio stream buffers (`TF4_Ogg` 0x16E250) | Audio | raw-page arena-3 | **X** | **Drop — actively harmful to capture** (audio-thread-driven, non-deterministic). |
| — *NOT in A (premise correction)*: Actor2D/Manager/Camera/Aura/AnimCtrl*/Sensor/EwActor/Input* pool blocks | Battle sim | **mspace B** → `battle_pools` | **S** | Already structural (live-slot). |
| — *NOT in A*: SqVector3/Matrix, TPrimitiveLayer/ILayer draw-tasks, Actor2D_ChildNode, effect_actor | Sim math / draw-task | tf4 mspace → `battle_pools::boostpool` | **S** | Already structural (whole-block). Open Q: confirm exact mspace of the boost singleton_pool blocks (SqVector3 = player `va/vf` velocity) — covered "wherever they live" by boostpool_save, but confirm byte-exact before dropping A. |

**Verdict:** mspace B is the *de-facto structural sim arena* (already covered by `battle_pools`; the raw B region isn't even registered — `SQUIROLL_TF4B` default OFF). mspace A is a raw catch-all of render/asset that should be **retired** — promoted to the non-sim domain alongside render_arena. The §9 goal ("route render/asset out of the sim arenas") is ~90% already true in the code, just not framed that way. This is **not** a §8 structural-vs-clean-raw decision — A needs *nothing*.

**Risks/open Qs:** (1) **Prove A holds zero sim-read-back state before dropping** — confirm the sim never reads mesh vertex data back as gameplay input (collision uses Bullet from transforms, not mesh verts — strong prior; verify via the oracle: drop arena-3 behind an env flag, run distance=10 solo + dual cross-peer, any A-resident sim state fires the desync counter). (2) Confirm exact mspace of boost singleton_pool blocks + that `boostpool_save` covers them byte-exact (the `[bp] round-trip` self-test already checks). (3) Assert no A alloc/free on the sim thread during an armed match (character-swap/stage-reload would break the address-stability assumption, but those don't happen inside `state==8`). (4) **Dropping A is a cleanliness/perf win, NOT a crash fix** — the #28 crash traced to a dual-only *plugin* in sq_arena + driver-internal D3D state, not to A. This is an M2-class "drop a pure region," gated by the soak.

### bullet_arena (0x2AM) — **Bullet physics (not LiquidFun); SIM-PURE**

**Naming correction:** th155 statically links *both* Bullet and LiquidFun/Box2D. The arena's **sole tenant is Bullet** — every allocation routes through Bullet's `btAllocFunc`/`btAlignedAllocFunc` (`0x498D44`/`0x498D4C`); all ~168 alloc + 200+ free callers are Bullet collision/physics. **LiquidFun (`b2*`) uses a different, un-hooked allocator and is NOT in this arena** (see gap below). The `snapshot_ring.cpp:327` "LiquidFun" comment conflates them.

| Physics object type | Captured today by | Class | Plan |
|---|---|---|---|
| Arena allocator `Meta` (bump + free-lists) | bullet raw + snapshot_ring arena-1 dirty-page | R (sim) | clean-raw; self-describing |
| **Broadphase** (`btAxisSweep3` / `btDbvtBroadphase` DBVT nodes, proxies) | raw arena-1 | R (sim) | clean-raw |
| Overlapping-pair cache, ghost-pair cb | raw arena-1 | R (sim) | clean-raw |
| Dispatcher + collision algorithms | raw arena-1 | R (sim) | clean-raw |
| Contact manifolds (`btPersistentManifold` via `btPoolAllocator`) | raw arena-1 | R (sim) | clean-raw |
| Collision shapes (`btBox2dShape`, convex, hull scratch) | raw arena-1 | R (sim) | clean-raw |
| Collision world / step object (World2D `task_instance`), islands/union-find | raw arena-1 | R (sim) | clean-raw |
| Constraint-solver scratch (transient, freed within step) | raw arena-1 | R (sim) | clean-raw; dirtiest arena/frame (M6 note) |
| `btAlignedObjectArray`/`btHashMap` backing | raw arena-1 | R (sim) | clean-raw |
| `Manbow::ActorCollisionData` refcount ctrl block | raw arena-1 (object itself in a battle pool = S) | R (sim) | clean-raw ctrl block; keep object in `battle_pools` |
| — *reachable but NOT in this arena* — | | | |
| `Manbow::World2D` **root** + 36×36 layer matrix | **cpp_arena** (operator new), dirty-page captured, checksum-excluded | R (sim) | co-capture/co-locate with physics for M5 (see risk 3) |
| **LiquidFun `b2World`/`b2Body`/`b2ParticleSystem`/allocator chunks** | **Act scene-physics (dormant)** — raw CRT heap via `b2Alloc`→malloc (un-hooked) | **X (cosmetic; DORMANT in shipped content)** | **RESOLVED (task #37): leave forward-only, no capture. Not a desync hole — nothing constructs it during a fight.** |

**Sim-purity verdict: PURE** for what's inside it — every caller of the arena alloc/free entry is a Bullet function; no D3D/String/signals2/plugin/camera anywhere. (The few non-physics-looking caller names — `CButton::scalar deleting destructor`, `basic_filebuf` — are FLIRT mislabels on Bullet destructors inside the module band 0x1A6000–0x1EF000; verified by decompile.) **Recommendation: keep clean-isolated-raw (§8 default holds)** — deep internal pointer graphs (DBVT trees, manifold arrays, pool free-lists), sim-pure, and the fixed-base bump arena is the ideal clean-raw substrate (single `memcpy` reproduces allocator + blocks bit-exact).

**Determinism:** **float, not fixed-point** (Bullet `btScalar`=single-precision SSE; LiquidFun `Solve` also float SSE). bullet_arena **IS cross-peer checksummed today** (`fold_checksum` folds arena-0 sq + arena-1 bullet; only arena≥2 excluded) and works — **desync=0 at e99cb80**. Raw-hashing is cross-peer-valid here (unlike cpp_arena) *because* the fixed base + deterministic bump makes every Bullet pointer byte-identical across peers; bullet holds no per-process pointers.

**Risks/open Qs:** (1) **LiquidFun `b2ParticleSystem` — RESOLVED (task #37, 2026-07-06): COSMETIC (X) and DORMANT (~95% conf).** It's a self-contained Act *scene*-physics engine (`Act::Physics2DWorldResource`, `NoConstructor`, driven by the Act layer Update `0x274120`→`b2World__Step` 0x333140, **not** `advance_one_frame`/ScriptAPI 0x49B01C), completely separate from the Bullet world that does gameplay collision (`Manbow::World2D`→`btCollisionWorld`). **No shipped content constructs it** (zero pak/script refs to `Physics2D`; gameplay particles are Squirrel `Actor2D`, not `b2Particle`), so its un-hooked allocator (`0x49AECC`/`0x49AED0`) never runs during a fight → **not a desync hole.** Only Squirrel bridge is a read-only `HitTest` on Act scene scripts. Leave forward-only; close `TODO v5` (gekko_bridge.cpp:977) as not-needed. Would flip to R only if a mod/DLC both loads a physics layer AND gates gameplay on its `HitTest`. (2) **Float determinism assumed, not enforced** — MXCSR/FTZ/DAZ not pinned up front; a deep-collision soak (many overlapping hitboxes → hull/clip float paths) is worth running. (3) **Cross-arena graph**: World2D root in cpp_arena, sub-objects in bullet_arena — clean-raw of just the physics region breaks at M5 unless the root is co-located/co-captured. (4) FLIRT mislabels inside the Bullet band should be renamed during Phase-0 so byte-provenance passes don't record phantom UI objects in the physics heap.

---

## Synthesis (all four arenas classified — 2026-07-06)

Phase 0 changed the shape of the problem in the project's favor. Three of the four snapshotted arenas do **not** need structural serialization:

### Per-arena verdict (final)
| Arena | Verdict | Milestone |
|---|---|---|
| **tf4A (0x3AM, 128MB)** | **DROP from the savestate** — it's the render/asset heap (meshes/textures/anim/audio), all class X, re-derived or load-once-immutable. The biggest arena just *leaves*. | **M2** (perf win ~8MB/frame, gated by soak) |
| **bullet_arena (0x2AM, 32MB)** | **Keep as clean-isolated-raw** — sim-pure, fixed-base, already cross-peer-checksummed and working (desync=0). §8 default confirmed. | M5 (formalize) |
| **mspace B (0x44M)** | **Already structural** via `battle_pools` — the de-facto sim-object arena. No new work. | done |
| **sq_arena (0x24M)** | **Structural** — `save_battle` SAVE is done + checksummed; the APPLY (reader) is the work. | **M4** |
| **cpp_arena (0x30M)** | **Structural, REQUIRED** — the *only* arena that can't be clean-raw (sim↔render pointer coupling). | **M3 partial + M4** |
| VM stack (out-of-arena) | Keep `sq_vm_snap` clean-raw. | done |

**The real structural work concentrates in exactly two places:** cpp_arena's **R** items (per-actor `AnimationController2D`/`Actor2DGroup` `std::vector` buffers + Actor2D task-list nodes — the confirmed within-peer divergence root) and sq_arena's **apply side** (`save_battle` reader/M4). Everything else drops, stays raw, or is already structured.

### Cross-cutting findings that refine the plan
1. **tf4A drops entirely.** Corrects the plan's "tf4A is sim" premise: sim objects are in mspace B (already structural). One of four arenas exits the savestate at M2 — the earliest, cheapest milestone — for a large perf win. *Not* a crash fix; gate on the soak.
2. **The #27 GC crash is designed out.** GC bookkeeping (`_next`/`_prev`/`_uiRef`, `_gc_chain` head) must never be serialized; structural recreates objects through the normal VM path, re-running `AddToChain` against the never-rewound `SQSharedState`. The GC-suppression hack retires at **M5**. (Fold #27 into this plan.)
3. **cpp_arena forces the structural walk to exist.** Its render residue (boost::signals2 dispatch nodes) lives *inside* the sim's per-frame dispatch list and can't be re-homed — proven by repeated crash-earlier failures. So M3 makes cpp_arena only *mostly* sim-pure; the residue is excluded by M4's sim-only walk. "Delete raw copy" for cpp_arena is gated on M4.
4. **Plugin segregation is a real prerequisite, not checksum cosmetics.** `_skip_keys` only hides plugin objects from the *checksum*; they're still rewound by raw restore (= the #28 crash class). M3 must build BOTH halves — native (render_arena) AND a non-snapshotted **Squirrel sub-domain** — to actually fix the crash.
5. ~~**NEW GAP: LiquidFun `b2ParticleSystem` is entirely uncaptured**~~ **RESOLVED (task #37): COSMETIC (X) + DORMANT.** It's a separate Act scene-physics engine (`NoConstructor`, Act-layer-driven, not the battle tick), unreferenced by any shipped pak/script content — so its un-hooked allocator never runs during a fight and it is **not** a desync hole. Leave forward-only; `TODO v5` closes as not-needed. Physics coverage is complete via bullet_arena (Bullet) alone. Flips to R only under a mod/DLC that both loads a physics layer AND gates gameplay on its `HitTest`.

### Total R inventory (sim state needing a serializer before the raw copy dies)
- cpp_arena: AnimationController2D/Dynamic + Actor2DGroup vector buffers; Actor2D task-list nodes; tf4_objpool slabs / Sqrat native bindings *(reconstructable-from-script? open Q)*.
- sq_arena: `::battle.task`/`battleUpdate`/`infoActor` (de-park from `_keep`); root-table globals mutated in battle (`::talk`, `::input_talk`, network singletons); live generators/outers *(hard — audit)*.
- bullet_arena + World2D root (cpp_arena): clean-raw, but co-capture the root (§M5 condition).

### Total X inventory (segregate; never serialize) → M3 routing
tf4A render/asset (drop wholesale) · boost::signals2 dispatch nodes+ctrl blocks (exclude via M4 walk, do NOT re-home) · DrawCommandSlot signals (0x56A50/0x57DC0→render_arena) · glyph vectors (done; extend to plugin) · front-render phase 0x49B02C (done) · HUD digits (0x158A40) · trails (0x159320) · effect primitives (0xEBEB0) · plugin HUD (native→render_arena + Squirrel sub-domain) · GC bookkeeping (never touch).

### Migration-order confirmation (no gaps)
§10's order holds, with two adjustments: **M2 gains a concrete first target (drop tf4A)**; **cpp_arena's raw-copy deletion is explicitly gated on M4** (not M3). New pre-work item: **classify LiquidFun** (blocks knowing whether physics coverage is complete). #27 folds into M5.
