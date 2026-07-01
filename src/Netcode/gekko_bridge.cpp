// GekkoNet bridge — skeleton. See gekko_bridge.h for the public API and
// design notes. Each TODO below is a concrete next step.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gekko_bridge.h"
#include "patch_utils.h"
#include "netcode.h"
#include "input_session_layout.h"
#include "util.h"
#include "log.h"
#include "Actor2D.h"
#include "live_actors.h"
#include "sq_arena.h"      // Squirrel subsystem arena (objects + VM + stacks)
#include "battle_pools.h"  // TF4 TPoolAllocator battle objects
#include "tf4_pool.h"      // generic-grow objpool redirect + freeze
#include "engine_snap.h"   // scheduler fixed-region snapshot
#include "cpp_arena.h"     // C++ std::list node arena
#include "bullet_arena.h"  // Bullet physics heap arena
#include "snapshot_ring.h" // dirty-page rollback snapshot for the big arenas
#include "input_hist.h"    // per-player input-history capture
#include "crash_handler.h" // watch_cxx — log C++ throws in a re-sim
#include "rollback.h"      // layer-4 sq-diff identifier
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
// Slot 0 is Act::ScriptAPI::Update (th155.exe 0x124870), the per-frame
// ::loop pump. Verified against update_logic, which calls it as
// (**(vtbl***)obj)(obj) — i.e. vtable[0]. The earlier struct placed
// Update at slot 4, which is actually a 3-arg InterfaceObject-lookup
// method (0x124670); calling it through a 1-arg signature left strcmp
// reading an uninitialised-stack name pointer → intermittent crash at
// th155.exe+0x1246C0 (READ of a garbage address).
struct ScriptAPI_vtbl {
    void (thiscall* Update)(void* self);  // slot 0 — the ::loop pump
    void* slot_4;
    void* slot_8;
    void* slot_C;
    void* slot_10;
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

// Published for sq_trace: the gekko frame currently being advanced and
// whether it is a rollback re-sim. Lets a fault detected deep in the
// Squirrel VM be attributed to a specific frame / forward-vs-rollback.
int g_trace_frame = -1;
int g_trace_rb    = 0;
// Rollback DEPTH = how many frames the current re-sim advance is past its load
// target (g_trace_frame - last GekkoLoad frame). 0 on the forward. Lets the
// diagnostics isolate the ONE divergent (DEEPEST, depth=8) re-sim of a frame
// from the 7 matching shallower re-sims that aggregate under rb=1.
int g_trace_depth = 0;
static int g_last_load_frame = -1;
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

// --- synthetic input (test harness) ----------------------------------------
// SQUIROLL_FAKE_INPUT=1 drives the players with generated inputs so a
// stress / rollback run actually fights — movement, attacks, projectiles,
// hits — instead of sitting AFK (AFK only exercises desync DETECTION, not
// real rollback of projectiles and actor state).
//
// Each player has its own xorshift32 stream, kept entirely separate from
// the engine's RNG (which is part of the rollback snapshot — this
// generator must never draw from it). GekkoNet stores every input it is
// handed and replays it verbatim on a rollback re-sim, so the stream only
// has to advance once per real frame; it need not be re-sim aware.
//
// Packed-input bit layout (TF4InputDevice::PollState, th155 0x169D80):
//   bit0 left  bit1 right  bit2 up  bit3 down
//   bits4-15   the 12 buttons (4/5/6/7 = the A/B/C/D attack buttons).
static bool     g_fake_input   = false;
static uint32_t g_fake_rng[2]  = {0, 0};
static uint16_t g_fake_held[2] = {0, 0};   // current held direction bits
static int      g_fake_hold[2] = {0, 0};   // frames left on that direction

static uint32_t fake_xs32(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

static void fake_input_init() {
    char buf[16] = {0};
    DWORD n = GetEnvironmentVariableA("SQUIROLL_FAKE_INPUT", buf, sizeof(buf));
    g_fake_input = (n > 0 && n < sizeof(buf) && atoi(buf) != 0);
    if (!g_fake_input) return;

    uint32_t seed;
    char sb[16] = {0};
    DWORD sn = GetEnvironmentVariableA("SQUIROLL_INPUT_SEED", sb, sizeof(sb));
    // NB: parse with strtoul(base 0) for BOTH the check and the value — atoi()
    // returns 0 for "0x1111"-style hex, which silently rejected every hex seed
    // the harness passed: all "seeded" DET runs played the SAME input stream
    // (DET also freezes GetTickCount, so the fallback seed was near-constant).
    uint32_t sv = (sn > 0 && sn < sizeof(sb)) ? (uint32_t)strtoul(sb, nullptr, 0) : 0;
    bool explicit_seed = (sv != 0);
    if (explicit_seed) {
        seed = sv;                                  // override -> reproduce a run
    } else {
        // The harness bat does NOT set SQUIROLL_INPUT_SEED, so a fixed default
        // made EVERY run play the identical input stream. Derive the seed from
        // the wall clock instead -> each run differs; the seed is logged so any
        // interesting run can be reproduced with SQUIROLL_INPUT_SEED=<that seed>.
        uint32_t t = GetTickCount();
        seed = (t * 2654435761u) ^ (t << 13) ^ (t >> 7) ^ 0x9E3779B9u;
        if (seed == 0) seed = 0x9E3779B9u;
    }
    // Two distinct, non-zero streams — one per player.
    g_fake_rng[0] = seed ^ 0xA5A5A5A5u;
    g_fake_rng[1] = seed ^ 0x5A5A5A5Au;
    g_fake_held[0] = g_fake_held[1] = 0;
    g_fake_hold[0] = g_fake_hold[1] = 0;
    log_printf("[gekko_bridge] FAKE INPUT enabled, seed=0x%08x (%s) — "
               "set SQUIROLL_INPUT_SEED=0x%08x to reproduce this run\n",
               seed, explicit_seed ? "explicit" : "time-based", seed);
}

// Generate one player's packed input for this frame: a direction held for
// a random 8-39 frame stretch, plus a ~38%-per-frame press of a random
// attack button. That keeps both characters moving and attacking, so
// projectiles, hitboxes and actor churn are continuously on screen for
// the rollback to capture and restore.
static uint16_t fake_input_gen(int p) {
    if (--g_fake_hold[p] <= 0) {
        uint32_t r = fake_xs32(g_fake_rng[p]);
        // 0x1/0x2 = up/down, 0x4/0x8 = left/right (the originals' diagonals
        // 0x1|0x4 etc. fix this mapping). Weight toward horizontal walking so
        // movement is VISIBLE, with diagonals/verticals/neutral mixed in. Short
        // holds (4-15 frames) so the direction CHANGES often -- a short run then
        // shows P1 actually moving around instead of one held direction.
        static const uint16_t dirs[16] = {
            0x4, 0x8, 0x4, 0x8, 0x4, 0x8,         // left/right walk (weighted)
            0x1, 0x2,                             // up (jump) / down (crouch)
            0x1|0x4, 0x1|0x8, 0x2|0x4, 0x2|0x8,   // diagonals
            0x0, 0x4, 0x8, 0x0                     // neutral + more walk
        };
        g_fake_held[p] = dirs[r & 15];
        g_fake_hold[p] = 4 + (int)((r >> 8) % 12);   // 4-15 frames
    }
    uint16_t in = g_fake_held[p];
    uint32_t r = fake_xs32(g_fake_rng[p]);
    if ((r & 0xFF) < 80) {                            // ~31% of frames: attack
        in |= (uint16_t)(0x10u << ((r >> 8) & 3));    // one of A/B/C/D
    }
    return in;
}

// Accumulating form — carries the rolling state `a` so a checksum can span
// several disjoint byte ranges (used to skip the cpp_arena/render section).
static uint32_t fletcher32_acc(uint32_t a, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) a = (a >> 8) ^ (a + data[i]);
    return a;
}
static uint32_t fletcher32(const uint8_t* data, size_t len) {
    // TODO: use whatever checksum GekkoNet's desync detector prefers.
    return fletcher32_acc(0xFFFFFFFFu, data, len);
}

} // namespace gekko_bridge — temporarily close so the extern is global

// plugin.cpp owns the live HSQUIRRELVM as a translation-unit global; we
// share access via extern. The symbol is top-level (no namespace).
extern HSQUIRRELVM v;

namespace gekko_bridge {

// ----------------------------------------------------------- squirrel hook --

// ============================================================================
// Native Squirrel-state serializer — raw-struct walker.
//
// Walks the battle object graph by reading th155's Squirrel 3.0.6 object
// structs DIRECTLY — zero Squirrel C API calls in the hot path — and emits
// the text format gekko_state.nut's deser() parses unchanged. Bound as
// ::__gekko_cpp_ser; save_battle() reaches it via _ser_out().
//
// Struct layouts below are th155's, verified in rollback.cpp. SQObject
// (HSQOBJECT) is 16 bytes (SQUSEDOUBLE build). Nothing runs Squirrel code
// during the walk (pure C++ memory reads), so the GC cannot move objects
// mid-walk and raw pointers stay valid.
//
// Format (byte-compatible with deser()):
//   n;  i<N>;  f<N>;  b0;/b1;  s<LEN>:<DATA>;  a<N>:[...]  t<N>:{kv...}
//   I<ID>:<M>:{kv...} (instance first-sight)  R<ID>; (instance ref)  ?; (skip)
// ============================================================================

namespace {

// th155 Squirrel internal layouts — offsets verified in rollback.cpp.
struct SqObjVec   { SQObject* vals; uint32_t size; uint32_t alloc; };
struct SqString   { char _pad[0x14]; int32_t len; uint32_t hash; char val[1]; };
struct SqWeakRef  { char _pad[0x0C]; SQObject obj; };
struct SqHashNode { SQObject val; SQObject key; SqHashNode* next; };
struct SqTable    { char _pad[0x20]; SqHashNode* nodes; int32_t numofnodes; };
struct SqArray    { char _pad[0x18]; SqObjVec values; };
struct SqClass    { char _pad[0x18]; SqTable* members; };
struct SqInstance { char _pad[0x1C]; SqClass* cls; char _pad2[0x0C]; SQObject values[1]; };

// SQClass._members maps a member name -> an OT_INTEGER encoding the member
// kind (method bit in the high byte) + the slot index in the low 24 bits.
static const long long SQ_MEMBER_METHOD = 0x01000000;

struct RawSer {
    std::string out;
    std::unordered_map<const void*, int> seen;   // instance ptr -> assigned id
    int next_id   = 1;
    int cur_depth = 0;
    int max_depth = 6;
};

static void raw_ser_append_int(std::string& s, long long n) {
    char b[24];
    int len = snprintf(b, sizeof(b), "%lld", n);
    if (len > 0) s.append(b, (size_t)len);
}

// device_id / input / last_snap — per-peer process-local; emitted as `?;`.
static bool raw_is_skip_key(const SqString* s) {
    if (!s || s->len <= 0) return false;
    int32_t n = s->len;
    const char* k = s->val;
    return (n == 9 && memcmp(k, "device_id", 9) == 0)
        || (n == 5 && memcmp(k, "input", 5) == 0)
        || (n == 9 && memcmp(k, "last_snap", 9) == 0);
}

static void raw_emit_string(std::string& out, const SqString* s) {
    int32_t len = (s && s->len > 0) ? s->len : 0;
    out += 's';
    raw_ser_append_int(out, len);
    out += ':';
    if (len > 0) out.append(s->val, (size_t)len);
    out += ';';
}

// Lexicographic byte order over two Squirrel strings (deterministic key sort).
static bool raw_str_less(const SqString* a, const SqString* b) {
    int32_t la = (a && a->len > 0) ? a->len : 0;
    int32_t lb = (b && b->len > 0) ? b->len : 0;
    int32_t m = la < lb ? la : lb;
    int cmp = (m > 0) ? memcmp(a->val, b->val, (size_t)m) : 0;
    if (cmp) return cmp < 0;
    return la < lb;
}

static void raw_ser(RawSer& c, const SQObject& o);

static void raw_ser_table(RawSer& c, const SqTable* t) {
    struct KV { int kind; long long ki; const SqString* ks; const SQObject* val; };
    std::vector<KV> kvs;
    if (t && t->nodes) {
        for (int32_t i = 0; i < t->numofnodes; ++i) {
            const SqHashNode* nd = &t->nodes[i];
            if (nd->key._type == OT_NULL) continue;   // empty bucket
            KV kv;
            kv.kind = 2; kv.ki = 0; kv.ks = nullptr;
            if (nd->key._type == OT_INTEGER) {
                kv.kind = 0; kv.ki = (long long)nd->key._unVal.nInteger;
            } else if (nd->key._type == OT_STRING) {
                kv.kind = 1; kv.ks = (const SqString*)nd->key._unVal.pString;
            }
            kv.val = &nd->val;
            kvs.push_back(kv);
        }
    }
    std::stable_sort(kvs.begin(), kvs.end(), [](const KV& a, const KV& b) {
        if (a.kind != b.kind) return a.kind < b.kind;
        if (a.kind == 0)      return a.ki < b.ki;
        if (a.kind == 1)      return raw_str_less(a.ks, b.ks);
        return false;
    });
    c.out += 't';
    raw_ser_append_int(c.out, (long long)kvs.size());
    c.out += ":{";
    for (const KV& kv : kvs) {
        if (kv.kind == 0)      { c.out += 'i'; raw_ser_append_int(c.out, kv.ki); c.out += ';'; }
        else if (kv.kind == 1) { raw_emit_string(c.out, kv.ks); }
        else                   { c.out += "n;"; }
        raw_ser(c, *kv.val);
    }
    c.out += '}';
}

static void raw_ser_instance(RawSer& c, const SQObject& o) {
    const SqInstance* inst = (const SqInstance*)o._unVal.pInstance;

    auto it = c.seen.find(inst);
    if (it != c.seen.end()) {
        c.out += 'R';
        raw_ser_append_int(c.out, it->second);
        c.out += ';';
        return;
    }
    int my_id = c.next_id++;
    c.seen[inst] = my_id;

    const SqClass* cls     = inst ? inst->cls : nullptr;
    const SqTable* members = cls  ? cls->members : nullptr;
    if (c.cur_depth >= c.max_depth || !members || !members->nodes) {
        c.out += 'I';
        raw_ser_append_int(c.out, my_id);
        c.out += ":0:{}";
        return;
    }

    struct Mem { const SqString* name; bool is_method; int idx; };
    std::vector<Mem> ms;
    for (int32_t i = 0; i < members->numofnodes; ++i) {
        const SqHashNode* nd = &members->nodes[i];
        if (nd->key._type != OT_STRING) continue;
        long long enc = (long long)nd->val._unVal.nInteger;
        Mem m;
        m.name      = (const SqString*)nd->key._unVal.pString;
        m.is_method = (enc & SQ_MEMBER_METHOD) != 0;
        m.idx       = (int)(enc & 0x00FFFFFF);
        ms.push_back(m);
    }
    std::stable_sort(ms.begin(), ms.end(), [](const Mem& a, const Mem& b) {
        return raw_str_less(a.name, b.name);
    });

    c.out += 'I';
    raw_ser_append_int(c.out, my_id);
    c.out += ':';
    raw_ser_append_int(c.out, (long long)ms.size());
    c.out += ":{";
    c.cur_depth++;
    for (const Mem& m : ms) {
        raw_emit_string(c.out, m.name);
        // Methods (incl. Sqrat-bound native accessors) and per-peer skip
        // keys emit `?;` — never read off the instance.
        if (m.is_method || raw_is_skip_key(m.name)) {
            c.out += "?;";
            continue;
        }
        raw_ser(c, inst->values[m.idx]);   // FIELD — raw instance value slot
    }
    c.cur_depth--;
    c.out += '}';
}

static void raw_ser(RawSer& c, const SQObject& o) {
    switch (o._type) {
    case OT_NULL:
        c.out += "n;";
        return;
    case OT_BOOL:
        c.out += (o._unVal.nInteger ? "b1;" : "b0;");
        return;
    case OT_INTEGER:
        c.out += 'i';
        raw_ser_append_int(c.out, (long long)o._unVal.nInteger);
        c.out += ';';
        return;
    case OT_FLOAT: {
        char b[40];
        int len = snprintf(b, sizeof(b), "%.17g", (double)o._unVal.fFloat);
        c.out += 'f';
        if (len > 0) c.out.append(b, (size_t)len);
        c.out += ';';
        return;
    }
    case OT_STRING:
        raw_emit_string(c.out, (const SqString*)o._unVal.pString);
        return;
    case OT_ARRAY: {
        const SqArray* a = (const SqArray*)o._unVal.pArray;
        uint32_t n = a ? a->values.size : 0;
        c.out += 'a';
        raw_ser_append_int(c.out, (long long)n);
        c.out += ":[";
        if (a && a->values.vals) {
            for (uint32_t i = 0; i < n; ++i) raw_ser(c, a->values.vals[i]);
        }
        c.out += ']';
        return;
    }
    case OT_TABLE:
        raw_ser_table(c, (const SqTable*)o._unVal.pTable);
        return;
    case OT_WEAKREF: {
        const SqWeakRef* w = (const SqWeakRef*)o._unVal.pWeakRef;
        if (w) raw_ser(c, w->obj);
        else   c.out += "n;";
        return;
    }
    case OT_INSTANCE:
        raw_ser_instance(c, o);
        return;
    default:
        // closure / nativeclosure / class / userdata / thread / generator /
        // funcproto / outer / userpointer — not serializable.
        c.out += "?;";
        return;
    }
}

} // anonymous namespace

// Bound as ::__gekko_cpp_ser(value, max_depth). save_battle() calls this
// instead of the Squirrel ser(); walks `value` via raw struct access.
static SQInteger gekko_cpp_ser(HSQUIRRELVM vm) {
    try {
        HSQOBJECT arg;
        sq_resetobject(&arg);
        if (SQ_FAILED(sq_getstackobj(vm, 2, &arg))) {
            return sq_throwerror(vm, _SC("gekko_cpp_ser: missing argument"));
        }
        SQInteger md = 6;
        if (sq_gettop(vm) >= 3) sq_getinteger(vm, 3, &md);
        RawSer c;
        c.max_depth = (int)md;
        c.out.reserve(192 * 1024);
        raw_ser(c, arg);
        sq_pushstring(vm, c.out.data(), (SQInteger)c.out.size());
        return 1;
    } catch (...) {
        return sq_throwerror(vm, _SC("gekko_cpp_ser: exception"));
    }
}

// Register ::__gekko_cpp_ser on the root table. Idempotent; called from
// init()/init_solo() once the Squirrel VM is up.
static void register_cpp_ser() {
    static bool done = false;
    if (done || !v) return;
    done = true;
    SQInteger top = sq_gettop(v);
    sq_pushroottable(v);
    sq_setfunc(v, _SC("__gekko_cpp_ser"), &gekko_cpp_ser);
    sq_settop(v, top);
    log_printf("[gekko_bridge] registered __gekko_cpp_ser (native walker)\n");
}

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
    // PERF: time the Squirrel walker. Logs avg us every 600 saves.
    LARGE_INTEGER _ps0, _ps1, _psf;
    QueryPerformanceFrequency(&_psf);
    QueryPerformanceCounter(&_ps0);
    SQRESULT _sr = sq_call(v, 2, SQTrue, SQTrue);
    QueryPerformanceCounter(&_ps1);
    {
        static uint64_t acc = 0, cnt = 0;
        acc += (uint64_t)(_ps1.QuadPart - _ps0.QuadPart) * 1000000ull / _psf.QuadPart;
        if (++cnt % 600 == 0) {
            log_printf("[perf] save_battle: avg %llu us/call over %llu calls\n",
                       acc / cnt, cnt);
        }
    }
    if (SQ_FAILED(_sr)) { sq_settop(v, top0); return 0; }
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
    LARGE_INTEGER _pl0, _pl1, _plf;
    QueryPerformanceFrequency(&_plf);
    QueryPerformanceCounter(&_pl0);
    SQRESULT _lr = sq_call(v, 3, SQFalse, SQTrue);
    QueryPerformanceCounter(&_pl1);
    {
        static uint64_t acc = 0, cnt = 0;
        acc += (uint64_t)(_pl1.QuadPart - _pl0.QuadPart) * 1000000ull / _plf.QuadPart;
        if (++cnt % 600 == 0) {
            log_printf("[perf] load_battle: avg %llu us/call over %llu calls\n",
                       acc / cnt, cnt);
        }
    }
    if (SQ_FAILED(_lr)) {
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

// ---------------------------------------------------------------- save/load --

// Header magic + version: bump version whenever the layout changes.
static constexpr uint32_t SAVE_MAGIC   = 0x46414B47; // 'GKAF'
static constexpr uint32_t SAVE_VERSION = 5;          // v5: snapshot_ring (blob = header only)

// ManbowActor2D::anim_controller is a std::shared_ptr at +0x3C; its first
// 4 bytes are the ManbowAnimationController2D*.
static constexpr uint32_t ACTOR_ANIM_CTRL_OFF = 0x3C;
// Bytes of the controller we snapshot — covers the base playback fields
// and the 2D playback block (motion @0x1C .. __bool13E @0x13E). The
// controller's std::vectors (collision boxes @0x78-0x9C, sprites @0x224)
// are deliberately NOT in this range and never restored: they own heap
// buffers and are derived state, recomputed every frame from the
// animation. Only the playback fields drive end-of-motion callback
// timing, which is what desyncs across rollback (#58).
static constexpr uint32_t ANIM_SNAP_BYTES = 0x140;

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
    // same address from save through load.
    uintptr_t ptr;
    uint8_t   body[sizeof(ManbowActor2D)];   // raw 0xEC ManbowActor2D
    uint32_t  ctrl_present;                  // 1 if anim_controller != null
    uint8_t   ctrl[ANIM_SNAP_BYTES];         // ManbowAnimationController2D head
};
#pragma pack(pop)
static_assert(sizeof(ActorRecord) ==
              4 + sizeof(ManbowActor2D) + 4 + ANIM_SNAP_BYTES);

// SQ walker is always enabled. The fine-grained "what depth do we walk"
// is controlled by ::__gekko_state._bisect_level in gekko_state.nut
// (default level 3 — walk team_data scalars, leave sub-instances at
// empty body).
static bool g_sq_save_enabled = true;

// Arena-based rollback: the real giuroll-style memory snapshot. ON: save/
// load capture and restore the whole battle state as one consistent unit
// — sq_arena (Squirrel VM heap) + cpp_arena (engine C++ allocations) +
// battle_pools (battle object pools) + engine_snap (the scheduler's
// fixed-address objects, sentinels and counters). Everything the battle
// touches lives in one of those, so the restore is lossless and the
// re-sim is bit-deterministic. OFF: the build falls back to the text
// walker for restore (non-crashing, but lossy — only used for bring-up).
static bool g_arena_rollback = true;

// input-recorder snapshot section (defined after inject_forced_inputs).
static uint32_t input_rec_save(uint8_t* out, uint32_t cap);
static void     input_rec_load(const uint8_t* blob, uint32_t len);

// perf probe — accumulate QPC ticks spent in save / load / advance, logged
// as average microseconds per call. Shows whether the stress-rig frame time
// is the snapshot or the 9x game simulation.
static uint64_t g_perf_save = 0, g_perf_load = 0, g_perf_adv = 0;
static uint32_t g_perf_nsave = 0, g_perf_nload = 0, g_perf_nadv = 0;
// save split: small-section serialization vs snapshot_ring::capture.
static uint64_t g_perf_sblob = 0, g_perf_cap = 0;

uint32_t save_state_to_buf(void* buf, uint32_t cap, uint32_t* out_checksum,
                           uint32_t frame) {
    if (cap < sizeof(SaveHeader)) return 0;

    // DIAGNOSTIC: the alloc-sequence trace is now reset/checked INSIDE
    // advance_one_frame (around update_related), bracketing exactly that
    // advance's allocations — the save/load points cleared it at the wrong
    // time (empty window on battle frames).

    // (Previously: a hardcoded watchpoint arm at frame=14 / disarm at 15
    // on bullet_arena+0x480830. snapshot_ring's divbyte loop now auto-arms
    // DR0 on the FIRST divergent dword across the run — leaving that arm
    // in place catches subsequent writers across every re-sim of f=15.
    // The hardcoded arm was disarming our auto-arm before the writer
    // could fire, so removed.)

    uint8_t* p = static_cast<uint8_t*>(buf);
    SaveHeader* hdr = reinterpret_cast<SaveHeader*>(p);
    hdr->magic      = SAVE_MAGIC;
    hdr->version    = SAVE_VERSION;
    hdr->frame      = frame; // Gekko frame — keys __gekko_state._keep
    hdr->rand_state = acrt_getptd()->rand_state;

    // Snapshot live actor pointers under a fixed cap. With arena rollback
    // on, the actors live in battle_pools and are restored wholesale by
    // the Trailer 2 pool snapshot — the per-actor records are redundant,
    // so skip them (actor_count = 0).
    static constexpr size_t MAX_ACTORS = 2048;
    ManbowActor2D* actors[MAX_ACTORS];
    size_t n = g_arena_rollback ? 0 : live_actors::snapshot(actors, MAX_ACTORS);
    hdr->actor_count = (uint32_t)n;

    // Arena rollback: the two big arenas (sq_arena, bullet_arena) are
    // dirty-page snapshotted by snapshot_ring; the small sections (battle
    // pools, cpp_arena, engine, input) go in as one ~1 MB blob. The GekkoNet
    // blob is then just the SaveHeader — hdr->frame is the ring handle, so
    // GekkoNet never copies the 22 MB of state. See snapshot_ring.h.
    if (g_arena_rollback && snapshot_ring::armed()) {
        static uint8_t* smb = nullptr;
        static constexpr uint32_t SMB_CAP = 4u * 1024 * 1024;
        if (!smb) smb = (uint8_t*)VirtualAlloc(nullptr, SMB_CAP,
                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        uint8_t* sp = smb;
        bool ok = smb != nullptr;
        auto sect = [&](auto save_fn) {
            if (!ok || sp + 4 > smb + SMB_CAP) { ok = false; return; }
            uint32_t* lf = (uint32_t*)sp;
            sp += 4;
            uint32_t w = save_fn(sp, (uint32_t)(smb + SMB_CAP - sp));
            if (w == 0) { ok = false; return; }
            *lf = w;
            sp += w;
        };
        LARGE_INTEGER _c0; QueryPerformanceCounter(&_c0);
        sect(&battle_pools::save);
        sect(&battle_pools::boostpool_save);   // Sqrat math boost::pools (this.va/vf/vfBaria)
        // cpp_arena is now dirty-page-snapshotted by snapshot_ring, not full-copied in the small blob.
        sect(&engine_snap::save);
        sect(&input_rec_save);
        sect(&input_hist::save);
        if (!ok) {
            log_printf("[gekko_bridge] !! small-section save overflow f=%u\n",
                       frame);
            if (out_checksum) *out_checksum = 0;
            return sizeof(SaveHeader);
        }
        // DIAGNOSTIC: per-section sblob checksums — pins a desync to the
        // exact small-section (battle_pools / engine/.data / input) that
        // diverged. Sections are [u32 len][data], in sect() order.
        {
            const uint8_t* q = smb;
            const char* nm[5] = { "bp", "mp", "eng", "irec", "ihist" };
            char comps[160]; int cn = 0;
            for (int s = 0; s < 5 && q + 4 <= sp; ++s) {
                uint32_t L = *(const uint32_t*)q; q += 4;
                if (q + L > sp) break;
                uint32_t h = 2166136261u;
                for (uint32_t i = 0; i < L; ++i) { h ^= q[i]; h *= 16777619u; }
                q += L;
                cn += wsprintfA(comps + cn, " %s=%08x", nm[s], h);
            }
            log_printf("[sblob] f=%u%s\n", frame, comps);
        }
        LARGE_INTEGER _c1; QueryPerformanceCounter(&_c1);
        uint32_t cs = snapshot_ring::capture(frame, smb,
                                             (uint32_t)(sp - smb));
        LARGE_INTEGER _c2; QueryPerformanceCounter(&_c2);
        g_perf_sblob += (uint64_t)(_c1.QuadPart - _c0.QuadPart);
        g_perf_cap   += (uint64_t)(_c2.QuadPart - _c1.QuadPart);
        if (out_checksum) *out_checksum = cs;
        return sizeof(SaveHeader);
    }

    p += sizeof(SaveHeader);
    uint32_t remaining = cap - sizeof(SaveHeader);
    uint32_t need = (uint32_t)(n * sizeof(ActorRecord));
    if (remaining < need) {
        log_printf("gekko_bridge::save_state: buffer too small (%u/%u)\n",
                   remaining, need);
        return 0;
    }

    // Per-actor record: [ptr][raw 0xEC body][ctrl_present][ctrl head].
    for (size_t i = 0; i < n; ++i) {
        ActorRecord* rec = (ActorRecord*)p;
        rec->ptr = (uintptr_t)actors[i];
        memcpy(rec->body, actors[i], sizeof(ManbowActor2D));
        // Snapshot the animation controller's playback head. The actor's
        // anim_controller is a shared_ptr at +0x3C; deref to the
        // ManbowAnimationController2D and copy its first ANIM_SNAP_BYTES.
        void* ctrl = *(void**)((char*)actors[i] + ACTOR_ANIM_CTRL_OFF);
        if (ctrl) {
            rec->ctrl_present = 1;
            memcpy(rec->ctrl, ctrl, ANIM_SNAP_BYTES);
        } else {
            rec->ctrl_present = 0;
            memset(rec->ctrl, 0, ANIM_SNAP_BYTES);
        }
        p += sizeof(ActorRecord);
    }

    // Trailer 1: [uint32 text_len][text blob]. The value-based Squirrel
    // walker — LEGACY. It is the restore mechanism ONLY when arena rollback
    // is off (bring-up fallback). With arena rollback ON it MUST NOT run:
    // `save_battle` mutates Squirrel VM state (it parks closures + clones
    // into `__gekko_state._keep`, allocating arena objects and shifting
    // refcounts). Those mutations land inside the sq_arena snapshot, so a
    // re-sim — which re-saves each rolled-back frame — runs `save_battle`
    // extra times vs the forward pass and the arena diverges (an SQClosure
    // refcount drifts +1). The save callback must be a pure read; the raw
    // arena snapshots in Trailer 2 are the real, lossless restorable state.
    uint32_t actors_end = (uint32_t)(p - static_cast<uint8_t*>(buf));
    if (actors_end + 4 > cap) return actors_end;
    uint32_t* sq_len_field = (uint32_t*)p;
    p += 4;
    uint32_t sq_cap = (cap - actors_end - 4);
    uint8_t* text_blob = p;
    uint32_t sq_n = (g_sq_save_enabled && !g_arena_rollback)
                        ? call_squirrel_save(p, sq_cap, frame) : 0;
    *sq_len_field = sq_n;
    p += sq_n;

    // Trailer 2: arena rollback snapshot — the real restorable state.
    // Three length-prefixed sections: sq_arena (Squirrel VM heap),
    // battle_pools (battle object pools) and engine_snap (the scheduler's
    // fixed-address objects, sentinels and counters). The text blob above
    // is kept only for the value-based desync checksum; this trailer is
    // what load() restores from when g_arena_rollback is on.
    uint8_t* trailer2_start = p;
    // Byte span of the cpp_arena (render-signal) section within trailer2, so the
    // desync checksum can skip it (see below). Empty span when rollback is off.
    uint8_t* cpp_sect_start = p;
    uint8_t* cpp_sect_end   = p;
    if (g_arena_rollback) {
        auto put_section = [&](const char* name, auto save_fn) -> bool {
            uint32_t off = (uint32_t)(p - static_cast<uint8_t*>(buf));
            if (off + 4 > cap) {
                log_printf("[gekko_bridge] !! arena save: no room for "
                           "%s length\n", name);
                return false;
            }
            uint32_t* len_field = (uint32_t*)p;
            p += 4;
            uint32_t wrote = save_fn(p, cap - off - 4);
            // Every section's save() returns 0 only on overflow — a valid
            // section is never empty. Treat 0 as a hard failure instead of
            // silently writing a 0-length (skipped-on-load) section.
            if (wrote == 0) {
                log_printf("[gekko_bridge] !! section '%s' save overflowed "
                           "(avail=%u) — bump state_size\n",
                           name, cap - off - 4);
                *len_field = 0;
                return false;
            }
            *len_field = wrote;
            p += wrote;
            return true;
        };
        // cpp_arena holds render-signal state (DrawCommandSlot / boost::signals2
        // grouped-signal connection nodes). That is NOT simulation state and the
        // rollback re-sim is headless by design, so it legitimately differs
        // forward-vs-re-sim. We still SAVE + restore it (snapshot_ring) for visual
        // correctness, but EXCLUDE its bytes from the desync checksum below. Track
        // its span so the checksum can skip it.
        bool ok = put_section("sq_arena",  &sq_arena::save)
               && put_section("pools",     &battle_pools::save)
               && put_section("boostpools", &battle_pools::boostpool_save)
               && put_section("engine",    &engine_snap::save);
        cpp_sect_start = p;
        ok = ok && put_section("cpp_arena", &cpp_arena::save);
        cpp_sect_end = p;
        ok = ok && put_section("bullet",    &bullet_arena::save)
                && put_section("input",     &input_rec_save);
        if (!ok) {
            log_printf("[gekko_bridge] !! arena save overflow frame=%u — "
                       "bump GekkoConfig::state_size\n", frame);
        }
        static uint32_t arena_log = 4;
        if (arena_log > 0) {
            --arena_log;
            log_printf("[gekko_bridge] arena save f=%u total=%u sq_used=%u\n",
                       frame, (uint32_t)(p - static_cast<uint8_t*>(buf)),
                       sq_arena::used());
        }
    }

    uint32_t written = (uint32_t)(p - static_cast<uint8_t*>(buf));

    if (out_checksum) {
        // Desync checksum. With arena rollback on, checksum the Trailer-2
        // sections — that is the exact, deterministic state load() restores
        // from (the arena allocators make addresses reproducible, so a
        // forward save and its rollback re-sim of the same frame produce
        // identical bytes when the logical state matches). With arena
        // rollback off, fall back to the legacy value-based text blob.
        if (g_arena_rollback) {
            // Checksum the SIM sections only — skip the cpp_arena (render-signal)
            // span. Render != simulation and the re-sim is headless, so cpp_arena
            // legitimately differs forward-vs-re-sim; including it would flag false
            // desyncs. cpp_arena is still saved+restored for visuals; the sim is
            // fully covered by sq/pools/boostpools/engine/bullet/input.
            uint32_t a = fletcher32_acc(0xFFFFFFFFu, trailer2_start,
                                        (size_t)(cpp_sect_start - trailer2_start));
            a = fletcher32_acc(a, cpp_sect_end, (size_t)(p - cpp_sect_end));
            *out_checksum = a;
        } else {
            *out_checksum = sq_n > 0 ? fletcher32(text_blob, sq_n) : 0;
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

    // Arena rollback: roll the big arenas back via snapshot_ring and restore
    // the small sections from that frame's blob. See save_state_to_buf.
    if (g_arena_rollback && snapshot_ring::armed()) {
        uint32_t sl = 0;
        const uint8_t* sblob = snapshot_ring::restore(hdr->frame, &sl);
        if (sblob) {
            const uint8_t* sp   = sblob;
            const uint8_t* send = sblob + sl;
            auto sect = [&](auto load_fn) {
                if (sp + 4 > send) return;
                uint32_t w = *(const uint32_t*)sp;
                sp += 4;
                if (sp + w > send) return;
                load_fn(sp, w);
                sp += w;
            };
            sect(&battle_pools::load);
            battle_pools::set_load_frame((int)hdr->frame);  // probe: which save is being restored
            sect(&battle_pools::boostpool_load);   // Sqrat math boost::pools (this.va/vf/vfBaria)
            // cpp_arena is now dirty-page-snapshotted by snapshot_ring, not full-copied in the small blob.
            sect(&engine_snap::load);
            sect(&input_rec_load);
            sect(&input_hist::load);
        }
        // (trace_reset moved into advance_one_frame — see save_state_to_buf note.)
        return;
    }

    const uint8_t* p = static_cast<const uint8_t*>(buf) + sizeof(SaveHeader);
    uint32_t need = hdr->actor_count * (uint32_t)sizeof(ActorRecord);
    if (len - sizeof(SaveHeader) < need) {
        log_printf("gekko_bridge::load_state: short actor blob "
                   "(have %u need %u)\n",
                   (uint32_t)(len - sizeof(SaveHeader)), need);
        return;
    }

    // Pointer-as-join-key: match each saved record to a still-live actor
    // by address. The raw 0xEC ManbowActor2D body is, however,
    // intentionally NOT restored — see the loop body.
    static constexpr size_t MAX_ACTORS = 2048;
    ManbowActor2D* live[MAX_ACTORS];
    size_t live_n = live_actors::snapshot(live, MAX_ACTORS);

    // saved_ptrs = the exact live actor set at the frame being loaded.
    // Anything live now but absent from it was spawned AFTER this frame.
    std::unordered_set<uintptr_t> saved_ptrs;
    saved_ptrs.reserve(hdr->actor_count * 2 + 8);
    {
        const uint8_t* q = p;
        for (uint32_t i = 0; i < hdr->actor_count; ++i) {
            saved_ptrs.insert(((const ActorRecord*)q)->ptr);
            q += sizeof(ActorRecord);
        }
    }

    size_t restored = 0, missing = 0;
    for (uint32_t i = 0; i < hdr->actor_count; ++i) {
        const ActorRecord* rec = (const ActorRecord*)p;
        ManbowActor2D* target = nullptr;
        for (size_t j = 0; j < live_n; ++j) {
            if ((uintptr_t)live[j] == rec->ptr) { target = live[j]; break; }
        }
        if (target) {
            // Roll back the actor's alive/inert state. active_flags (@0x70,
            // a single byte) is what the group's per-frame loop tests:
            // it ticks/renders only actors with bit 0 set. Restoring it
            // from the saved body re-activates an actor that a prior
            // rollback parked inert, and is a pure value write — no
            // pointer/satellite hazard.
            target->active_flags = rec->body[0x70];
            // The raw 0xEC ManbowActor2D body is NOT memcpy'd back: it is
            // dense with pointers to satellite heap objects not in the
            // snapshot — the task linked-list (head @ 0xD0), the
            // SqratFunction HSQOBJECTs (0xA8 / 0xBC). Restoring those raw
            // makes them dangle (a stale 0xD0 head walks Actor2D::Update
            // into a freed task node — crash at th155.exe+0xC12B9,
            // VEH-confirmed). The actor's spatial state is carried by the
            // Squirrel blob; the C++ task-list rollback is separate work.
            //
            // The animation controller IS restored. It is a stable
            // per-actor object (lives with the actor, not reallocated
            // mid-round) so its playback fields can be written back in
            // place. It is NOT in the Squirrel blob — without this, a
            // rollback leaves the C++ anim frame un-rewound, end-of-motion
            // callbacks fire off-by-one, and they reset the character's
            // Squirrel `count` at the wrong frame: the round-transition
            // desync (#58). Restore only the playback fields:
            //   base   0x1C..0x28 — motion, key_take, key_frame
            //   2D     0x124..0x13F — anim_set, take, animation_data,
            //                         frame, frame_again, speed, flags
            // Skip the colour/matrix render state and the hitbox
            // std::vectors (derived; recomputed each frame).
            if (rec->ctrl_present) {
                void* ctrl = *(void**)((char*)target + ACTOR_ANIM_CTRL_OFF);
                if (ctrl) {
                    memcpy((char*)ctrl + 0x1C,  rec->ctrl + 0x1C,  0x28 - 0x1C);
                    memcpy((char*)ctrl + 0x124, rec->ctrl + 0x124, 0x13F - 0x124);
                }
            }
            ++restored;
        } else {
            ++missing;
        }
        p += sizeof(ActorRecord);
    }

    // Reconcile the live actor SET back to the saved frame. Any actor live
    // now but absent from the save was spawned by a forward-sim Advance
    // that this rollback is undoing — e.g. a round-transition effect
    // actor. `live_actors`' g_live is process-global and never shrinks
    // (defer_release), so without this every re-sim that crosses a spawn
    // accumulates a duplicate and the actor set at a given gekko frame
    // becomes path-dependent — the round-transition desync (#58, proven:
    // frame 234 saved with 12 actors on most paths, 13 on the minimal
    // re-sim). Park each stray inert: active_flags=4 is the engine's
    // released value (bit 0 clear → the group loop skips it for
    // update+render). We deliberately do NOT set group->pending_release,
    // so the group's cleanup pass never runs and the actor is never
    // pool-freed mid-window — that pool-free is what crashed earlier
    // (VEH-confirmed). The object leaks until disarm; bounded by round
    // length. The re-sim re-creates whatever the deterministic logic
    // spawns.
    size_t culled = 0;
    if (!g_arena_rollback) {
        for (size_t j = 0; j < live_n; ++j) {
            if (saved_ptrs.find((uintptr_t)live[j]) == saved_ptrs.end()) {
                live[j]->active_flags = 4;
                ++culled;
            }
        }
    }

    // Only chatter when the restore was not a clean 1:1.
    if (missing || culled) {
        log_printf("gekko_bridge::load_state: restored %zu/%u "
                   "(%zu missing, %zu culled)\n",
                   restored, hdr->actor_count, missing, culled);
    }

    // Trailer 1: [uint32 text_len][text blob].
    if (len - sizeof(SaveHeader) < need + 4) {
        static bool no_trailer_logged = false;
        if (!no_trailer_logged) {
            no_trailer_logged = true;
            log_printf("[gekko_bridge] load: NO trailer (len=%u need=%u)\n",
                       len, need);
        }
        return;
    }
    const uint32_t text_len = *(const uint32_t*)p;
    const uint8_t* text_blob = p + 4;

    // Arena rollback path — the real restore. Trailer 2 follows the text
    // blob: three length-prefixed sections (sq_arena, battle_pools,
    // engine_snap) restored as one consistent unit. Each section is
    // range-checked before use; a truncated section aborts the rest
    // rather than reading past the blob.
    if (g_arena_rollback) {
        const uint8_t* ap   = text_blob + text_len;
        const uint8_t* aend = static_cast<const uint8_t*>(buf) + len;
        auto get_section = [&](const char* name, auto load_fn) -> bool {
            if (ap + 4 > aend) {
                log_printf("[gekko_bridge] load: %s section truncated\n",
                           name);
                return false;
            }
            uint32_t slen = *(const uint32_t*)ap;
            ap += 4;
            if (ap + slen > aend) {
                log_printf("[gekko_bridge] load: %s section overrun "
                           "(len=%u)\n", name, slen);
                return false;
            }
            load_fn(ap, slen);
            ap += slen;
            return true;
        };
        if (get_section("sq_arena", &sq_arena::load) &&
            get_section("pools",    &battle_pools::load) &&
            get_section("boostpools", &battle_pools::boostpool_load)) {
            if (get_section("engine", &engine_snap::load) &&
                get_section("cpp_arena", &cpp_arena::load) &&
                get_section("bullet", &bullet_arena::load))
                get_section("input", &input_rec_load);
        }
        return;
    }

    // Text-walker restore path — lossy fallback, used only when arena
    // rollback is off (bring-up / diagnostics).
    if (text_len > 0 &&
        (size_t)(text_blob - static_cast<const uint8_t*>(buf)) + text_len <= len) {
        call_squirrel_load(text_blob, text_len, hdr->frame);
    }
}

// DIAGNOSTIC — fingerprint the whole rollback state at the top of an
// advance. Re-serialises each snapshot section from live memory and logs
// --- sq_arena divergence locator -----------------------------------------
// Layer 4: with the C++ side deterministic, the ONLY section that diverges
// on re-sim is sq_arena (the Squirrel VM heap), inside RunOneFrame. This
// keeps each forward frame's full sq_arena blob and, on a re-sim of the same
// frame, byte-diffs them — reporting the first diverging offset and dumping
// the surrounding DWORDs (fwd vs resim) so the Squirrel object that went
// non-deterministic can be identified.
namespace {
static constexpr int      SQ_DIFF_RING = 8;       // covers the first rollback
static constexpr uint32_t SQ_BLOB_CAP  = 20u * 1024 * 1024;
struct SqBlobEntry { int frame; uint32_t len; uint8_t* buf; };
static SqBlobEntry g_sqpd[SQ_DIFF_RING];
static bool        g_sqpd_init = false;
static bool        g_sqpd_done = false;

static void sq_page_diff(const uint8_t* blob, uint32_t len, int frame, int rb) {
    if (g_sqpd_done || frame < 0 || !blob || !len) return;
    if (!g_sqpd_init) {
        for (int i = 0; i < SQ_DIFF_RING; ++i) {
            g_sqpd[i].frame = -1;
            g_sqpd[i].buf   = (uint8_t*)VirtualAlloc(
                nullptr, SQ_BLOB_CAP, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        }
        g_sqpd_init = true;
    }
    SqBlobEntry& e = g_sqpd[((frame % SQ_DIFF_RING) + SQ_DIFF_RING) % SQ_DIFF_RING];
    if (!e.buf || len > SQ_BLOB_CAP) return;

    if (rb == 0) {                       // forward — store
        memcpy(e.buf, blob, len);
        e.frame = frame;
        e.len   = len;
        return;
    }
    if (e.frame != frame || e.len == 0) return;   // re-sim — compare
    uint32_t d = 0, m = len < e.len ? len : e.len;
    while (d < m && e.buf[d] == blob[d]) ++d;
    if (d >= m && len == e.len) return;           // identical
    g_sqpd_done = true;
    log_printf("[sqdiff] *** sq_arena DIVERGES frame=%d  first-diff @0x%X  "
               "(fwd-len=%u resim-len=%u)\n", frame, d, e.len, len);
    uint32_t base = d > 0x40 ? (d - 0x40) & ~0xFu : 0;
    for (uint32_t o = base; o < base + 0xC0 && o + 4 <= m; o += 4) {
        uint32_t fv = *(const uint32_t*)(e.buf + o);
        uint32_t rv = *(const uint32_t*)(blob + o);
        log_printf("[sqdiff]   @0x%X  fwd=%08X  resim=%08X %s\n",
                   o, fv, rv, fv != rv ? "<-- DIFF" : "");
    }
    // Layer-4: resolve the diverging arena block to a named Squirrel class.
    rollback_identify_sqdiff(e.buf, blob, m, d);
    // Dump the engine input devices so the diverging InputCommand.device
    // can be matched against what input_rec_save captures.
    if (g_active_input_session) {
        log_printf("[sqid] session local_input=%p device_vec.size=%u\n",
                   g_active_input_session->local_input,
                   (unsigned)g_active_input_session->device_vec.size());
        for (size_t i = 0; i < g_active_input_session->device_vec.size(); ++i)
            log_printf("[sqid]   device_vec[%zu]=%p\n",
                       i, g_active_input_session->device_vec[i].get());
        auto* rec = g_active_input_session->input_recorder.get();
        if (rec) {
            for (size_t i = 0; i < rec->devices.size(); ++i) {
                auto* dd = rec->devices[i].get();
                log_printf("[sqid]   recorder dev[%zu]=%p tf4_device=%p\n",
                           i, dd, dd ? dd->tf4_device : nullptr);
            }
        }
    }
}
// --- cpp_arena divergence locator (mirrors sq_page_diff) -----------------
// cpp_arena toggles between two fingerprints on re-sim — a tiny operator-new
// / free divergence. This keeps each forward frame's cpp_arena blob and, on a
// re-sim, byte-diffs it, decoding the first diff to a Meta field or an arena
// block (its Hdr: size-class, reqsize, allocated/free).
static constexpr int      CPP_DIFF_RING = 8;
static constexpr uint32_t CPP_BLOB_CAP  = 4u * 1024 * 1024;
struct CppBlobEntry { int frame; uint32_t len; uint8_t* buf; };
static CppBlobEntry g_cppd[CPP_DIFF_RING];
static bool         g_cppd_init = false;
static bool         g_cppd_done = false;

static void cpp_page_diff(const uint8_t* blob, uint32_t len, int frame, int rb) {
    if (g_cppd_done || frame < 0 || !blob || !len) return;
    if (!g_cppd_init) {
        for (int i = 0; i < CPP_DIFF_RING; ++i) {
            g_cppd[i].frame = -1;
            g_cppd[i].buf = (uint8_t*)VirtualAlloc(nullptr, CPP_BLOB_CAP,
                                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        }
        g_cppd_init = true;
    }
    CppBlobEntry& e = g_cppd[((frame % CPP_DIFF_RING) + CPP_DIFF_RING) % CPP_DIFF_RING];
    if (!e.buf || len > CPP_BLOB_CAP) return;
    if (rb == 0) { memcpy(e.buf, blob, len); e.frame = frame; e.len = len; return; }
    if (e.frame != frame || e.len == 0) return;
    uint32_t d = 0, m = len < e.len ? len : e.len;
    while (d < m && e.buf[d] == blob[d]) ++d;
    if (d >= m && len == e.len) return;                       // identical
    g_cppd_done = true;
    log_printf("[cppdiff] *** cpp_arena DIVERGES frame=%d first-diff@0x%X "
               "(fwd-len=%u resim-len=%u)\n", frame, d, e.len, len);
    // cpp_arena Meta = {magic, bump, live_bytes, free_off[21], reserved[8]}
    // = 0x80 bytes; the first block follows at 0x80.
    if (d < 0x80) {
        const char* fld = "reserved";
        uint32_t fo = d;
        if      (fo < 4)   fld = "magic";
        else if (fo < 8)   fld = "bump";
        else if (fo < 12)  fld = "live_bytes";
        else if (fo < 12 + 21 * 4) fld = "free_off[]";
        log_printf("[cppdiff]   diff in Meta field '%s' off=0x%X  fwd=%08X "
                   "resim=%08X\n", fld, fo,
                   *(const uint32_t*)(e.buf + (d & ~3u)),
                   *(const uint32_t*)(blob + (d & ~3u)));
        if (fo >= 12 && fo < 12 + 21 * 4)
            log_printf("[cppdiff]   -> free-list head for size-class %u\n",
                       (fo - 12) / 4 + 4);
        return;
    }
    uint32_t off = 0x80;
    while (off + 16 <= m) {
        uint32_t cls = *(const uint32_t*)(e.buf + off);
        uint32_t req = *(const uint32_t*)(e.buf + off + 4);
        uint32_t mag = *(const uint32_t*)(e.buf + off + 12);
        if (cls < 4 || cls > 24) { log_printf("[cppdiff]   walk lost @0x%X\n", off); return; }
        uint32_t bsz = 1u << cls;
        if (d >= off && d < off + bsz) {
            log_printf("[cppdiff]   diff in block @0x%X cls=%u (%uB) reqsize=%u "
                       "magic=%08X %s  field-off=0x%X\n", off, cls, bsz, req, mag,
                       mag == 0x42504143 ? "ALLOCATED" : (mag ? "?" : "FREE"),
                       d - off);
            for (uint32_t o = off; o < off + bsz && o + 4 <= m && o < off + 0x60; o += 4) {
                uint32_t fv = *(const uint32_t*)(e.buf + o);
                uint32_t rv = *(const uint32_t*)(blob + o);
                log_printf("[cppdiff]   +0x%02X fwd=%08X resim=%08X %s\n",
                           o - off, fv, rv, fv != rv ? "<--" : "");
            }
            return;
        }
        off += bsz;
    }
    log_printf("[cppdiff]   offset 0x%X unresolved\n", d);
}
} // namespace

// Rollback determinism diagnostics — per-section state fingerprints, the
// per-pool checksum dump (battle_pools::log_fingerprint), and the divergence
// locator (diff_locate) — are HEAVY: each advance hashes/serializes ~50 MB,
// and a GekkoStressSession pays that 9x per displayed frame (1 forward + 8
// re-sim). That alone drags the solo rig to ~1 fps. Off by default; set
// SQUIROLL_RB_DIAG=1 to turn them back on when chasing a desync.
static bool rb_diag_enabled() {
    static int v = -1;
    if (v < 0) { char b[8]; v = GetEnvironmentVariableA("SQUIROLL_RB_DIAG", b, sizeof(b)) ? 1 : 0; }
    return v != 0;
}

// a per-section fletcher checksum. Comparing the forward advance(N) line
// against the re-sim advance(N) line shows whether re-sim resumes from the
// exact state the forward run did — and if not, which section diverged.
static void log_state_fingerprint(int frame, int rb, const char* tag) {
    if (!rb_diag_enabled()) return;
    static int quota = 240;
    if (quota <= 0) return;
    --quota;
    static uint8_t* scratch = nullptr;
    static constexpr uint32_t SCRATCH = 24u * 1024 * 1024;
    if (!scratch) {
        scratch = (uint8_t*)VirtualAlloc(nullptr, SCRATCH,
                                         MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!scratch) return;
    }
    auto cs = [&](uint32_t n) -> uint32_t {
        return n ? fletcher32(scratch, n) : 0u;
    };
    uint32_t sq_len = sq_arena::save(scratch, SCRATCH);
    uint32_t a = cs(sq_len);
    // Layer-4 locator: the divergence appears in RunOneFrame, so diff
    // sq_arena at "post-run" (logged right after update_related).
    if (tag[5] == 'r' /* "post-run" */)
        sq_page_diff(scratch, sq_len, frame, rb);
    uint32_t p = cs(battle_pools::save(scratch, SCRATCH));
    uint32_t e = cs(engine_snap::save(scratch, SCRATCH));
    uint32_t cpp_len = cpp_arena::save(scratch, SCRATCH);
    uint32_t c = cs(cpp_len);
    if (tag[5] == 'r' /* "post-run" */)
        cpp_page_diff(scratch, cpp_len, frame, rb);
    uint32_t bt = cs(bullet_arena::save(scratch, SCRATCH));
    uint32_t in = cs(input_rec_save(scratch, SCRATCH));
    log_printf("[diag] %-9s f=%d rb=%d  arena=%08x pools=%08x eng=%08x "
               "cpp=%08x bullet=%08x input=%08x\n",
               tag, frame, rb, a, p, e, c, bt, in);
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

// --- input-recorder snapshot ------------------------------------------------
// th155's input recorder is per-frame-mutable state the battle reads but no
// arena captures: each TF4InputRecorderDevice has a read/write cursor + an
// input_vec history ring, and each TF4InputDevice (0x140) carries cumulative
// button hold-counters. Fighting-game motion/command detection reads the
// input HISTORY, so if the recorder is not rewound with the rest of the
// state a re-sim reads a shifted history window and diverges. This section
// snapshots both, per device.
static uint32_t input_rec_save(uint8_t* out, uint32_t cap) {
    uint8_t* p = out;
    uint8_t* end = out + cap;
    auto put = [&](const void* s, uint32_t n) -> bool {
        if (p + n > end) return false;
        memcpy(p, s, n); p += n; return true;
    };
    uint32_t magic = 0x52504E49;  // 'INPR'
    uint32_t ndev = 0;
    ManbowInputRecorder* rec = g_active_input_session
        ? g_active_input_session->input_recorder.get() : nullptr;
    if (rec) ndev = (uint32_t)rec->devices.size();
    if (ndev > 2) ndev = 2;
    if (!put(&magic, 4) || !put(&ndev, 4)) return 0;
    for (uint32_t i = 0; i < ndev; ++i) {
        TF4InputRecorderDevice* d = rec->devices[i].get();
        uint32_t wi = 0, ri = 0, b8 = 0, vs = 0, hasdev = 0;
        if (d) {
            wi = (uint32_t)d->input_write_idx;
            ri = (uint32_t)d->input_read_idx;
            b8 = d->__bool_8;
            vs = (uint32_t)d->input_vec.size();
            hasdev = d->tf4_device ? 1u : 0u;
        }
        if (!put(&wi, 4) || !put(&ri, 4) || !put(&b8, 4) ||
            !put(&vs, 4) || !put(&hasdev, 4))
            return 0;
        if (d && vs && !put(d->input_vec.data(), vs * 2)) return 0;
        if (hasdev && !put(d->tf4_device, sizeof(TF4InputDevice))) return 0;
    }
    return (uint32_t)(p - out);
}

static void input_rec_load(const uint8_t* blob, uint32_t len) {
    const uint8_t* p = blob;
    const uint8_t* e = blob + len;
    auto get = [&](void* d, uint32_t n) -> bool {
        if (p + n > e) return false;
        memcpy(d, p, n); p += n; return true;
    };
    uint32_t magic = 0, ndev = 0;
    if (!get(&magic, 4) || !get(&ndev, 4) || magic != 0x52504E49) return;
    ManbowInputRecorder* rec = g_active_input_session
        ? g_active_input_session->input_recorder.get() : nullptr;
    for (uint32_t i = 0; i < ndev; ++i) {
        uint32_t wi = 0, ri = 0, b8 = 0, vs = 0, hasdev = 0;
        if (!get(&wi, 4) || !get(&ri, 4) || !get(&b8, 4) ||
            !get(&vs, 4) || !get(&hasdev, 4))
            return;
        TF4InputRecorderDevice* d =
            (rec && i < rec->devices.size()) ? rec->devices[i].get() : nullptr;
        if (d) {
            d->input_write_idx = wi;
            d->input_read_idx  = ri;
            d->__bool_8        = (bool)b8;
            d->input_vec.resize(vs);
            if (vs && p + vs * 2 <= e)
                memcpy(d->input_vec.data(), p, vs * 2);
        }
        p += vs * 2;
        if (hasdev) {
            if (p + sizeof(TF4InputDevice) > e) return;
            if (d && d->tf4_device)
                memcpy(d->tf4_device, p, sizeof(TF4InputDevice));
            p += sizeof(TF4InputDevice);
        }
    }
}

// SAFE X-FINDER (dump only, no restore): at advance-ENTRY for f=15 (so the state
// is f=14: loaded-from-save(14) on the depth-1 re-sim, live on the forward),
// checksum every committed writable region. The saved regions (arenas/.data) will
// match fwd vs depth-1; the region whose cs DIFFERS is where the un-saved carry-
// over X lives -> snapshot just that next.
static void log_region_checksums() {
    if (g_trace_frame != 15) return;
    uintptr_t p = 0x00400000; int n = 0;
    while (p < 0x40000000 && n < 600) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((void*)p, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        uintptr_t rend = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE)) &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            uint32_t sz = mbi.RegionSize > 0x10000u ? 0x10000u : (uint32_t)mbi.RegionSize;
            uint32_t cs = fletcher32((const uint8_t*)mbi.BaseAddress, sz);
            log_printf("[rgncs] f=15 rb=%d d=%d base=%08X size=%08X cs=%08X\n",
                       g_trace_rb, g_trace_depth, (uint32_t)(uintptr_t)mbi.BaseAddress,
                       (uint32_t)mbi.RegionSize, cs);
            ++n;
        }
        if (rend <= p) break;
        p = rend;
    }
}

void advance_one_frame() {
    // log_region_checksums();   // X-finder probe (done: X is real-heap, not .data)
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
    static int log_quota = 40;
    bool trace = log_quota > 0;
    if (trace) {
        --log_quota;
        log_printf("[gekko_bridge] advance: enter "
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
    // Reset th155's per-frame BUMP ALLOCATOR. g_frame_alloc_ptr (0x4DAD1C)
    // is a linear allocator into a fixed block (base = g_frame_alloc_base
    // 0x4DAD20, allocated once at init); the engine bump-allocates per-frame
    // scratch from it (collision lists, ContactResultActor, ...) and resets
    // the pointer to the base every logical frame. That reset lives at the
    // top of update_logic (0xE1A0) — which this custom advance deliberately
    // does NOT call. Without it the pointer climbs unbounded: it overruns
    // the block (corrupting the MeshVertex pool) AND, because it is never
    // rewound, a rollback re-sim allocates from a different offset, so
    // pointers the engine caches into persistent objects (e.g.
    // Actor2DGroup+0x7C) diverge → false-positive desync.
    //
    // The pointer reset alone is NOT enough: the allocator's BLOCK CONTENT
    // is not part of the rollback snapshot. After a rollback, re-sim frame N
    // sees whatever the latest forward frame left in that scratch block, not
    // what forward frame N saw — so any uninitialised read from this frame's
    // scratch allocations diverges. Zeroing the region the previous frame
    // used (before resetting the pointer) makes the scratch block a
    // deterministic function of the frame: every advance starts it clean.
    {
        uint32_t fa_base = *(uint32_t*)(0x4DAD20_R);
        uint32_t fa_ptr  = *(uint32_t*)(0x4DAD1C_R);
        if (fa_ptr > fa_base && fa_ptr - fa_base < 0x8000000u)
            memset((void*)(uintptr_t)fa_base, 0, fa_ptr - fa_base);
        *(uint32_t*)(0x4DAD1C_R) = fa_base;   // reset the bump pointer
    }
    // I2: Dr0 write-watch on the deterministic crash DrawCommandSlot head.
    // The +0x305AE crash reads head=0xFFFFFF00 from the slot at 0x37000870
    // (render region, alloc site create_and_bind 0x56AB5, address stable
    // across runs thanks to the fixed arena bases). SQUIROLL_WP_RR=<hexaddr>
    // (or =1 for the default) arms at f>=1 to catch the corrupting writer.
    {
        static int wp_rr = -2;   // -2 unparsed, 0 off, else address
        if (wp_rr == -2) {
            char b[24] = {0};
            DWORD n = GetEnvironmentVariableA("SQUIROLL_WP_RR", b, sizeof b);
            wp_rr = 0;
            if (n > 0) {
                uint32_t v = 0;
                for (const char* s = b; *s; ++s) {
                    char c = *s | 0x20;
                    if (c >= '0' && c <= '9') v = v * 16 + (c - '0');
                    else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
                    else if (c == 'x') v = 0;
                    else break;
                }
                wp_rr = (v > 0x10000) ? (int)v : (int)0x37000870u;
            }
        }
        static bool wp_rr_armed = false;
        if (wp_rr && !wp_rr_armed && g_trace_rb == 0 && g_trace_frame >= 1) {
            wp_rr_armed = true;
            crash_handler::watchpoint_arm((void*)(uintptr_t)(uint32_t)wp_rr);
        }
    }
    if (trace) log_printf("[gekko_bridge] advance: -> update_related\n");
    // cpp_arena stays armed for the whole match (armed once at session arm).
    // It MUST capture every th155 operator-new — including the animation
    // system's CompositeSprite std::vector buffers, which the renderer also
    // reallocates. Arming only per-advance let a render-time realloc escape
    // to the real Win32 heap; a later advance then freed that buffer, and a
    // rollback re-sim freed it AGAIN (the real heap is not snapshotted) —
    // STATUS_HEAP_CORRUPTION. With cpp_arena always armed the buffer lives in
    // the captured arena, so its alloc/free is rolled back like everything
    // else. (The cpp_arena divergence this gating was meant to fix was
    // actually the InputHistory vector — fixed by input_hist.)
    // On a re-sim advance, log any C++ exception thrown inside the game
    // update — an uncaught throw here terminates with no crash report.
    if (g_trace_rb) crash_handler::watch_cxx(true);
    // Mark the re-sim so cpp_arena suppresses real-heap frees (the real heap
    // is not snapshotted — a re-sim re-free would double-free).
    cpp_arena::set_resim(g_trace_rb != 0);
    cpp_arena::trace_reset();                               // start THIS advance's alloc trace
    update_related(*MAIN_SCRIPTAPI_PTR);                    // RunOneFrame(g_main), once
    log_state_fingerprint(g_trace_frame, g_trace_rb, "post-run");
    if (rb_diag_enabled()) battle_pools::log_fingerprint("post-run");
    if (trace) log_printf("[gekko_bridge] advance: -> ScriptAPI::Update\n");
    Act_ScriptAPI_ptr->vftable->Update(Act_ScriptAPI_ptr);  // Act::ScriptAPI::Update
    // B1: rebuild trail ribbon meshes deterministically in the SIM (fwd AND re-sim),
    // BEFORE the save — the render-only build is forward-only and is the residual cpp
    // divergence + the texture-refcount crash. Must run while still marked resim so
    // any internal frees route correctly; runs on both sides identically.
    cpp_arena::rebuild_trail_meshes();
    if (g_trace_rb) crash_handler::watch_cxx(false);
    cpp_arena::set_resim(false);
    log_state_fingerprint(g_trace_frame, g_trace_rb, "post-upd");
    // Post-advance battle_pools diff. diff_locate at adv-top only compares
    // load-state — i.e. the snapshot's deterministic round-trip — and rarely
    // diverges in our setup. The interesting divergence is what advance()
    // PRODUCED: the f=15 1-of-8 actor velocity write that ripples through
    // sq/bt. Capture the forward post-state per frame, compare every re-sim's
    // post-state against it, and dump the first diverging field (with HW
    // write-watch armed) on each non-matching re-sim.
    if (rb_diag_enabled()) {
        battle_pools::diff_locate(g_trace_frame, g_trace_rb);
    }
    // [engdiff] .data divergence locator — RE-ENABLED: the residual intermittent
    // desync is eng (.data) diverging on some rollback re-sims. This pins the exact
    // .data offset. ([bplive] stays off — bp is clean.)
    engine_snap::diff_locate(g_trace_frame, g_trace_rb);
    // battle_pools::diff_live(g_trace_frame, g_trace_rb);

    // Self-terminate at a target battle frame so diagnostic runs exit cleanly.
    // The solo stress harness otherwise HANGS at the round end (~f=70), keeping
    // Netcode.dll locked against the next build.sh deploy. SQUIROLL_EXIT_FRAME=N
    // (default off) ExitProcess()es once the forward sim passes frame N.
    if (g_trace_rb == 0) {
        static DWORD t_start = 0;
        if (t_start == 0) t_start = GetTickCount();
        // HANG WATCHDOG: the residual failure mode is a silent stall (forward
        // frame stops advancing, no exception). If f hasn't moved for 20s,
        // dump every thread's stack and exit(3) — a diagnosed artifact instead
        // of a harness timeout. Started lazily on the first forward advance.
        static HANDLE wd = nullptr;
        if (!wd) {
            wd = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
                int last = -1; DWORD since = GetTickCount();
                for (;;) {
                    Sleep(3000);
                    int f = g_trace_frame;
                    if (f != last) { last = f; since = GetTickCount(); continue; }
                    if (GetTickCount() - since >= 20000) {
                        log_printf("[watchdog] forward frame STUCK at f=%d for "
                                   "20s — dumping stacks\n", f);
                        crash_handler::dump_all_thread_stacks("hang watchdog");
                        log_flush();
                        Sleep(600);
                        ExitProcess(3);
                    }
                }
            }, nullptr, 0, nullptr);
        }
        // Ungated heartbeat — proves how far the forward sim actually got (the
        // [adv]/[save] logs are rate-gated and stop early). One line per 30 frames.
        if ((g_trace_frame % 30) == 0)
            log_printf("[hb] forward f=%d t=%ums\n", g_trace_frame,
                       GetTickCount() - t_start);

        static int exit_frame = -2;
        if (exit_frame == -2) {
            char buf[16] = {0};
            DWORD n = GetEnvironmentVariableA("SQUIROLL_EXIT_FRAME", buf, sizeof buf);
            int v = 0;
            for (const char* s = buf; *s >= '0' && *s <= '9'; ++s) v = v * 10 + (*s - '0');
            exit_frame = (n > 0 && v > 0) ? v : -1;
        }
        if (exit_frame > 0 && g_trace_frame >= exit_frame) {
            log_printf("[gekko_bridge] SQUIROLL_EXIT_FRAME=%d reached (f=%d) — exiting clean\n",
                       exit_frame, g_trace_frame);
            Sleep(400);            // let the async logger drain to disk
            ExitProcess(0);
        }
        // Wall-clock cap — run ~N seconds then exit clean (the preferred harness:
        // run for a fixed time, terminate only on desync/crash before then).
        static int exit_secs = -2;
        if (exit_secs == -2) {
            char buf[16] = {0};
            DWORD n = GetEnvironmentVariableA("SQUIROLL_EXIT_SECONDS", buf, sizeof buf);
            int v = 0;
            for (const char* s = buf; *s >= '0' && *s <= '9'; ++s) v = v * 10 + (*s - '0');
            exit_secs = (n > 0 && v > 0) ? v : -1;
        }
        if (exit_secs > 0 && (GetTickCount() - t_start) >= (DWORD)exit_secs * 1000) {
            log_printf("[gekko_bridge] SQUIROLL_EXIT_SECONDS=%d reached (f=%d, %ums) — exiting clean\n",
                       exit_secs, g_trace_frame, GetTickCount() - t_start);
            Sleep(400);
            ExitProcess(0);
        }
    }
    // cpp_arena divergence diagnostics ([cpptrace]/[allocdiag]) — DISABLED: cpp is
    // intentionally excluded from the desync checksum (render state), so it
    // diverges by design every frame. Diagnosing it just floods the log (the ~10
    // fps) and walks the divergent arena. Re-enable only when debugging cpp itself.
    // cpp_arena::trace_check(g_trace_frame, g_trace_rb);
    // cpp_arena::diag_alloc_counts(g_trace_frame, g_trace_rb);
    ++*(uint32_t*)(0x4DACE0_R);                             // g_frame_counter
    if (trace) log_printf("[gekko_bridge] advance: exit\n");
}

void render_one_frame() {
    // NOTE: bracketing this with cpp_arena::set_render_pass(true) to route render
    // allocations to the render region made cpp WORSE (24->29) — drawing_related
    // allocates SIM-referenced state too (the render-effect boost::signals2
    // connections live in the dispatch list RunOneFrame walks). Every attempt to
    // route the render allocs out by where/when they happen breaks the refs. The
    // set_render_pass hook stays available but is not used here. The render-effect
    // connections need to be separated by IDENTITY (which signal/list), or the
    // re-sim must replay the draw's sim side-effects — not a location heuristic.
    drawing_related();
}

// ---------------------------------------------------------------- session --

static void apply_test_round_frames();  // defined below; used by init/init_solo

bool init(uint16_t local_port, uint16_t remote_port,
          uint8_t local_player_idx, const char* remote_ip)
{
    if (g_session) return false;
    register_cpp_ser();
    fake_input_init();

    gekko_create(&g_session, GekkoGameSession);

    GekkoConfig config = {};
    config.desync_detection = true;
    config.input_size = sizeof(uint16_t);
    // The GekkoNet save blob is just the SaveHeader now — snapshot_ring owns
    // the real state (dirty-page ring for the arenas, its own small-section
    // ring). 1 MB is generous headroom for the header / the legacy bring-up
    // path; it must match the cap passed to save_state_to_buf below.
    config.state_size = 1 * 1024 * 1024;
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
    // Deferred release is OFF for the arena-rollback path. It no-ops
    // Actor2D::Release entirely, which keeps the C++ actor "alive" while
    // the Squirrel side of a dying actor tears down normally — the two
    // halves desync, and a diverging dual re-sim turns that into a
    // dangling Squirrel reference. battle_pools snapshots the actor pool
    // raw, so a normally-released actor is captured/restored correctly
    // without deferral. (Deferral remains for the legacy actor-record
    // path, g_arena_rollback off.)
    live_actors::set_defer_release(!g_arena_rollback);
    // Route th155's operator-new allocations into the snapshot-able arena
    // FIRST — must precede the vector re-homing below so the fresh buffers
    // land in cpp_arena.
    cpp_arena::set_armed(true);
    // Pre-grow the C++ battle object pools so their block set is frozen
    // for the match — the rollback snapshot copies those blocks raw.
    battle_pools::pregrow();
    tf4_pool::pregrow_objpools();        // freeze the generic-grow objpool family (f=34 hang)
    // Re-home each animation controller's CompositeSprite std::vector
    // buffers into cpp_arena (they were operator-new'd during vs.Initialize,
    // before cpp_arena was armed, so they sit uncaptured on the CRT heap).
    // Re-allocating them now — armed — puts them in the snapshot and at a
    // fixed 256-elem capacity so they never realloc/move mid-match.
    battle_pools::reserve_anim_vectors();
    // Fix each player's input-history vector capacity so it never reallocs
    // mid-match (its backing buffer then keeps a stable address to snapshot).
    input_hist::pregrow();
    // Arm dirty-page snapshotting: arenas installed, pools pre-grown — take
    // the write-watch baseline before the first advance/save.
    // This runs on the battle/game thread — designate it the simulation
    // thread before the baseline is taken, so only its allocations enter
    // cpp_arena (the audio thread is kept out of the snapshot).
    cpp_arena::set_sim_thread(GetCurrentThreadId());
    snapshot_ring::arm();
    apply_test_round_frames();

    log_printf("gekko_bridge: session up. local=%u port=%u remote=%s (remote_addr_len=%u)\n",
               local_player_idx, local_port, remote_addr,
               (unsigned)strlen(remote_addr));
    return true;
}

bool init_solo() {
    if (g_session) return false;
    register_cpp_ser();
    fake_input_init();

    g_solo = true;
    gekko_create(&g_session, GekkoStressSession);

    GekkoConfig config = {};
    config.desync_detection = true;
    config.input_size = sizeof(uint16_t);
    config.state_size = 1 * 1024 * 1024;   // see init() — snapshot_ring owns the state
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
    live_actors::set_defer_release(!g_arena_rollback);
    cpp_arena::set_armed(true);          // capture operator-new for the whole match
    battle_pools::pregrow();             // freeze the C++ battle pools' block set
    tf4_pool::pregrow_objpools();        // freeze the generic-grow objpool family (f=34 hang)
    battle_pools::reserve_anim_vectors(); // re-home AnimCtrl CompositeSprite vectors into cpp_arena
    // Fix each player's input-history vector capacity so it never reallocs
    // mid-match (its backing buffer then keeps a stable address to snapshot).
    input_hist::pregrow();
    // Arm dirty-page snapshotting: arenas installed, pools pre-grown — take
    // the write-watch baseline before the first advance/save.
    // Designate this (the battle/game thread) the simulation thread before
    // the baseline — see init().
    cpp_arena::set_sim_thread(GetCurrentThreadId());
    snapshot_ring::arm();

    // The battle is already created — vs.Initialize ran under the
    // vanilla loop during the intro. A stress session has no handshake
    // and emits no GekkoSessionStarted, so we are started immediately:
    // gekko owns the frame loop from here, frame 0 = this Round_Fight
    // frame.
    apply_test_round_frames();
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

// Deferred-arm parameters. g_watch_dual selects init() vs init_solo();
// the *_dual fields carry init()'s args captured at watch time.
static bool     g_watch_dual        = false;
static uint16_t g_watch_local_port  = 0;
static uint16_t g_watch_remote_port = 0;
static uint8_t  g_watch_local_idx   = 0;
static char     g_watch_remote_ip[64] = {0};

// TEST hook: if SQUIROLL_ROUND_FRAMES=N is set, overwrite battle.time
// with N at session-arm — once, before the first save, so it is
// deterministic and re-sims reproduce it. Shortens round 1 so a harness
// run reaches the round transition (time-over -> round 2) quickly.
// Both dual peers read the same env -> same value -> still in sync.
static void apply_test_round_frames() {
    static int rf = -1;
    if (rf < 0) {
        char buf[16] = {0};
        DWORD n = GetEnvironmentVariableA("SQUIROLL_ROUND_FRAMES", buf, sizeof(buf));
        rf = (n > 0 && n < sizeof(buf)) ? atoi(buf) : 0;
        if (rf < 0) rf = 0;
    }
    if (rf <= 0 || !v) return;
    SQInteger top = sq_gettop(v);
    sq_pushroottable(v);
    sq_pushstring(v, _SC("battle"), -1);
    if (SQ_SUCCEEDED(sq_get(v, -2))) {
        sq_pushstring(v, _SC("time"), -1);
        sq_pushinteger(v, rf);
        if (SQ_SUCCEEDED(sq_set(v, -3))) {
            log_printf("[gekko_bridge] TEST: round timer shortened to %d frames\n", rf);
        }
    }
    sq_settop(v, top);
}

void watch_for_fight_solo() {
    g_watch_for_fight = true;
    g_watch_dual = false;
    log_printf("[gekko_bridge] watching for Round_Fight to arm solo session\n");
}

void watch_for_fight_dual(uint16_t local_port, uint16_t remote_port,
                          uint8_t local_idx, const char* remote_ip)
{
    g_watch_for_fight   = true;
    g_watch_dual        = true;
    g_watch_local_port  = local_port;
    g_watch_remote_port = remote_port;
    g_watch_local_idx   = local_idx;
    snprintf(g_watch_remote_ip, sizeof(g_watch_remote_ip), "%s",
             remote_ip ? remote_ip : "127.0.0.1");
    log_printf("[gekko_bridge] watching for Round_Fight to arm dual session "
               "(local=%u remote=%u idx=%u ip=%s)\n",
               local_port, remote_port, (unsigned)local_idx, g_watch_remote_ip);
}

// Called every vanilla-loop frame (from better_game_loop) before any
// session exists. Once battle.state reaches Round_Fight (8), the intro
// is over — create the gekko session so gekko frame 0 is fight frame 0.
// Solo arms a GekkoStressSession (started immediately); dual arms a
// GekkoGameSession (then better_game_loop holds the frame loop until
// the handshake fires GekkoSessionStarted).
void pre_arm_poll() {
    if (!g_watch_for_fight || g_session) return;
    int st = 0;
    if (read_battle_state(&st) && st == 8 /* Round_Fight */) {
        g_watch_for_fight = false;
        if (g_watch_dual) {
            log_printf("[gekko_bridge] Round_Fight reached -> arming dual session\n");
            init(g_watch_local_port, g_watch_remote_port,
                 g_watch_local_idx, g_watch_remote_ip);
        } else {
            log_printf("[gekko_bridge] Round_Fight reached -> arming solo session\n");
            init_solo();
        }
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
    cpp_arena::set_armed(false);
}

bool is_active()         { return g_active; }
bool is_session_started(){ return g_session_started; }

// Round-end disarm. The interactive fight is exactly battle.state == 8
// (the engine's own damage code gates on `state != 8`); the round-start
// intro, KO / time-up demos, win poses and round transitions (states
// 2/4/64/32/128) are non-interactive and must not be rolled back. When
// the confirmed state leaves 8 the session is torn down; g_watch_for_fight
// is re-enabled so pre_arm_poll re-arms at the next round's Round_Fight.
// A best-of-3 match therefore arms/disarms 1-3 times; if the match has
// ended, state never returns to 8 and the re-armed watch simply idles.
static void disarm_for_round_end() {
    log_printf("[gekko_bridge] round ended (battle.state left 8) -> disarm\n");
    shutdown();                  // preserves g_watch_dual + dual params
    g_watch_for_fight = true;    // re-arm at the next Round_Fight
}

// Solo fast-forward. While a solo stress session owns the frame loop and
// the backtick (`) key is held, run extra tick()s per rendered frame so a
// run can reach time-over (~8910 logical frames) in seconds. Netplay is
// network-paced — turbo is gated to g_solo so it can't desync a match.
int turbo_ticks() {
    // Turbo only applies to the solo stress session — dual would need
    // both peers fast-forwarding in lockstep.
    if (!g_solo) return 1;
    // Env override: SQUIROLL_TURBO=N runs N logical frames per real
    // frame with no key held, so the harness can fast-forward to
    // time-over for short cross-round tests. Read once, cached.
    static int env_turbo = -1;
    if (env_turbo < 0) {
        char buf[16] = {0};
        DWORD n = GetEnvironmentVariableA("SQUIROLL_TURBO", buf, sizeof(buf));
        env_turbo = (n > 0 && n < sizeof(buf)) ? atoi(buf) : 1;
        if (env_turbo < 1)  env_turbo = 1;
        if (env_turbo > 64) env_turbo = 64;
    }
    // Backtick (VK_OEM_3) held = manual boost, at least 8x.
    if (GetAsyncKeyState(VK_OEM_3) & 0x8000) {
        return env_turbo > 8 ? env_turbo : 8;
    }
    return env_turbo;
}

// ------------------------------------------------------------------- tick --

bool tick() {
    if (!g_active) return false;

    // DIAGNOSTIC: confirm the Squirrel VM heap is living in the arena.
    // Logged every 600 frames while a session runs — `used` is the arena
    // high-water, `live` is currently-handed-out bytes.
    if (g_session_started) {
        static uint32_t arena_log = 0;
        if ((arena_log++ % 600) == 0) {
            log_printf("[sq_arena] used=%u KB live=%u KB cap=%u MB\n",
                       sq_arena::used() / 1024,
                       (uint32_t)(sq_arena::live_bytes() / 1024),
                       sq_arena::capacity() / (1024 * 1024));
            log_printf("[cpp_arena] used=%u KB live=%u KB cap=%u MB\n",
                       cpp_arena::used() / 1024,
                       (uint32_t)(cpp_arena::live_bytes() / 1024),
                       cpp_arena::capacity() / (1024 * 1024));
        }
        // DIAGNOSTIC: is the remote peer's traffic actually arriving?
        // kb_received ~0 on a peer => its socket gets no packets from the
        // other side (one-directional delivery). Logged every 30 ticks.
        if (!g_solo && (arena_log % 3) == 1) {
            GekkoNetworkStats ns = {};
            uint8_t remote = (uint8_t)(1 - g_local_idx);
            gekko_network_stats(g_session, remote, &ns);
            log_printf("[netstat] tick=%u remote=%u ping=%ums sent=%.2f "
                       "recv=%.2f KB/s\n", arena_log, remote, ns.last_ping,
                       ns.kb_sent, ns.kb_received);
        }
    }

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
                // vs.Initialize already ran under the vanilla loop and
                // the intro played out before the session was created
                // at Round_Fight — so there is nothing deferred to run
                // here. The handshake is done: gekko now owns the frame
                // loop, frame 0 = the held Round_Fight frame.
                g_session_started = true;
                log_printf("[gekko_bridge] SessionStarted -> gekko owns frame loop\n");
                break;
            case GekkoDesyncDetected: {
                // DESYNC is loud — Gekko fires it for every frame the
                // checksums disagree. Throttle to one line per 300 to
                // keep the log readable while still showing the issue.
                static uint32_t desync_counter = 0;
                bool first = (desync_counter == 0);
                if (first || (desync_counter % 300) == 0) {
                    log_printf("[gekko_bridge] !! DESYNC frame=%d local=0x%08x remote=0x%08x peer_handle=%d (desyncs_so_far=%u)\n",
                               e->data.desynced.frame,
                               e->data.desynced.local_checksum,
                               e->data.desynced.remote_checksum,
                               e->data.desynced.remote_handle,
                               desync_counter + 1);
                }
                ++desync_counter;
                // SQUIROLL_DESYNC_ABORT=1 -> stop dead on the FIRST desync, so a
                // long run's log ends exactly at the diverging frame. Lets us push
                // for 100% determinism: run long with random input seeds, and any
                // run that aborts marks a frame+seed to trace.
                if (first) {
                    static int abort_on = -1;
                    if (abort_on < 0) {
                        char b[8] = {0};
                        abort_on = (GetEnvironmentVariableA("SQUIROLL_DESYNC_ABORT", b, sizeof b) > 0
                                    && b[0] != '0') ? 1 : 0;
                    }
                    if (abort_on) {
                        // Terminate + dump on the FIRST desync: the log then ends exactly
                        // at the diverging frame with every per-frame diff diagnostic
                        // ([comp]/[divf]/[engdiff]/[sblob]) for it already emitted.
                        // Exit code 5 = "desync" so the harness can classify (0=clean).
                        log_printf("[gekko_bridge] DESYNC_ABORT: frame=%d local=%08x remote=%08x "
                                   "— dumping + exiting(5) for trace\n",
                                   e->data.desynced.frame,
                                   e->data.desynced.local_checksum,
                                   e->data.desynced.remote_checksum);
                        log_state_fingerprint(g_trace_frame, g_trace_rb, "desync-abort");
                        battle_pools::log_fingerprint("desync-abort");
                        log_flush();
                        Sleep(500);
                        ExitProcess(5);
                    }
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
        if (g_fake_input) {
            // Test harness: generated inputs. Each player draws from its
            // own stream — for solo we add both; for dual each peer adds
            // only its own (the other arrives over the network), and the
            // per-player seeding keeps the streams identical cross-peer.
            uint16_t mine = fake_input_gen(g_local_idx);
            gekko_add_local_input(g_session, g_local_idx, &mine);
            if (g_solo) {
                uint16_t other = fake_input_gen(1);
                gekko_add_local_input(g_session, 1, &other);
            }
        } else {
            uint16_t my_input = read_local_input_bits();
            gekko_add_local_input(g_session, g_local_idx, &my_input);
            // Solo stress session: both players are local, so drive the
            // second one too.
            if (g_solo) {
                gekko_add_local_input(g_session, 1, &my_input);
            }
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
                LARGE_INTEGER _ts0; QueryPerformanceCounter(&_ts0);
                uint32_t n = save_state_to_buf(
                    e->data.save.state,
                    /*cap=*/ 1 * 1024 * 1024,   // must match GekkoConfig::state_size
                    &cs,
                    (uint32_t)e->data.save.frame
                );
                LARGE_INTEGER _ts1; QueryPerformanceCounter(&_ts1);
                g_perf_save += (uint64_t)(_ts1.QuadPart - _ts0.QuadPart);
                ++g_perf_nsave;
                *e->data.save.state_len = n;
                *e->data.save.checksum = cs;
                // Determinism probe: the same gekko frame is saved once
                // forward and again on each rollback re-sim. cs is the
                // value-based (address-independent) checksum — if a frame
                // logs two different cs values, its re-sim diverged from
                // the forward sim (the rollback restore is incomplete /
                // the sim is non-deterministic).
                {
                    static int save_trace = 240;
                    if (save_trace > 0) {
                        --save_trace;
                        // blobcs = checksum of the WHOLE blob as handed to
                        // gekko. The matching [load] logs the same for the
                        // blob gekko hands back — if they differ for a
                        // frame, gekko's state buffer was clobbered.
                        uint32_t blobcs =
                            fletcher32((const uint8_t*)e->data.save.state, n);
                        log_printf("[save] f=%d cs=0x%08x len=%u blobcs=0x%08x\n",
                                   (int)e->data.save.frame, cs, n, blobcs);
                    }
                }
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
                    static const int DUMP_LO = 2, DUMP_HI = 9;
                    int fr = e->data.save.frame;
                    if (fr >= DUMP_LO && fr <= DUMP_HI) {
                        static int dump_cnt[DUMP_HI - DUMP_LO + 1] = {0};
                        int* dc = &dump_cnt[fr - DUMP_LO];
                        if (*dc < 10) {
                            char path[128];
                            snprintf(path, sizeof(path),
                                     "C:\\dev\\aocf\\th155\\sq_blob_p%u_f%d_s%d.bin",
                                     (unsigned)g_local_idx, fr, *dc);
                            FILE* f = fopen(path, "wb");
                            if (f) {
                                fwrite(e->data.save.state, 1, n, f);
                                fclose(f);
                                log_printf("[gekko_bridge] Save frame=%d #%d "
                                           "len=%u cs=0x%08x\n", fr, *dc, n, cs);
                            }
                            ++*dc;
                        }
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
                {
                    static int load_trace = 120;
                    if (load_trace > 0) {
                        --load_trace;
                        uint32_t blobcs = fletcher32(
                            (const uint8_t*)e->data.load.state,
                            e->data.load.state_len);
                        log_printf("[load] f=%d len=%u blobcs=0x%08x\n",
                                   (int)e->data.load.frame,
                                   e->data.load.state_len, blobcs);
                    }
                }
                g_last_load_frame = (int)e->data.load.frame;  // rollback target
                {
                    LARGE_INTEGER _tl0; QueryPerformanceCounter(&_tl0);
                    load_state_from_buf(e->data.load.state,
                                        e->data.load.state_len);
                    LARGE_INTEGER _tl1; QueryPerformanceCounter(&_tl1);
                    g_perf_load += (uint64_t)(_tl1.QuadPart - _tl0.QuadPart);
                    ++g_perf_nload;
                }
                break;
            }
            case GekkoAdvanceEvent: {
                const uint16_t* inputs = (const uint16_t*)e->data.adv.inputs;
                forced_inputs[0] = inputs[0];
                forced_inputs[1] = inputs[1];
                forced_inputs_active = true;
                static int adv_trace = 60;
                bool at = adv_trace > 0;
                if (at) {
                    --adv_trace;
                    log_printf("[adv] >>> frame=%d rb=%d p0=0x%04x p1=0x%04x\n",
                               e->data.adv.frame, (int)e->data.adv.rolling_back,
                               inputs[0], inputs[1]);
                }
                g_trace_frame = (int)e->data.adv.frame;
                g_trace_rb    = (int)e->data.adv.rolling_back;
                g_trace_depth = (g_trace_rb && g_last_load_frame >= 0)
                                  ? ((int)e->data.adv.frame - g_last_load_frame) : 0;
                log_state_fingerprint((int)e->data.adv.frame,
                                      (int)e->data.adv.rolling_back, "adv-top");
                if (rb_diag_enabled()) {
                    battle_pools::log_fingerprint("adv-top");
                    // NB: adv-top diff_locate disabled. Post-advance
                    // diff_locate (below, after the advance completes)
                    // shares the same slot[frame%RING] storage, and a
                    // pre-advance call's slot value is overwritten by the
                    // post-advance call for the same frame — so a re-sim
                    // adv-top compare would diff PRE-advance current
                    // against POST-advance saved, producing a false
                    // positive on every frame. Keep just the post-advance
                    // diff — that's the one that catches \"advance()
                    // produced different output\" anyway.
                    //
                    // battle_pools::diff_locate(
                    //     (int)e->data.adv.frame,
                    //     (int)e->data.adv.rolling_back);
                }
                LARGE_INTEGER _ta0; QueryPerformanceCounter(&_ta0);
                advance_one_frame();
                // FINDING: the forward runs the particle draws (Ew_tEftParticle
                // vtable[19] @0x10db30) but the headless re-sim never draws (verified:
                // [draweff] is rb=0 only). Those draws run SQVM scripts whose
                // sim-referenced boost::signals2 connections then diverge. Replaying via
                // render_one_frame() here is WRONG — the effect draws are NOT in
                // render_one_frame (calling it made cpp 24->37, eng broke). The draws
                // are a SEPARATE sTask DRAW iteration (vtable[19]; StepLayerMember
                // dispatches vtable[8]=update, not the draw). Correct replay/exclusion
                // must target that draw iteration (and gate the GPU off).
                LARGE_INTEGER _ta1; QueryPerformanceCounter(&_ta1);
                g_perf_adv += (uint64_t)(_ta1.QuadPart - _ta0.QuadPart);
                if (++g_perf_nadv >= 240) {
                    LARGE_INTEGER _fr; QueryPerformanceFrequency(&_fr);
                    uint64_t hz = (uint64_t)_fr.QuadPart;
                    auto _us = [&](uint64_t t, uint32_t k) -> uint32_t {
                        return k ? (uint32_t)(t * 1000000ull / hz / k) : 0;
                    };
                    log_printf("[perf] per-call us: advance=%u  save=%u "
                               "(sblob=%u cap=%u)  load=%u  [nsave=%u nload=%u]\n",
                               _us(g_perf_adv, g_perf_nadv),
                               _us(g_perf_save, g_perf_nsave),
                               _us(g_perf_sblob, g_perf_nsave),
                               _us(g_perf_cap, g_perf_nsave),
                               _us(g_perf_load, g_perf_nload),
                               g_perf_nsave, g_perf_nload);
                    g_perf_save = g_perf_load = g_perf_adv = 0;
                    g_perf_sblob = g_perf_cap = 0;
                    g_perf_nsave = g_perf_nload = g_perf_nadv = 0;
                }
                if (at) log_printf("[adv] <<< frame=%d done\n", e->data.adv.frame);
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

    // Round-end check: once the fight is over (battle.state has left 8),
    // tear the session down so the non-interactive demo/transition runs
    // on the vanilla loop, un-rolled-back. Only after the session has
    // actually started, and only solo for now — dual must drive this off
    // the confirmed (non-speculative) frame so a predicted-then-rolled-
    // back KO can't disarm early (TODO when dual is re-tested).
    if (g_session_started && g_solo) {
        int st = 0;
        if (read_battle_state(&st) && st != 8) {
            disarm_for_round_end();
            return advanced;
        }
    }

    // Note: rendering is driven by better_game_loop's window_render
    // call AFTER tick(), not from here. That keeps the window updating
    // even on frames where Gekko didn't advance (e.g. while the sync
    // handshake is still in flight, or waiting on remote inputs) so
    // the screen doesn't freeze to black.
    return advanced;
}

} // namespace gekko_bridge
