# Squiroll plugins under rollback — author guide

**Status:** validated 2026-09-05 on `merge-daze-menus` (input_display and frame_bar
running on ONE peer only through ~1800 rollbacks over 90 s of 55 ms / 20 ms jitter /
6 % loss: 0 crashes, 0 desyncs). **Audience:** plugin authors (incl. Daze).

## TL;DR

Cosmetic plugins keep using the normal th155 API (`::UI.Core.Text`, `::manbow.Sprite`,
`ConnectRenderSlot` on the game's slots). They may run on one peer and not the other.
Three rules:

1. **Stay sync (the default `async = false`).** Your task is constructed in
   `battle.Create` and updated inside the re-simulated battle update, so a rollback
   rewinds and re-derives your objects together with the game's own HUD. Squiroll
   takes care of the one engine gap this exposes (see "Why this works").
2. **Never feed plugin state into the sim.** Read the sim (`::battle.team[i]`,
   actors, inputs); write only your own objects. If you *inject* a field into a sim
   object (a member added to an actor class, a slot on a team table), declare it:
   `::plugin.ExemptKey("_my_field")` at load time. That keeps it out of the
   cross-peer checksum, which is what lets the plugin run on one side only.
3. **Async plugins (`async = true`) must not touch th155 render objects.** They run
   from the forward-only loop, are never re-simulated, and a rewound
   `UI.Core.Text` they own has no re-sim to repair it. Draw through the native
   immediate-mode HUD instead: `::hud.clear()` / `::hud.text(x,y,str,r,g,b[,a][,scale])`
   / `::hud.rect(x,y,w,h,r,g,b[,a])`, re-emitted every `Update`. (ping_display is
   the reference.)

## Why this works (what squiroll does for you)

* **Plugin instances are checksum-exempt.** The plugin base class carries a
  `__squiroll_plugin` marker; the native `::battle` serializer emits any instance of
  a marked class as empty. Your task's fields can differ per peer freely.
* **`::plugin.ExemptKey(name)`** adds a member key to the serializer's skip list
  (native `::__gekko_skip_key`, bound before any script loads). frame_bar uses it for
  the `_tid` it injects into the player actor class; without it the checksum split
  at frame 0 (`s4:_tid;i0` vs `i-1`).
* **Text vertex streams self-heal (`text_vb_heal.cpp`).** A `Manbow::String` lives in
  the snapshot, but its GPU vertex-stream node (`shared_ptr<TF4::D3D11VertexBuffer>`
  at String+288) lives in an engine pool that is never snapshotted. Whenever a text
  *grew* between the snapshot and a rollback, the rewound String named a node that
  was already released or recycled: d3d11 `Map` on garbage (0xC0000094) or writes
  into someone else's buffer. th155's own HUD strings are pre-sized, so only plugin
  text hit it. Squiroll keeps a registry {node -> owner String} from the stream
  create/release hooks and, on every forward render, re-adopts the newest node the
  String itself allocated (or drops and reallocates). Nothing for plugins to do.
* Sprites, textures and render-slot connections created in your constructor are in
  the baseline snapshot and are consistent by construction.

## Interactive (non-sim) plugins

Reading the local keyboard/pad inside a sync `Update` works, but that `Update` also
runs during re-simulation, so an edge-detected toggle can fire more than once per
real frame. Prefer sampling input from a forward-only task and reading the sampled
state from the sync task (`::plugin.Input` / `InputManager` registers a `::loop`
task, which is forward-only). Verification of that path is still on the list.

## Testing your plugin under rollback (two instances, one machine)

* `th155/run_both_plug.bat` — lossy dual rig; enables input_display on the host only
  via `SQUIROLL_PLUGIN_CFG=input_display:p1.enabled=true;input_display:p2.enabled=true`
  (per-instance override of `plugin/config/<label>.ini`, format
  `label:section.key=value;...`, never written back to the .ini).
* `th155/run_both_plugfb.bat` — same rig, `SQUIROLL_PLUGIN_FORCE=frame_bar` forces a
  plugin on regardless of its `Enabled()`.
* Success = both peers reach `SQUIROLL_EXIT_SECONDS reached ... exiting clean`, no
  `DESYNC` lines, no `aocf_crash.log`. `[vbheal]` lines show stream heals (normal).
* `SQUIROLL_DIAG=1` makes a desync dump the per-frame checksum text to
  `sqtext_p{0,1}_f{N}.txt`; `cmp` them to find the differing key.

## Still open

* Async plugins are `::hud`-only (bitmap font). A richer native HUD (real font
  atlas, sprites) is the follow-up if async authors need it.
* Round-end / multi-round teardown with plugins active (Release path) is untested.
* Interactive-input sampling helper (above).
