// GekkoNet bridge — skeleton. See gekko_bridge.h for the public API and
// design notes. Each TODO below is a concrete next step.

#include <stdint.h>
#include <string.h>
#include <windows.h>
#include <unordered_map>

#include "gekko_bridge.h"
#include "patch_utils.h"
#include "netcode.h"
#include "input_session_layout.h"
#include "util.h"
#include "log.h"
#include "Actor2D.h"
#include "live_actors.h"
#include <squirrel.h>
// squiroll routes every sq_* call through a runtime-filled KITE table —
// without this header the bare sq_pushroottable etc. show up as undefined
// linker symbols.
#include "kite_api.h"

#define GEKKONET_STATIC
#include <gekkonet.h>

// --- Engine entry points (RVAs in canonical scheme, used with _R macro) ---

// Per-frame logic update: runs update_related(dword_AFB01C) +
// Act::ScriptAPI_ptr->vftable->Update(...). update_logic also polls the
// PrintScreen key — fine to skip during rollback resim.
typedef void cdecl update_logic_t();
#define update_logic ((update_logic_t*)0xE1A0_R)

typedef void thiscall update_related_t(void* this_);
#define update_related ((update_related_t*)0x2FAD0_R)

typedef char cdecl drawing_related_t();
#define drawing_related ((drawing_related_t*)0xE330_R)

// Two Act::ScriptAPI globals (verified in IDA):
//   0x49B01C = g_main_scriptapi      — the MAIN game/battle ScriptAPI;
//              update_logic() pumps it on the main thread.
//   0x49AF8C = g_bg_thread_scriptapi — the BACKGROUND ScriptAPI;
//              ScriptAPI_BackgroundThreadLoop (0x2F2A0) pumps it on a
//              worker thread. better_game_loop's no_input_thread_patch
//              NOPs that thread, so we drive it once per real frame.
// update_related (0x2FAD0) IS Act::ScriptAPI::RunOneFrame(this).
#define MAIN_SCRIPTAPI_PTR     ((void**)0x49B01C_R)
#define INPUT_UPDATE_LIST_PTR  ((void**)0x49AF8C_R)

// Act::ScriptAPI dispatcher pointer — drives the Squirrel root frame.
struct ScriptAPI_vtbl {
    void* field_0;
    void* field_4;   // render preprocess
    void* field_8;   // render
    void* field_C;
    void (thiscall* Update)(void* self);  // ::loop pump that drives the Squirrel side
};
struct ScriptAPI { ScriptAPI_vtbl* vftable; };
// Verified via IDA list_globals: g_Act_ScriptAPI_ptr is at IDA 0x8DACFC
// → RVA 0x4DACFC. The prior 0xB3ACFC value resolved to a data slot well
// past the .data segment and read NULL, crashing in advance_one_frame.
#define Act_ScriptAPI_ptr (*(ScriptAPI**)0x4DACFC_R)

// MSVC CRT per-thread data — holds the rand() seed at offset 0x24. Same
// definition as rollback.cpp uses; not pulled in via a shared header
// because rollback.cpp's struct lives inside that translation unit.
struct ACRTThreadData {
    char     pad0[0x24];
    uint32_t rand_state;     // 0x24 — read/written by rand() / srand()
    char     pad28[0x364 - 0x28];
};
typedef ACRTThreadData* stdcall acrt_getptd_t();
#define acrt_getptd ((acrt_getptd_t*)0x319663_R)

namespace gekko_bridge {

// ------------------------------------------------------------------ state --

static GekkoSession* g_session = nullptr;
static uint8_t       g_local_idx = 0;
static bool          g_active          = false; // session exists; UDP handshake can run
static bool          g_session_started = false; // SessionStarted fired + vs.Initialize done;
                                                 // gekko owns the frame counter
static bool          g_solo            = false; // single-process GekkoStressSession:
                                                 // both players local, no networking
static bool          g_watch_for_fight = false; // solo: armed by boot.nut, pre_arm_poll
                                                 // creates the session at Round_Fight
static uint32_t      g_evt_trace = 0;            // diagnostic: # of Save/Load/
                                                 // Advance events to trace with
                                                 // engine count. 0 = off (set
                                                 // non-zero only when debugging
                                                 // the count/desync path)

uint16_t forced_inputs[2] = {0, 0};
bool     forced_inputs_active = false;

// ----------------------------------------------------------------- helpers --

// 0x169D80 = TF4InputDevice::PollState (thiscall taking the
// TF4InputDeviceState* and returning the packed input bits). Same target
// SyncInput_hook calls — we just invoke it directly when armed because
// the vanilla SyncInput path that updates g_last_local_input_bits stops
// firing once we route through gekko_bridge.
typedef uint16_t (thiscall *poll_input_state_t)(TF4InputDeviceState*);
#define poll_input_state ((poll_input_state_t)0x169D80_R)

static uint16_t read_local_input_bits() {
    // Armed: poll the device directly each tick. The vanilla
    // SyncInput_hook isn't on the call path anymore so g_last_local_input_bits
    // would be stuck at its last pre-arm value (typically 0 from the
    // countdown), which is what was making both players idle after ARM.
    if (g_active_input_session &&
        g_active_input_session->local_input)
    {
        return poll_input_state(&g_active_input_session->local_input->state);
    }
    // Fallback: cached pre-arm value.
    return g_last_local_input_bits;
}

static uint32_t fletcher32(const uint8_t* data, size_t len) {
    // TODO: use whatever checksum GekkoNet's desync detector prefers.
    uint32_t a = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) a = (a >> 8) ^ (a + data[i]);
    return a;
}

} // namespace gekko_bridge — temporarily close so the extern is global

// plugin.cpp owns the live HSQUIRRELVM as a translation-unit global; we
// share access via extern. The symbol is top-level (no namespace).
extern HSQUIRRELVM v;

namespace gekko_bridge {

// ----------------------------------------------------------- squirrel hook --

// Call ::__gekko_state.save_battle() and copy its returned string into
// out (up to cap bytes). Returns bytes written; 0 if anything failed
// (the Save event then has empty squirrel data — still consistent).
static uint32_t call_squirrel_save(uint8_t* out, uint32_t cap, uint32_t frame) {
    if (!v) return 0;
    SQInteger top0 = sq_gettop(v);
    sq_pushroottable(v);
    sq_pushstring(v, _SC("__gekko_state"), -1);
    if (SQ_FAILED(sq_get(v, -2))) { sq_settop(v, top0); return 0; }
    sq_pushstring(v, _SC("save_battle"), -1);
    if (SQ_FAILED(sq_get(v, -2))) { sq_settop(v, top0); return 0; }
    // stack: root, gekko_state, save_battle
    sq_push(v, -2);  // `this` = gekko_state table
    sq_pushinteger(v, (SQInteger)frame);  // save_battle(frame) — keys _keep
    if (SQ_FAILED(sq_call(v, 2, SQTrue, SQTrue))) { sq_settop(v, top0); return 0; }
    const SQChar* sqstr = nullptr;
    if (SQ_FAILED(sq_getstring(v, -1, &sqstr)) || !sqstr) {
        sq_settop(v, top0);
        return 0;
    }
    size_t n = strlen(sqstr);
    if (n > cap) {
        // Truncating the serialized blob mid-string corrupts the
        // length-prefixed format → deserializer slice-out-of-range
        // crashes. Refuse: emit empty + loud log so we bump state_size.
        log_printf("[gekko_bridge] !! squirrel blob OVERFLOW: %zu bytes > %u cap. "
                   "Bump GekkoConfig::state_size.\n", n, cap);
        sq_settop(v, top0);
        return 0;
    }
    memcpy(out, sqstr, n);
    sq_settop(v, top0);
    return (uint32_t)n;
}

static void call_squirrel_load(const uint8_t* data, uint32_t len, uint32_t frame) {
    if (!v || len == 0) return;
    SQInteger top0 = sq_gettop(v);
    sq_pushroottable(v);
    sq_pushstring(v, _SC("__gekko_state"), -1);
    if (SQ_FAILED(sq_get(v, -2))) { sq_settop(v, top0); return; }
    sq_pushstring(v, _SC("load_battle"), -1);
    if (SQ_FAILED(sq_get(v, -2))) { sq_settop(v, top0); return; }
    sq_push(v, -2);  // this
    // Squirrel strings are null-terminated; the serializer produces
    // 7-bit ASCII output with no embedded NULs so passing as a C string
    // is safe. If we ever switch to binary we'll need a different bind.
    sq_pushstring(v, (const SQChar*)data, (SQInteger)len);
    sq_pushinteger(v, (SQInteger)frame);  // load_battle(str, frame) — keys _keep
    if (SQ_FAILED(sq_call(v, 3, SQFalse, SQTrue))) {
        log_printf("[gekko_bridge] __gekko_state.load_battle threw\n");
    }
    sq_settop(v, top0);
}

// Pull the engine `count` field out of a serialized Squirrel blob.
// The blob is gekko_state.nut's text format; battle.count appears as
// the token "s5:count;i<N>;". Returns -1 if not found. Used purely for
// diagnostics — to correlate gekko's frame number with the engine's
// own frame counter and pinpoint save/load timing drift.
static int blob_extract_count(const uint8_t* blob, uint32_t len) {
    static const char needle[] = "s5:count;i";
    const uint32_t nlen = sizeof(needle) - 1;
    if (len < nlen) return -1;
    for (uint32_t i = 0; i + nlen < len; ++i) {
        if (memcmp(blob + i, needle, nlen) == 0) {
            int val = 0; uint32_t j = i + nlen;
            bool neg = false;
            if (j < len && blob[j] == '-') { neg = true; ++j; }
            bool any = false;
            while (j < len && blob[j] >= '0' && blob[j] <= '9') {
                val = val * 10 + (blob[j] - '0'); ++j; any = true;
            }
            if (any) return neg ? -val : val;
        }
    }
    return -1;
}

// Invoke the root-level Squirrel function ::__gekko_do_vs_init() — boot.nut
// installs this when gekko is enabled. It runs the deferred vs.Initialize
// (battle.Create + battle.Begin + loop.Begin), bringing the engine fully
// into the battle scene. We call this the instant GekkoSessionStarted
// fires, BEFORE the same tick's gekko_add_local_input, so gekko's frame-0
// save captures the post-vs.Initialize battle. Returns true on success.
static bool call_squirrel_vs_init() {
    if (!v) return false;
    SQInteger top0 = sq_gettop(v);
    sq_pushroottable(v);
    sq_pushstring(v, _SC("__gekko_do_vs_init"), -1);
    if (SQ_FAILED(sq_get(v, -2))) {
        log_printf("[gekko_bridge] __gekko_do_vs_init not found\n");
        sq_settop(v, top0);
        return false;
    }
    sq_pushroottable(v);  // this = root table
    bool ok = SQ_SUCCEEDED(sq_call(v, 1, SQFalse, SQTrue));
    if (!ok) log_printf("[gekko_bridge] __gekko_do_vs_init threw\n");
    sq_settop(v, top0);
    return ok;
}

// ---------------------------------------------------------------- save/load --

// Header magic + version: bump version whenever the layout changes.
static constexpr uint32_t SAVE_MAGIC   = 0x46414B47; // 'GKAF'
static constexpr uint32_t SAVE_VERSION = 3;          // v3: + Squirrel blob trailer

struct SaveHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t frame;
    uint32_t rand_state;
    uint32_t actor_count;
    // followed by actor_count records of:
    //   uint32_t id;                          // Actor2D::id @ 0x18
    //   uint8_t  body[sizeof(ManbowActor2D)]; // raw 0xEC bytes
    // id is the stable join key — the live actor set is unordered so we
    // can't trust positional ordering. On load we build an id → ptr map
    // from the current live set and memcpy each saved blob into the
    // matching slot. Actors saved-but-not-currently-live are skipped
    // with a count (proper reanimation needs SharedPoolAllocator
    // deferred-free, separate task).
    //
    // TODO v3: per-actor sq_addref'd flag1..5 SQObjects (8 bytes each).
    // TODO v4: Squirrel diff buffer length + bytes.
    // TODO v5: b2ParticleSystem state.
};

#pragma pack(push, 1)
struct ActorRecord {
    // Pointer-as-join-key. Process-local: each peer saves/loads its own
    // state. Heap addresses are stable within a process (no ASLR mid-
    // game) and live_actors' deferred-release queue keeps the memory
    // alive across the rollback window, so the same actor sits at the
    // same address from save through load. Attempted Actor2D::id (offset
    // 0x18) first — that field is NOT a stable integer in practice; the
    // value observed was a pointer-like 0x23fef990, churning between
    // frames. Pointer is straightforwardly stable.
    uintptr_t ptr;
    uint8_t   body[sizeof(ManbowActor2D)];
};
#pragma pack(pop)
static_assert(sizeof(ActorRecord) == 4 + sizeof(ManbowActor2D));

// SQ walker is always enabled. The fine-grained "what depth do we walk"
// is controlled by ::__gekko_state._bisect_level in gekko_state.nut
// (default level 3 — walk team_data scalars, leave sub-instances at
// empty body).
static bool g_sq_save_enabled = true;

uint32_t save_state_to_buf(void* buf, uint32_t cap, uint32_t* out_checksum,
                           uint32_t frame) {
    if (cap < sizeof(SaveHeader)) return 0;

    uint8_t* p = static_cast<uint8_t*>(buf);
    SaveHeader* hdr = reinterpret_cast<SaveHeader*>(p);
    hdr->magic      = SAVE_MAGIC;
    hdr->version    = SAVE_VERSION;
    hdr->frame      = frame; // Gekko frame — keys __gekko_state._keep
    hdr->rand_state = acrt_getptd()->rand_state;

    // Snapshot live actor pointers under a fixed cap.
    static constexpr size_t MAX_ACTORS = 1024;
    ManbowActor2D* actors[MAX_ACTORS];
    size_t n = live_actors::snapshot(actors, MAX_ACTORS);
    hdr->actor_count = (uint32_t)n;

    p += sizeof(SaveHeader);
    uint32_t remaining = cap - sizeof(SaveHeader);
    uint32_t need = (uint32_t)(n * sizeof(ActorRecord));
    if (remaining < need) {
        log_printf("gekko_bridge::save_state: buffer too small (%u/%u)\n",
                   remaining, need);
        return 0;
    }

    // Per-actor record: [ptr][raw 0xEC body]. See ActorRecord comment
    // re: pointer join-key.
    for (size_t i = 0; i < n; ++i) {
        ActorRecord* rec = (ActorRecord*)p;
        rec->ptr = (uintptr_t)actors[i];
        memcpy(rec->body, actors[i], sizeof(ManbowActor2D));
        p += sizeof(ActorRecord);
    }

    // Trailer: [uint32 squirrel_len][bytes]. Lets the C++ state ride
    // alongside the dynamic per-character Squirrel state captured by
    // ::__gekko_state.save_battle (task #22).
    uint32_t actors_end = (uint32_t)(p - static_cast<uint8_t*>(buf));
    if (actors_end + 4 > cap) return actors_end;
    uint32_t* sq_len_field = (uint32_t*)p;
    p += 4;
    uint32_t sq_cap = (cap - actors_end - 4);
    uint32_t sq_n = g_sq_save_enabled ? call_squirrel_save(p, sq_cap, frame) : 0;
    *sq_len_field = sq_n;
    p += sq_n;

    uint32_t written = (uint32_t)(p - static_cast<uint8_t*>(buf));
    if (out_checksum) {
        // Checksum ONLY the Squirrel blob (deterministic text representation).
        // The C++ ActorRecord includes a per-process pointer field (the join
        // key), so two peers naturally produce different bytes even when
        // their game state is identical — that would spuriously trip
        // Gekko's desync detector every frame. The Squirrel text blob, by
        // contrast, encodes value-by-value via getclass() foreach and is
        // identical cross-peer when state matches.
        if (sq_n > 0) {
            *out_checksum = fletcher32(p - sq_n, sq_n);
        } else {
            *out_checksum = 0;
        }
    }
    return written;
}

void load_state_from_buf(const void* buf, uint32_t len) {
    if (len < sizeof(SaveHeader)) {
        log_printf("gekko_bridge::load_state: undersized blob (%u)\n", len);
        return;
    }
    const SaveHeader* hdr = static_cast<const SaveHeader*>(buf);
    if (hdr->magic != SAVE_MAGIC || hdr->version != SAVE_VERSION) {
        log_printf("gekko_bridge::load_state: bad header magic=%08x ver=%u\n",
                   hdr->magic, hdr->version);
        return;
    }

    acrt_getptd()->rand_state = hdr->rand_state;
    // TODO: restore frame counter if/where it lives

    const uint8_t* p = static_cast<const uint8_t*>(buf) + sizeof(SaveHeader);
    uint32_t need = hdr->actor_count * (uint32_t)sizeof(ActorRecord);
    if (len - sizeof(SaveHeader) < need) {
        log_printf("gekko_bridge::load_state: short actor blob "
                   "(have %u need %u)\n",
                   (uint32_t)(len - sizeof(SaveHeader)), need);
        return;
    }

    // Pointer-as-join-key: build a flat set of live actor pointers and
    // memcpy each saved body if its source pointer is still live. With
    // live_actors::set_defer_release(true), the pointer is the most
    // stable thing we have — Release won't reclaim the slot until
    // disarm, so a save+load within one rollback window finds the same
    // address occupied by the same logical actor.
    static constexpr size_t MAX_ACTORS = 1024;
    ManbowActor2D* live[MAX_ACTORS];
    size_t live_n = live_actors::snapshot(live, MAX_ACTORS);

    size_t restored = 0, missing = 0;
    for (uint32_t i = 0; i < hdr->actor_count; ++i) {
        const ActorRecord* rec = (const ActorRecord*)p;
        ManbowActor2D* target = nullptr;
        for (size_t j = 0; j < live_n; ++j) {
            if ((uintptr_t)live[j] == rec->ptr) { target = live[j]; break; }
        }
        if (target) {
            memcpy(target, rec->body, sizeof(ManbowActor2D));
            ++restored;
        } else {
            ++missing;
        }
        p += sizeof(ActorRecord);
    }
    // Only chatter when something went wrong with the restore.
    if (missing) {
        log_printf("gekko_bridge::load_state: restored %zu/%u (%zu missing)\n",
                   restored, hdr->actor_count, missing);
    }

    // Squirrel trailer (v3+). Layout after actor records:
    //   uint32_t squirrel_len; <bytes>
    if (len - sizeof(SaveHeader) >= need + 4) {
        const uint32_t sq_len = *(const uint32_t*)p;
        p += 4;
        static uint32_t sq_load_log_quota = 4;
        if (sq_len > 0 && (size_t)(p - static_cast<const uint8_t*>(buf)) + sq_len <= len) {
            if (sq_load_log_quota > 0) {
                --sq_load_log_quota;
                log_printf("[gekko_bridge] call_squirrel_load sq_len=%u "
                           "(quota_remaining=%u)\n",
                           sq_len, sq_load_log_quota);
            }
            call_squirrel_load(p, sq_len, hdr->frame);
        } else if (sq_load_log_quota > 0) {
            --sq_load_log_quota;
            log_printf("[gekko_bridge] SKIP call_squirrel_load sq_len=%u "
                       "(empty or out-of-bounds)\n", sq_len);
        }
    } else {
        static bool no_trailer_logged = false;
        if (!no_trailer_logged) {
            no_trailer_logged = true;
            log_printf("[gekko_bridge] load: NO trailer (len=%u sizeof(SH)=%u need=%u)\n",
                       len, (uint32_t)sizeof(SaveHeader), need);
        }
    }
}

// ----------------------------------------------------------- frame drivers --

// Push forced_inputs[i] into each player's TF4InputRecorderDevice ring
// buffer so the engine's normal "read next input from recorder" path
// returns Gekko's synced inputs during resim, without any vanilla
// SyncInput / network packet handling.
static void inject_forced_inputs_into_recorder() {
    if (!g_active_input_session) return;
    auto* recorder = g_active_input_session->input_recorder.get();
    if (!recorder) return;
    size_t n = recorder->devices.size();
    for (size_t i = 0; i < n && i < 2; ++i) {
        auto* dev = recorder->devices[i].get();
        if (!dev) continue;
        if (g_evt_trace > 0 && i < 2) {
            log_printf("[trace] inject dev%zu write=%u read=%u vec=%u in=0x%04x\n",
                       i, (unsigned)dev->input_write_idx,
                       (unsigned)dev->input_read_idx,
                       (unsigned)dev->input_vec.size(), forced_inputs[i]);
        }
        // Deterministic recorder feed for rollback. Write gekko's input
        // AT the current read index and set write exactly one ahead.
        // The battle's input device then reads input_vec[read_idx] —
        // which is precisely our forced value — and advances read_idx.
        //
        // The OLD approach pushed at write_idx and let both indices
        // climb. But the vanilla SyncInput pre-fills the ring buffer by
        // a different amount on each peer (host had write=3/vec=7,
        // client write=1/vec=1), so the battle read DIFFERENT inputs on
        // each peer -> desync. The absolute index values are
        // per-process and don't matter; only the VALUE at read_idx must
        // match cross-peer, which writing-at-read guarantees.
        uint32_t ri = dev->input_read_idx;
        if (ri >= dev->input_vec.size()) {
            dev->input_vec.resize(ri + 1);
        }
        dev->input_vec[ri] = forced_inputs[i];
        dev->input_write_idx = ri + 1;
    }
}

void advance_one_frame() {
    // Drive one full logic tick WITHOUT rendering.
    //
    // The vanilla engine advances the game across TWO threads:
    //   - main thread: update_logic() — RunOneFrame(g_main_scriptapi) +
    //     HasPendingFrame catch-up + Act::ScriptAPI::Update. This is the
    //     battle / ::loop / round-phase machine (verified in IDA: 0xE1A0).
    //   - background thread: ScriptAPI_BackgroundThreadLoop() —
    //     RunOneFrame(g_bg_thread_scriptapi) in a 60 Hz loop (0x2F2A0).
    //
    // better_game_loop's no_input_thread_patch NOPs the call that spawns
    // the background thread, so this single-threaded path must drive
    // BOTH ScriptAPIs itself. update_related(*g_bg_thread_scriptapi) is
    // the background half; update_logic() is the main half. Earlier this
    // function called ONLY the background half — which is why the battle
    // never advanced (demoCount frozen at 0).
    static int log_quota = 4;
    if (log_quota > 0) {
        --log_quota;
        log_printf("[gekko_bridge] advance_one_frame: enter "
                   "forced_active=%d p0=0x%04x p1=0x%04x\n",
                   (int)forced_inputs_active,
                   forced_inputs[0], forced_inputs[1]);
    }
    if (forced_inputs_active) {
        inject_forced_inputs_into_recorder();
    }
    // Strict 1 gekko-Advance = 1 logical frame.
    //
    // We deliberately do NOT call update_logic() (0xE1A0) here. Its body
    // is:
    //     RunOneFrame(g_main_scriptapi);
    //     if (HasPendingFrame()) RunOneFrame(g_main_scriptapi);  // catch-up
    //     Act::ScriptAPI::Update();
    //     ++g_frame_counter;
    //     <PrtScn screenshot polling>
    // The conditional second RunOneFrame is a real-time frame-pacing
    // catch-up: battle.count / demoCount (and per-actor count) increment
    // once per RunOneFrame, so update_logic advanced the round-phase
    // machine 1 OR 2 steps per call depending on HasPendingFrame(). A
    // rollback re-simulation lands that conditional differently than the
    // original run, the counters drift, and the HUD races/staggers
    // rounds. Deterministic rollback requires exactly one logical step
    // per Advance — so we call RunOneFrame exactly once. We also skip
    // the PrtScn polling (GetAsyncKeyState — non-deterministic real-time
    // input) and the QPC bookkeeping (real-time pacing only).
    update_related(*MAIN_SCRIPTAPI_PTR);                    // RunOneFrame(g_main), once
    Act_ScriptAPI_ptr->vftable->Update(Act_ScriptAPI_ptr);  // Act::ScriptAPI::Update
    ++*(uint32_t*)(0x4DACE0_R);                             // g_frame_counter
    if (log_quota > 0) {
        log_printf("[gekko_bridge] advance_one_frame: exit\n");
    }
}

void render_one_frame() {
    drawing_related();
}

// ---------------------------------------------------------------- session --

bool init(uint16_t local_port, uint16_t remote_port,
          uint8_t local_player_idx, const char* remote_ip)
{
    if (g_session) return false;

    gekko_create(&g_session, GekkoGameSession);

    GekkoConfig config = {};
    config.desync_detection = true;
    config.input_size = sizeof(uint16_t);
    // 2 MB: observed actor-walker blob alone ~ 204 KB per actor at
    // depth 6, and that's just team.master/slave (no projectiles/effects
    // yet). Bumping ahead of the eventual full live_actors enumeration.
    // Memory: Gekko keeps ~10 saves in flight → 20 MB working set.
    config.state_size = 2 * 1024 * 1024;
    config.max_spectators = 0;
    config.input_prediction_window = 10;
    config.num_players = 2;

    gekko_start(g_session, &config);
    gekko_net_adapter_set(g_session, gekko_default_adapter(local_port));
    gekko_set_runahead(g_session, 8);

    g_local_idx = local_player_idx;
    char remote_addr[64];
    snprintf(remote_addr, sizeof(remote_addr), "%s:%u", remote_ip, (unsigned)remote_port);

    for (int i = 0; i < 2; ++i) {
        if (i == local_player_idx) {
            gekko_add_actor(g_session, GekkoLocalPlayer, nullptr);
            gekko_set_local_delay(g_session, i, 2);
        } else {
            GekkoNetAddress addr = {};
            addr.data = remote_addr;
            addr.size = (uint32_t)strlen(remote_addr);
            gekko_add_actor(g_session, GekkoRemotePlayer, &addr);
        }
    }

    g_active = true;
    // Hold every Actor2D::Release from this point on. We start saving
    // state immediately (gekko emits Save events while sync is still in
    // flight); if Release was still un-deferred during that window the
    // saved actors would be gone by the time the matching Load fires.
    live_actors::set_defer_release(true);

    log_printf("gekko_bridge: session up. local=%u port=%u remote=%s (remote_addr_len=%u)\n",
               local_player_idx, local_port, remote_addr,
               (unsigned)strlen(remote_addr));
    return true;
}

bool init_solo() {
    if (g_session) return false;

    g_solo = true;
    gekko_create(&g_session, GekkoStressSession);

    GekkoConfig config = {};
    config.desync_detection = true;
    config.input_size = sizeof(uint16_t);
    config.state_size = 2 * 1024 * 1024;
    config.max_spectators = 0;
    config.num_players = 2;
    // Roll back 8 frames every frame: the stress session re-simulates
    // current-8 .. current each tick, so save + load + advance all run
    // hard, in one process. This is the rig for rollback determinism /
    // perf iteration.
    config.check_distance = 8;

    gekko_start(g_session, &config);

    // Both players are local — no net adapter, no handshake.
    g_local_idx = 0;
    for (int i = 0; i < 2; ++i) {
        gekko_add_actor(g_session, GekkoLocalPlayer, nullptr);
        gekko_set_local_delay(g_session, i, 1);
    }

    g_active = true;
    live_actors::set_defer_release(true);

    // The battle is already created — vs.Initialize ran under the
    // vanilla loop during the intro. A stress session has no handshake
    // and emits no GekkoSessionStarted, so we are started immediately:
    // gekko owns the frame loop from here, frame 0 = this Round_Fight
    // frame.
    g_session_started = true;

    log_printf("gekko_bridge: SOLO stress session up. check_distance=%u\n",
               config.check_distance);
    return true;
}

// Read ::battle.state from the Squirrel VM. Returns false if the table
// or field is not reachable yet.
static bool read_battle_state(int* out) {
    if (!v) return false;
    SQInteger top = sq_gettop(v);
    sq_pushroottable(v);
    sq_pushstring(v, _SC("battle"), -1);
    bool ok = SQ_SUCCEEDED(sq_get(v, -2));
    if (ok) {
        sq_pushstring(v, _SC("state"), -1);
        ok = SQ_SUCCEEDED(sq_get(v, -2));
        if (ok) {
            SQInteger st = 0;
            ok = SQ_SUCCEEDED(sq_getinteger(v, -1, &st));
            if (ok) *out = (int)st;
        }
    }
    sq_settop(v, top);
    return ok;
}

void watch_for_fight_solo() {
    g_watch_for_fight = true;
    log_printf("[gekko_bridge] watching for Round_Fight to arm solo session\n");
}

// Called every vanilla-loop frame (from better_game_loop) before any
// session exists. Once battle.state reaches Round_Fight (8), the intro
// is over — create the solo session so gekko frame 0 is fight frame 0.
void pre_arm_poll() {
    if (!g_watch_for_fight || g_session) return;
    int st = 0;
    if (read_battle_state(&st) && st == 8 /* Round_Fight */) {
        g_watch_for_fight = false;
        log_printf("[gekko_bridge] Round_Fight reached -> arming solo session\n");
        init_solo();
    }
}

void shutdown() {
    if (g_session) {
        if (!g_solo) gekko_default_adapter_destroy();
        gekko_destroy(&g_session);
        g_session = nullptr;
    }
    g_active = false;
    g_session_started = false;
    g_solo = false;
    g_watch_for_fight = false;
    // Flush any actors held by defer-release so the engine can actually
    // reclaim their slots once we're done with the session.
    live_actors::set_defer_release(false);
    live_actors::flush_deferred();
}

bool is_active()         { return g_active; }
bool is_session_started(){ return g_session_started; }

// Solo fast-forward. While a solo stress session owns the frame loop and
// the backtick (`) key is held, run extra tick()s per rendered frame so a
// run can reach time-over (~8910 logical frames) in seconds. Netplay is
// network-paced — turbo is gated to g_solo so it can't desync a match.
int turbo_ticks() {
    if (g_solo && (GetAsyncKeyState(VK_OEM_3) & 0x8000)) return 8;
    return 1;
}

// ------------------------------------------------------------------- tick --

bool tick() {
    if (!g_active) return false;

    // Always poll the network so the GekkoNet sync handshake
    // (SyncRequest/SyncResponse / session_magic exchange) can complete
    // in the background while the engine still runs the vanilla path
    // through any pre-battle UI.
    gekko_network_poll(g_session);

    // Drain session events at all times — PlayerConnected and
    // SessionStarted only fire here, so without this the handshake
    // would never appear to complete.
    int count = 0;
    GekkoSessionEvent** sevents = gekko_session_events(g_session, &count);
    for (int i = 0; i < count; ++i) {
        GekkoSessionEvent* e = sevents[i];
        switch (e->type) {
            case GekkoPlayerSyncing:
                log_printf("[gekko_bridge] PlayerSyncing handle=%d %u/%u\n",
                           e->data.syncing.handle,
                           e->data.syncing.current, e->data.syncing.max);
                break;
            case GekkoPlayerConnected:
                log_printf("[gekko_bridge] PlayerConnected handle=%d\n",
                           e->data.connected.handle);
                break;
            case GekkoPlayerDisconnected:
                log_printf("[gekko_bridge] PlayerDisconnected handle=%d\n",
                           e->data.disconnected.handle);
                break;
            case GekkoSessionStarted:
                log_printf("[gekko_bridge] SessionStarted -> running deferred vs.Initialize\n");
                // Run vs.Initialize NOW, synchronously, before this same
                // tick's gekko_add_local_input. That makes gekko's
                // frame-0 save capture the fully-initialized battle
                // (battle.Create + battle.Begin → state=2). If we let
                // vanilla update run vs.Initialize a frame later, the
                // battle setup would happen between gekko frames and
                // rollback could never reproduce it.
                call_squirrel_vs_init();
                g_session_started = true;
                log_printf("[gekko_bridge] vs.Initialize done -> gekko owns frame loop\n");
                break;
            case GekkoDesyncDetected: {
                // DESYNC is loud — Gekko fires it for every frame the
                // checksums disagree. Throttle to one line per 300 to
                // keep the log readable while still showing the issue.
                static uint32_t desync_counter = 0;
                if ((desync_counter++ % 300) == 0) {
                    log_printf("[gekko_bridge] !! DESYNC frame=%d local=0x%08x remote=0x%08x peer_handle=%d (desyncs_so_far=%u)\n",
                               e->data.desynced.frame,
                               e->data.desynced.local_checksum,
                               e->data.desynced.remote_checksum,
                               e->data.desynced.remote_handle,
                               desync_counter);
                }
                break;
            }
            default:
                log_printf("[gekko_bridge] session event type=%d\n", e->type);
                break;
        }
    }

    // The sync handshake (SendSyncRequest / SyncResponse) is pumped by
    // gekko_update_session, NOT gekko_network_poll alone — we must call
    // it every tick from session creation onward or SessionStarted will
    // never fire. But gekko's frame counter only advances on
    // gekko_add_local_input: skipping that until SessionStarted keeps
    // both peers at frame 0 until the handshake completes, so their
    // first real frame happens at the same wall-clock moment.
    if (g_session_started) {
        uint16_t my_input = read_local_input_bits();
        gekko_add_local_input(g_session, g_local_idx, &my_input);
        // Solo stress session: both players are local, so drive the
        // second one too. Same input bits — a stress run exercises the
        // save/load/rollback path, it is not a real match.
        if (g_solo) {
            gekko_add_local_input(g_session, 1, &my_input);
        }
    }

    count = 0;
    GekkoGameEvent** uevents = gekko_update_session(g_session, &count);
    bool advanced = false;
    for (int i = 0; i < count; ++i) {
        GekkoGameEvent* e = uevents[i];
        switch (e->type) {
            case GekkoSaveEvent: {
                if (!g_session_started) {
                    // Pre-vs.Initialize: ::battle exists as a blank
                    // global table (state=0, teams=[null,null]) from
                    // early boot. Capturing it would let gekko later
                    // Load that blank state over the real battle. Write
                    // an empty save so the matching Load is a no-op.
                    *e->data.save.state_len = 0;
                    *e->data.save.checksum = 0;
                    break;
                }
                uint32_t cs = 0;
                uint32_t n = save_state_to_buf(
                    e->data.save.state,
                    /*cap=*/ 2 * 1024 * 1024,  // must match GekkoConfig::state_size
                    &cs,
                    (uint32_t)e->data.save.frame
                );
                *e->data.save.state_len = n;
                *e->data.save.checksum = cs;
                // Dump the Squirrel blob for two specific frames to
                // per-peer text files: frame 0 (pre-divergence baseline)
                // and frame 90 (well past where DESYNC first fires ~f20).
                // `diff sq_blob_p0_f90.txt sq_blob_p1_f90.txt` then shows
                // exactly which actor field diverged across peers.
                {
                    // Dump every save of DUMP_FRAME to its own numbered
                    // file — the WHOLE blob (header + actor records +
                    // Squirrel). The SAME peer saves a frame multiple
                    // times (speculative, then post-rollback re-sims).
                    // With identical inputs those MUST be byte-identical;
                    // diffing _s0 vs _s1 shows exactly which section (and
                    // offset) the sim mutated that save/load did NOT
                    // restore — i.e. the missing piece of the snapshot.
                    // Set to the frame run_solo.sh reports the DESYNC at.
                    static const int DUMP_FRAME = 2;
                    int fr = e->data.save.frame;
                    static int dump_save_count = 0;
                    if (fr == DUMP_FRAME && dump_save_count < 6) {
                        char path[128];
                        snprintf(path, sizeof(path),
                                 "C:\\dev\\aocf\\th155\\sq_blob_p%u_f%d_s%d.bin",
                                 (unsigned)g_local_idx, fr, dump_save_count);
                        FILE* f = fopen(path, "wb");
                        if (f) {
                            fwrite(e->data.save.state, 1, n, f);
                            fclose(f);
                            log_printf("[gekko_bridge] Save frame=%d #%d len=%u"
                                       " cs=0x%08x -> %s\n",
                                       fr, dump_save_count, n, cs, path);
                        }
                        ++dump_save_count;
                    }
                }
                // TRACE: correlate gekko frame ↔ engine `count` so we
                // can see where the off-by-one between speculative and
                // re-sim saves enters.
                if (g_evt_trace > 0) {
                    --g_evt_trace;
                    const SaveHeader* sh = (const SaveHeader*)e->data.save.state;
                    log_printf("[trace] SAVE gframe=%d count=%d cs=0x%08x rand=0x%08x\n",
                               e->data.save.frame,
                               blob_extract_count((const uint8_t*)e->data.save.state, n),
                               cs, (n >= sizeof(SaveHeader)) ? sh->rand_state : 0);
                }
                break;
            }
            case GekkoLoadEvent: {
                if (g_evt_trace > 0) {
                    --g_evt_trace;
                    const SaveHeader* sh = (const SaveHeader*)e->data.load.state;
                    log_printf("[trace] LOAD gframe=%d count=%d len=%u rand=0x%08x\n",
                               e->data.load.frame,
                               blob_extract_count((const uint8_t*)e->data.load.state,
                                                  e->data.load.state_len),
                               e->data.load.state_len,
                               (e->data.load.state_len >= sizeof(SaveHeader)) ? sh->rand_state : 0);
                }
                load_state_from_buf(e->data.load.state,
                                    e->data.load.state_len);
                break;
            }
            case GekkoAdvanceEvent: {
                const uint16_t* inputs = (const uint16_t*)e->data.adv.inputs;
                forced_inputs[0] = inputs[0];
                forced_inputs[1] = inputs[1];
                forced_inputs_active = true;
                advance_one_frame();
                forced_inputs_active = false;
                advanced = true;
                if (g_evt_trace > 0) {
                    --g_evt_trace;
                    log_printf("[trace] ADV  gframe=%d rb=%d p0=0x%04x p1=0x%04x\n",
                               e->data.adv.frame, (int)e->data.adv.rolling_back,
                               inputs[0], inputs[1]);
                }
                // Log only rollback resims + an occasional heartbeat.
                if (e->data.adv.rolling_back) {
                    static uint32_t rb_log = 0;
                    if ((rb_log++ % 300) == 0) {
                        log_printf("[gekko_bridge] Advance frame=%d p0=0x%04x p1=0x%04x ROLLBACK (rb_so_far=%u)\n",
                                   e->data.adv.frame, inputs[0], inputs[1], rb_log);
                    }
                } else if ((e->data.adv.frame % 300) == 0) {
                    log_printf("[gekko_bridge] heartbeat frame=%d p0=0x%04x p1=0x%04x\n",
                               e->data.adv.frame, inputs[0], inputs[1]);
                }
                break;
            }
            default:
                log_printf("[gekko_bridge] unknown GekkoGameEvent type=%d\n", e->type);
                break;
        }
    }

    // Tick the background-thread ScriptAPI exactly ONCE per real frame,
    // here — OUTSIDE the rollback resim. It drives system scripts
    // (network poll, Discord RPC, audio, timing) which are
    // non-deterministic and must not run inside advance_one_frame (that
    // re-runs up to runahead times per frame during rollback). Keeping
    // it once-per-real-frame here keeps audio/RPC alive without
    // polluting the deterministic battle sim.
    update_related(*INPUT_UPDATE_LIST_PTR);

    // Note: rendering is driven by better_game_loop's window_render
    // call AFTER tick(), not from here. That keeps the window updating
    // even on frames where Gekko didn't advance (e.g. while the sync
    // handshake is still in flight, or waiting on remote inputs) so
    // the screen doesn't freeze to black.
    return advanced;
}

} // namespace gekko_bridge
