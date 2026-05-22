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
#include "alloc_man.h"  // sq_heap — giuroll-style Squirrel VM heap snapshot
#include "sq_arena.h"   // Squirrel VM heap arena
#include "battle_pools.h" // C++ battle object pools
#include "engine_snap.h"  // scheduler fixed-region snapshot
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

    uint32_t seed = 0x9E3779B9u;
    char sb[16] = {0};
    DWORD sn = GetEnvironmentVariableA("SQUIROLL_INPUT_SEED", sb, sizeof(sb));
    if (sn > 0 && sn < sizeof(sb)) {
        int s = atoi(sb);
        if (s != 0) seed = (uint32_t)s;
    }
    // Two distinct, non-zero streams — one per player.
    g_fake_rng[0] = seed ^ 0xA5A5A5A5u;
    g_fake_rng[1] = seed ^ 0x5A5A5A5Au;
    g_fake_held[0] = g_fake_held[1] = 0;
    g_fake_hold[0] = g_fake_hold[1] = 0;
    log_printf("[gekko_bridge] FAKE INPUT enabled, seed=0x%08x\n", seed);
}

// Generate one player's packed input for this frame: a direction held for
// a random 8-39 frame stretch, plus a ~38%-per-frame press of a random
// attack button. That keeps both characters moving and attacking, so
// projectiles, hitboxes and actor churn are continuously on screen for
// the rollback to capture and restore.
static uint16_t fake_input_gen(int p) {
    if (--g_fake_hold[p] <= 0) {
        uint32_t r = fake_xs32(g_fake_rng[p]);
        static const uint16_t dirs[9] = {
            0x0, 0x1, 0x2, 0x4, 0x8, 0x1|0x4, 0x1|0x8, 0x2|0x4, 0x2|0x8
        };
        g_fake_held[p] = dirs[r % 9];
        g_fake_hold[p] = 8 + (int)((r >> 8) % 32);
    }
    uint16_t in = g_fake_held[p];
    uint32_t r = fake_xs32(g_fake_rng[p]);
    if ((r & 0xFF) < 96) {                       // ~38% of frames
        in |= (uint16_t)(0x10u << ((r >> 8) & 3));  // one of A/B/C/D
    }
    return in;
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
static constexpr uint32_t SAVE_VERSION = 4;          // v4: + anim controller snapshot

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

uint32_t save_state_to_buf(void* buf, uint32_t cap, uint32_t* out_checksum,
                           uint32_t frame) {
    if (cap < sizeof(SaveHeader)) return 0;

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
    // walker. Kept ONLY to compute the desync checksum — it encodes
    // value-by-value (no raw addresses) so it is identical cross-peer when
    // state matches. It is NOT used to restore state anymore (the raw
    // sq_heap snapshot below does that, losslessly).
    uint32_t actors_end = (uint32_t)(p - static_cast<uint8_t*>(buf));
    if (actors_end + 4 > cap) return actors_end;
    uint32_t* sq_len_field = (uint32_t*)p;
    p += 4;
    uint32_t sq_cap = (cap - actors_end - 4);
    uint8_t* text_blob = p;
    uint32_t sq_n = g_sq_save_enabled ? call_squirrel_save(p, sq_cap, frame) : 0;
    *sq_len_field = sq_n;
    p += sq_n;

    if (out_checksum) {
        // Checksum the value-based text blob — address-independent, so two
        // peers (or a speculative save vs a rollback re-sim) produce the
        // same checksum whenever the logical state matches.
        *out_checksum = sq_n > 0 ? fletcher32(text_blob, sq_n) : 0;
    }

    // Trailer 2: arena rollback snapshot — the real restorable state.
    // Three length-prefixed sections: sq_arena (Squirrel VM heap),
    // battle_pools (battle object pools) and engine_snap (the scheduler's
    // fixed-address objects, sentinels and counters). The text blob above
    // is kept only for the value-based desync checksum; this trailer is
    // what load() restores from when g_arena_rollback is on.
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
            *len_field = wrote;
            p += wrote;
            return true;
        };
        bool ok = put_section("sq_arena", &sq_arena::save)
               && put_section("pools",    &battle_pools::save)
               && put_section("engine",   &engine_snap::save);
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
            get_section("pools",    &battle_pools::load)) {
            get_section("engine", &engine_snap::load);
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
    if (trace) log_printf("[gekko_bridge] advance: -> update_related\n");
    update_related(*MAIN_SCRIPTAPI_PTR);                    // RunOneFrame(g_main), once
    if (trace) log_printf("[gekko_bridge] advance: -> ScriptAPI::Update\n");
    Act_ScriptAPI_ptr->vftable->Update(Act_ScriptAPI_ptr);  // Act::ScriptAPI::Update
    ++*(uint32_t*)(0x4DACE0_R);                             // g_frame_counter
    if (trace) log_printf("[gekko_bridge] advance: exit\n");
}

void render_one_frame() {
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
    // 16 MB: the sq_heap snapshot is the full live Squirrel VM heap (every
    // tracked allocation), plus the text checksum blob + actor records.
    // Gekko keeps ~10-20 saves in flight. Logged per save ([sq_heap] save)
    // so the real size can be measured and this dialed in.
    config.state_size = 16 * 1024 * 1024;
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
    // Defer Squirrel VM frees too, so every object keeps a stable address
    // for the raw heap snapshot (giuroll model).
    sq_heap::set_armed(true);
    // Pre-grow the C++ battle object pools so their block set is frozen
    // for the match — the rollback snapshot copies those blocks raw.
    battle_pools::pregrow();
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
    config.state_size = 16 * 1024 * 1024;  // full Squirrel VM heap snapshot
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
    sq_heap::set_armed(true);  // defer Squirrel frees for the heap snapshot
    battle_pools::pregrow();   // freeze the C++ battle pools' block set

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
    // Stop deferring Squirrel frees and hard-free every block we held back.
    sq_heap::set_armed(false);
    sq_heap::flush();
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
                uint32_t n = save_state_to_buf(
                    e->data.save.state,
                    /*cap=*/ 16 * 1024 * 1024,  // must match GekkoConfig::state_size
                    &cs,
                    (uint32_t)e->data.save.frame
                );
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
                        log_printf("[save] f=%d cs=0x%08x len=%u\n",
                                   (int)e->data.save.frame, cs, n);
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
                    static const int DUMP_LO = 230, DUMP_HI = 235;
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
                        log_printf("[load] f=%d len=%u\n",
                                   (int)e->data.load.frame, e->data.load.state_len);
                    }
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
                static int adv_trace = 60;
                bool at = adv_trace > 0;
                if (at) {
                    --adv_trace;
                    log_printf("[adv] >>> frame=%d rb=%d p0=0x%04x p1=0x%04x\n",
                               e->data.adv.frame, (int)e->data.adv.rolling_back,
                               inputs[0], inputs[1]);
                }
                advance_one_frame();
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
