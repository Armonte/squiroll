// render_slot — squiroll-owned Manbow::DrawCommandSlot for plugin HUDs.
// See render_slot.h for the design rationale (task #30).
//
// No inline hooks here, so safetyhook is NOT included. windows.h comes first
// (for CRITICAL_SECTION / Enter-LeaveCriticalSection) BEFORE any squiroll header
// so the util.h calling-convention macros (cdecl/thiscall/...) don't collide
// with the Win32 headers.
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "render_slot.h"
#include "render_arena.h"
#include "log.h"     // pulls util.h + windows.h; provides log_printf
#include "util.h"    // _R address literal + cdecl/thiscall attribute macros

namespace render_slot {

// ---------------------------------------------------------------------------
// th155 entry points — RVAs verified against th155_fresh.exe.i64
// (_R adds the runtime module base; see util.h).
// ---------------------------------------------------------------------------

// Manbow::DrawCommandSlot::create_and_bind(void* out_sqobj, const char* name)
//   __cdecl @0x56A50. Allocates a 20B DrawCommandSlot { vtable@+0 (0x442900),
//   grouped-signal ptr@+8 }, builds its boost::signals2 grouped signal
//   (ctor 0x2FF80), registers it in th155's C++ global slot map, binds it into
//   Squirrel as ::graphics.slot.<name> AND ::<name>, and writes a Sqrat::Object
//   wrapper to out_sqobj. Returns out_sqobj. Verified sole caller:
//   init_system_related @0x55ED0 (which creates the built-in "ui"/etc. slots).
typedef int (cdecl* create_and_bind_t)(void* out_sqobj, const char* name);

// Sqrat::Object::getInstanceUserPtr(this) __thiscall @0x5A5B0. Pushes the
// Sqrat::Object, sq_getinstanceup, pops, returns the bound native C++ pointer
// (here: the Manbow::DrawCommandSlot*). Same primitive
// Manbow::String::ConnectRenderSlot @0x65680 uses to resolve the slot.
typedef void* (thiscall* get_instance_up_t)(void* sqrat_object);

// call boost::function<void()> stored at a connection @0x32400 __thiscall(this).
// Dispatches (*(int(__cdecl**)(void*))((*this & ~1)+4))(this+2). THROWS
// std::bad_function_call if the target is EMPTY (*this == 0) — so emit()
// pre-checks *func != 0 (th155 relies on signal_connlist_iterate having already
// filtered dead slots; we deliberately skip that pass, so we filter here).
typedef int (thiscall* call_boost_function_t)(void* boost_function);

// g_graphics_slot_table_vm @0x49AFF0 — the Squirrel VM backing ::graphics.slot.
// Non-null once init_system_related @0x55ED0 has run (script system up). Used as
// a cheap "is the VM ready" guard: create_and_bind dereferences this table + VM.
#define GRAPHICS_SLOT_VM (*(void**)(0x49AFF0_R))

// ---------------------------------------------------------------------------
// DrawCommandSlot / boost::signals2 layout constants (verified: ctors 0x2FF80,
// 0x301C0, 0x13400; invoker 0x31CC0; lock 0x304F0).
//   DrawCommandSlot: +8  -> grouped signal object (20B)
//   grouped signal : +8  -> std::list _Myhead SENTINEL node ptr
//                    +0xC -> CRITICAL_SECTION*
//   list node      : +0  -> next, +4 -> prev, +8 -> connbody
//   connbody       : +0xC (BYTE) m_connected  (1 = CONNECTED, 0 = DISCONNECTED)
//                    +0x14 -> slot_meta (cloned slot fn); +0x10 within = func_obj
//   func_obj (boost::function): *(func_obj) == 0  => empty (skip; would throw)
// ---------------------------------------------------------------------------
static constexpr size_t OFF_SLOT_SIGNAL      = 0x8;
static constexpr size_t OFF_SIG_SENTINEL     = 0x8;
static constexpr size_t OFF_SIG_CRITSECT     = 0xC;
static constexpr size_t OFF_NODE_NEXT        = 0x0;
static constexpr size_t OFF_NODE_CONNBODY    = 0x8;
static constexpr size_t OFF_CB_CONNECTED     = 0xC;   // BYTE: 1=connected, 0=disc.
static constexpr size_t OFF_CB_SLOTMETA      = 0x14;
static constexpr size_t OFF_META_FUNCTION    = 0x10;

// The Squirrel-visible slot name: ::graphics.slot.rollback_hud (and ::rollback_hud).
static const char* const SLOT_NAME = "rollback_hud";

// Sqrat::Object wrapper for our slot. Kept alive for the process lifetime (never
// destructed) so its addref'd HSQOBJECT pins the slot. 0x28 bytes is ample:
// create_and_bind writes vtable@+0, VM@+4, HSQOBJECT@+8..+15, bool@+16.
static unsigned char g_sqobj[0x28];
static void*         g_slot_native = nullptr;   // Manbow::DrawCommandSlot*
static bool          g_inited      = false;

bool  ready()       { return g_slot_native != nullptr; }
void* native_slot() { return g_slot_native; }

bool init() {
    if (g_inited)
        return g_slot_native != nullptr;

    // The th155 script system must be up (VM + ::graphics.slot table) or
    // create_and_bind dereferences a null VM. Retry on a later call.
    if (GRAPHICS_SLOT_VM == nullptr) {
        static bool warned = false;
        if (!warned) { warned = true;
            log_printf("[render_slot] init deferred: ::graphics.slot VM not up yet\n"); }
        return false;
    }

    render_arena::init();               // idempotent
    if (!render_arena::ready()) {
        log_printf("[render_slot] init failed: render_arena not ready\n");
        return false;
    }

    g_inited = true;                    // one-shot: never double-bind the name

    // Create the slot INSIDE a render_arena scope so the 20B slot, its 20B
    // grouped signal, the 24B connlist, the container and the CRITICAL_SECTION
    // all land in the non-snapshotted render_arena (create_and_bind's
    // operator_new_0 calls route there while in_scope()). Scope routing is
    // THREAD-LOCAL (render_arena hook_op_new), so this MUST run on the SIM
    // thread — the Squirrel VM owner. Any transient std::string / global-string
    // hash allocs inside create_and_bind also land here; that is fine (one-time,
    // render_arena is never rewound and never resets during a match).
    memset(g_sqobj, 0, sizeof(g_sqobj));
    {
        render_arena::Scope scope;
        ((create_and_bind_t)(0x56A50_R))(g_sqobj, SLOT_NAME);   // create_and_bind
    }

    // Resolve the native DrawCommandSlot* from the Sqrat::Object (pushes/pops the
    // VM stack once).
    g_slot_native = ((get_instance_up_t)(0x5A5B0_R))(g_sqobj);  // getInstanceUserPtr

    void* signal = g_slot_native
                 ? *(void**)((uint8_t*)g_slot_native + OFF_SLOT_SIGNAL) : nullptr;
    log_printf("[render_slot] ::graphics.slot.%s created native=%p signal=%p in_render_arena=%d\n",
               SLOT_NAME, g_slot_native, signal,
               g_slot_native ? (int)render_arena::owns(g_slot_native) : -1);
    return g_slot_native != nullptr;
}

void emit() {
    void* slot = g_slot_native;
    if (!slot)
        return;

    // slot+8 -> the 20B boost::signals2 grouped signal.
    uint8_t* signal = *(uint8_t**)((uint8_t*)slot + OFF_SLOT_SIGNAL);
    if (!signal)
        return;

    // signal+8 -> the std::list _Myhead SENTINEL node. Empty list => sentinel
    // points to itself, so the loop below no-ops.
    uint8_t* sentinel = *(uint8_t**)(signal + OFF_SIG_SENTINEL);
    if (!sentinel)
        return;

    // signal+0xC -> the signal's CRITICAL_SECTION. Hold it across the walk so a
    // concurrent sim-thread Manbow::String::ConnectRenderSlot can't relink the
    // list mid-iteration. (In steady state the plugin only mutates Text content,
    // not the connection list, so this rarely contends.)
    CRITICAL_SECTION* cs = *(CRITICAL_SECTION**)(signal + OFF_SIG_CRITSECT);
    if (cs) EnterCriticalSection(cs);

    // Forward-only walk: mirrors boost_signals2_invoke_one_slot @0x31CC0's
    // per-node invoke (func = *(*(node+8)+0x14)+0x10) with NO round-robin cursor
    // and NO node-erase (those are the rollback divergence/hang root in
    // Act::ScriptAPI::RunOneFrame @0x2FAD0).
    uint8_t* node = *(uint8_t**)(sentinel + OFF_NODE_NEXT);   // sentinel->next
    unsigned fuse = 0;
    while (node && node != sentinel) {
        uint8_t* connbody = *(uint8_t**)(node + OFF_NODE_CONNBODY);
        // INVOKE iff CONNECTED. connbody+0xC == 1 => connected (ctor 0x13400);
        // == 0 => disconnected/torn-down (lingers because we never erase). NOTE:
        // this polarity is the REVERSE of a naive "skip if disconnect flag set"
        // reading — verified against the connection_body ctor.
        if (connbody && *(uint8_t*)(connbody + OFF_CB_CONNECTED) != 0) {
            uint8_t* slot_meta = *(uint8_t**)(connbody + OFF_CB_SLOTMETA);
            if (slot_meta) {
                void* func_obj = slot_meta + OFF_META_FUNCTION;
                // Skip empty boost::function (would throw std::bad_function_call).
                if (*(void**)func_obj)
                    ((call_boost_function_t)(0x32400_R))(func_obj);  // call_boost__function_3
            }
        }
        node = *(uint8_t**)(node + OFF_NODE_NEXT);            // node->next
        if (++fuse > 4096) {   // corruption fuse; never reached in practice
            log_printf("[render_slot] emit: walk fuse tripped (list corrupt?)\n");
            break;
        }
    }

    if (cs) LeaveCriticalSection(cs);
}

} // namespace render_slot
