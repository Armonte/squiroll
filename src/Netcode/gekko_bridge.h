#pragma once

#ifndef GEKKO_BRIDGE_H
#define GEKKO_BRIDGE_H 1

// GekkoNet ↔ AoCF integration.
//
// Public flow per real-time frame (called from better_game_loop instead of
// the unconditional update_logic/window_render pair):
//
//   bool gekko_bridge_tick();
//     1. gekko_network_poll
//     2. capture local input bits from Manbow::InputRecorder (already
//        intercepted by SyncInput_hook in netcode.cpp)
//     3. gekko_add_local_input for each local player
//     4. drain gekko_session_events for connection/desync notifications
//     5. drain gekko_update_session events:
//          GekkoSaveEvent    → write state via save_state_to_buf
//          GekkoLoadEvent    → restore via load_state_from_buf
//          GekkoAdvanceEvent → push gekko-supplied inputs into the input
//                              source then call advance_one_frame()
//     6. call render_one_frame() exactly once (after all events drained)
//     return true if frame advanced normally, false if we should skip render
//
// State-blob layout (versioned, all little-endian):
//   uint32 magic = 'GKAF'
//   uint32 version
//   uint32 frame
//   uint32 rand_state                    (acrt_ptd->rand_state @ 0x319663_R)
//   uint32 actor_count
//   <Actor2D × N>                        (0xEC bytes each, raw memcpy)
//     + per-actor: 5 SQObject flag1..5  (sq_addref pinned)
//   <Squirrel diff buffer>               (from rollback.cpp's FrameUndo)
//   <Box2D/LiquidFun b2ParticleSystem>   (TBD; b2World @ ManbowWorld2D[0x20])
//
// Estimated size at 200 actors: ~60 KB raw + diffs ≈ <200 KB. Well within
// rollback budget.

#include <stdint.h>

// Forward decls — don't pull gekkonet.h into hot path headers.
struct GekkoSession;

namespace gekko_bridge {

// Lifecycle (call from common_init, after Squirrel + Netcode are up).
bool init(uint16_t local_port, uint16_t remote_port,
          uint8_t local_player_idx, const char* remote_ip);
// Single-process stress session: a GekkoStressSession with both players
// local and no networking. It rolls back `check_distance` frames every
// frame, so the full save/load path is exercised hard in one instance —
// the rig for iterating on rollback determinism and perf without two
// processes. Assumes the battle is ALREADY created (vs.Initialize has
// run) — it just creates the session and takes the frame loop.
bool init_solo();

// Deferred-arm: boot.nut runs vs.Initialize normally so the round-start
// intro plays under the vanilla loop, then calls one of these.
// pre_arm_poll() watches battle.state and creates the gekko session the
// instant Round_Fight begins — so gekko frame 0 is fight frame 0 and the
// rollback never touches the intro. _solo arms a GekkoStressSession (no
// handshake); _dual arms a GekkoGameSession with the given peer params.
void watch_for_fight_solo();
void watch_for_fight_dual(uint16_t local_port, uint16_t remote_port,
                          uint8_t local_idx, const char* remote_ip);
void pre_arm_poll();

void shutdown();
bool is_active();
bool menu_mash_active();          // session exists; UDP poll runs in background
bool is_session_started(); // GekkoSessionStarted fired; tick() owns the frame
// GekkoNet time sync: how far ahead of the remote this peer is. The game loop
// stretches its frame period slightly while this is > 0.5, so the peer that is
// ahead does not absorb every mispredict. 0 when there is no live session.
float frames_ahead();
bool is_holding_transition(); // round-end: engine held until the remote reaches the latch
void hold_poll();             // per-frame pump while holding (render-only frames)
void hold_vanilla_feed();     // arm-handshake hold: keep the vanilla input step alive for the peer
// True when a DUAL (2-peer) gekko session owns input replay via the recorder
// queue + reader rebind. input_global_sync's capture-replay MUST stand down
// then: on a mispredicted rolling peer it would restore the forward-captured
// (predicted, wrong) InputGlobal state over the corrected re-sim decode.
bool dual_input_owned();

// Per-frame entry. Returns true if the visible frame was advanced.
// Pre-SessionStarted: drains network/session events only — engine runs
// vanilla update_logic via better_game_loop. Post-SessionStarted: takes
// ownership of the frame counter; every Save/Load/Advance event is
// processed (gekko's rollback model assumes events are never skipped).
bool tick();

// Inputs pulled by Gekko (set during AdvanceEvent handling, read by
// the SyncInput_hook override in netcode.cpp).
extern uint16_t forced_inputs[2];
extern bool     forced_inputs_active;

// State serialization hooks (implemented in gekko_bridge.cpp).
// Returns bytes written. The buffer is owned by Gekko; size is configured
// via GekkoConfig::state_size at init. `frame` is the Gekko frame number
// — stored in the header and used to key __gekko_state._keep.
uint32_t save_state_to_buf(void* buf, uint32_t cap, uint32_t* out_checksum,
                           uint32_t frame);
void     load_state_from_buf(const void* buf, uint32_t len);

// Solo fast-forward multiplier: number of tick()s better_game_loop should
// run per rendered frame. 1 normally; >1 when the solo stress session is
// active and the turbo key is held.
int turbo_ticks();

// Drive one logic-frame without rendering. Calls update_related(...) +
// Act::ScriptAPI->Update() directly, bypassing the screenshot-key polling
// in update_logic.
void advance_one_frame();

// Drive one render (only called after all rollback resim is done).
void render_one_frame();


// Bind ::__gekko_skip_key on the root table (root must be on the stack top).
void register_skip_key_native(void* vm);   // HSQUIRRELVM; root table on stack top

// Diagnostic (SQUIROLL_HEAPCHECK=2): validate every process heap at a named
// phase boundary. Used by better_game_loop to bracket heap damage to the
// RENDER pass vs the sim advance — the 0xC0000374 the D3D driver reports is
// raised inside the render pass, after the per-advance check has already
// passed. No-op unless the heap check is enabled.
void heap_scan_phase(const char* when);
} // namespace gekko_bridge

#endif // GEKKO_BRIDGE_H
