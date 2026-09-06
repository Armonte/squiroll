// text_vb_heal — see text_vb_heal.h.
#include "text_vb_heal.h"

#include <safetyhook.hpp>
#include <windows.h>
#include <stdint.h>
#include <unordered_map>

#include "log.h"
#include "util.h"   // _R + thiscall/fastcall

namespace gekko_bridge { extern int g_trace_frame; extern int g_trace_rb; }
namespace cpp_arena { bool owns(const void* p); }

namespace text_vb_heal {

// Manbow::String layout (verified: update_renderable_mesh_and_transform 0x66580)
static constexpr size_t OFF_GLYPH_COUNT   = 200;  // vertex count (glyphs*4)
static constexpr size_t OFF_DIRTY         = 264;  // BYTE: re-upload vertices
static constexpr size_t OFF_STREAM_OBJ    = 288;  // shared_ptr.obj  (= node+12)
static constexpr size_t OFF_STREAM_CTRL   = 292;  // shared_ptr.ctrl (= node)
static constexpr size_t OFF_STREAM_CAP    = 296;  // capacity (vertex count)

struct Entry { uint8_t* owner; uint32_t cap; };
static std::unordered_map<void*, Entry>    g_nodes;   // ctrl node -> entry
static std::unordered_map<uint8_t*, void*> g_owner;   // String -> newest ctrl node
static thread_local uint8_t* g_cur_string = nullptr;  // String being rendered
static int g_log_quota = 64;
// The registry is touched from the game-loop thread (create/validate) AND from
// whichever thread releases a vertex-buffer node back to the pool (worker
// threads do) -> unsynchronized unordered_map mutation = NT heap corruption
// (0xC0000374 fast-fails, no hook sees them; Windows event log did). Lock it.
static CRITICAL_SECTION g_reg_lock;
struct RegLock { RegLock() { EnterCriticalSection(&g_reg_lock); } ~RegLock() { LeaveCriticalSection(&g_reg_lock); } };

static SafetyHookInline g_h_update{};
static SafetyHookInline g_h_store{};
static SafetyHookInline g_h_release{};

// update_renderable_mesh_and_transform(String* this, mat44* world, __int128* row)
static unsigned int thiscall update_hook(uint8_t* self, int world, void* row) {
    void* obj  = *(void**)(self + OFF_STREAM_OBJ);
    void* ctrl = *(void**)(self + OFF_STREAM_CTRL);
    if (obj || ctrl) {
        RegLock rl;
        auto it = g_nodes.find(ctrl);
        bool ok = it != g_nodes.end() && it->second.owner == self
               && obj == (uint8_t*)ctrl + 12;
        if (!ok) {
            // Stale (rewound) stream. Prefer re-adopting the newest node this
            // String itself allocated (its own future allocation, leaked by the
            // rewind); else drop and let the normal path reallocate.
            auto ow = g_owner.find(self);
            if (ow != g_owner.end() && g_nodes.count(ow->second)) {
                void* n = ow->second;
                *(void**)(self + OFF_STREAM_CTRL)   = n;
                *(void**)(self + OFF_STREAM_OBJ)    = (uint8_t*)n + 12;
                *(uint32_t*)(self + OFF_STREAM_CAP) = g_nodes[n].cap;
                if (g_log_quota > 0) { --g_log_quota;
                    log_printf("[vbheal] String %p: stale stream ctrl=%p -> re-adopted %p cap=%u\n",
                               self, ctrl, n, g_nodes[n].cap); }
            } else {
                *(void**)(self + OFF_STREAM_CTRL)   = nullptr;
                *(void**)(self + OFF_STREAM_OBJ)    = nullptr;
                *(uint32_t*)(self + OFF_STREAM_CAP) = 0;
                if (g_log_quota > 0) { --g_log_quota;
                    log_printf("[vbheal] String %p: stale stream ctrl=%p -> dropped, realloc\n",
                               self, ctrl); }
            }
            *(uint8_t*)(self + OFF_DIRTY) = 1;   // GPU contents are from another frame
        }
    }
    uint8_t* prev = g_cur_string;
    g_cur_string = self;
    unsigned int r = g_h_update.unsafe_thiscall<unsigned int>(self, world, row);
    g_cur_string = prev;
    return r;
}

// TF4::MeshVertex::StoreStreamWithStride(shared_ptr* out, int bytes, int stride, int flag)
// CONVENTION (verified from disasm 0x3A7B0 + caller 0x665ff): out=ecx, bytes=edx,
// stride/flag on the stack, and the function ends in a plain `retn` -> the
// CALLER cleans the 8 stack bytes (`add esp,8` at 0x66604). That is NOT
// __fastcall (callee-clean), so the hook needs a naked shim; a fastcall hook
// double-cleaned the stack and returned into heap garbage (0xC000001D).
// out[0]=obj, out[1]=ctrl node.
static void* g_store_orig = nullptr;   // safetyhook trampoline (original code)

static void** cdecl store_hook_c(void** out, int bytes, int stride, int flag);

// Call the original with its real convention (ecx, edx, 2 stack args, caller-clean).
static naked void** cdecl call_orig_store(void** out, int bytes, int stride, int flag) {
    __asm {
        push ebp
        mov  ebp, esp
        push dword ptr [ebp+20]     // flag
        push dword ptr [ebp+16]     // stride
        mov  edx, [ebp+12]          // bytes
        mov  ecx, [ebp+8]           // out
        call dword ptr [g_store_orig]
        add  esp, 8
        pop  ebp
        ret
    }
}

// Hook entry: receives the game's convention, forwards to the cdecl body.
static naked void store_hook_entry() {
    __asm {
        push ebp
        mov  ebp, esp
        push dword ptr [ebp+12]     // flag   ([ebp+4]=ret, [ebp+8]=stride)
        push dword ptr [ebp+8]      // stride
        push edx                    // bytes
        push ecx                    // out
        call store_hook_c
        add  esp, 16
        pop  ebp
        ret                         // caller cleans stride/flag
    }
}

static void** cdecl store_hook_c(void** out, int bytes, int stride, int flag) {
    void** r = call_orig_store(out, bytes, stride, flag);
    if (g_cur_string && out && out[1]) {
        uint32_t cap = *(uint32_t*)(g_cur_string + OFF_GLYPH_COUNT);
        RegLock rl;
        g_nodes[out[1]] = Entry{ g_cur_string, cap };
        g_owner[g_cur_string] = out[1];
    }
    return r;
}

// std::_Ref_count_obj_alloc<D3D11VertexBuffer,TPoolAllocator>::_Delete_this
// (0x3AC90, __thiscall node): destroys + pushes the node on the pool free-list.
static int thiscall release_hook(void* node) {
    RegLock rl;
    auto it = g_nodes.find(node);
    if (it != g_nodes.end()) {
        auto ow = g_owner.find(it->second.owner);
        if (ow != g_owner.end() && ow->second == node) g_owner.erase(ow);
        g_nodes.erase(it);
    }
    return g_h_release.unsafe_thiscall<int>(node);
}

// [heapdiag] Manbow::InputSingle scalar deleting destructor (0x6D9E0): the
// 0xC0000374 heap fast-fails happen inside the `operator delete(this)` at its
// tail. Log every call (rare) with the pointer, delete flag, arena membership,
// frame/re-sim and caller, so the fatal one is attributable (repeat pointer =
// double delete across a rollback; foreign pointer = not a heap block).
// DIAGNOSTIC ONLY (SQUIROLL_ISDTOR=1). Written to hunt the InputSingle-destructor
// death that turned out to be the partial tapped-vtable copy (fixed in 7d77cc3).
// It logs AND log_flush()es — a BLOCKING disk write — on every InputSingle
// destruction, so it must never be on in a normal or perf run.
static bool isdtor_log_enabled() {
    static int on = -1;
    if (on < 0) {
        char b[8] = {0};
        DWORD n = GetEnvironmentVariableA("SQUIROLL_ISDTOR", b, sizeof b);
        on = (n > 0 && b[0] == '1') ? 1 : 0;
    }
    return on != 0;
}
static SafetyHookInline g_h_isdtor{};
static void* thiscall inputsingle_dtor_hook(void* self, int flags) {
    if (!isdtor_log_enabled())
        return g_h_isdtor.unsafe_thiscall<void*>(self, flags);
    static void* seen[256]; static int nseen = 0;
    bool repeat = false;
    for (int i = 0; i < nseen; ++i) if (seen[i] == self) { repeat = true; break; }
    if (!repeat && nseen < 256) seen[nseen++] = self;
    uint32_t ret = (uint32_t)(uintptr_t)_ReturnAddress();
    log_printf("[isdtor] this=%p flags=%d arena=%d REPEAT=%d f=%d rb=%d ret=th155+%X\n",
               self, flags, (int)cpp_arena::owns(self), (int)repeat,
               gekko_bridge::g_trace_frame, gekko_bridge::g_trace_rb,
               ret - (uint32_t)base_address);
    log_flush();
    return g_h_isdtor.unsafe_thiscall<void*>(self, flags);
}

void install() {
    static bool done = false;
    if (done) return;
    done = true;
    if (isdtor_log_enabled()) {
        g_h_isdtor = safetyhook::create_inline((void*)(0x6D9E0_R), (void*)inputsingle_dtor_hook);
        log_printf("[heapdiag] InputSingle dtor hook %s (SQUIROLL_ISDTOR=1)\n",
                   g_h_isdtor ? "OK" : "FAIL");
    }
    InitializeCriticalSection(&g_reg_lock);
    g_h_update  = safetyhook::create_inline((void*)(0x66580_R), (void*)update_hook);
    g_h_store   = safetyhook::create_inline((void*)(0x3A7B0_R), (void*)store_hook_entry);
    g_store_orig = g_h_store ? (void*)g_h_store.trampoline().address() : nullptr;
    g_h_release = safetyhook::create_inline((void*)(0x3AC90_R), (void*)release_hook);
    log_printf("[vbheal] String vertex-stream heal hooks: update=%d store=%d release=%d\n",
               (int)(bool)g_h_update, (int)(bool)g_h_store, (int)(bool)g_h_release);
}

} // namespace text_vb_heal
