// safetyhook MUST be included before any squiroll header (calling-convention
// macro collision — same constraint as sq_arena.cpp).
#include <safetyhook.hpp>

#include <windows.h>
#include <stdint.h>
#include <intrin.h>

#include "sq_trace.h"
#include "patch_utils.h"
#include "util.h"
#include "log.h"

// SQVM__Get (th155 0x1918E0): the generic member-get dispatcher. a2 points
// at the operand SQObjectPtr {type@0, value@4}. For OT_INSTANCE it calls
// SQInstance__Get(value, key), which does
// SQTable__Get(inst->_class[+0x1C]->_members[+0x18], key). The dual-rollback
// crash is an operand tagged OT_INSTANCE whose value is a freed / recycled
// non-instance — so _class or _members dereferences to garbage and
// SQTable__Get faults reading [_members+0x24].
//
// This hook walks that chain defensively; on a broken link it reports
// WHERE the corrupt operand lives (its address + memory region), the VM,
// and the SQVM__Get return address — telling us which region the rollback
// snapshot failed to capture — then returns member-not-found so the run
// continues and every occurrence is logged.
#define SQVM_GET (0x1918E0_R)
#define SQVM_PTR (0x4DB020_R)

namespace gekko_bridge { extern int g_trace_frame; extern int g_trace_rb; }

namespace sq_trace {
namespace {

static SafetyHookInline g_h{};

static bool readable(uint32_t p) {
    return p >= 0x10000u && !IsBadReadPtr((const void*)(uintptr_t)p, 4);
}

static const char* region_of(uint32_t addr) {
    uint8_t* vm = *(uint8_t**)SQVM_PTR;
    if (vm) {
        uint32_t sv = *(uint32_t*)(vm + 0x18);          // _stack._vals
        uint32_t sa = *(uint32_t*)(vm + 0x20) * 8u;     // _stack._allocated*8
        if (sv && addr >= sv && addr < sv + sa) return "operand-stack";
        uint32_t cv = *(uint32_t*)(vm + 0x6C);          // _callstackdata._vals
        uint32_t ca = *(uint32_t*)(vm + 0x74) * 0x2Cu;
        if (cv && addr >= cv && addr < cv + ca) return "callstack-buf";
        if (addr >= (uint32_t)vm && addr < (uint32_t)vm + 0xA8) return "vm-struct";
    }
    if (addr >= 0x037C0000u && addr < 0x037C0000u + 64u * 1024 * 1024)
        return "sq-arena";
    return "OTHER(uncaptured?)";
}

static char thiscall hook_get(uint8_t* self_vm, int* obj, int* key, int* out,
                              char a5, int a6) {
    void* ret = _ReturnAddress();
    if (obj && (uint32_t)obj[0] == 0x0A008000u) {       // OT_INSTANCE
        uint32_t inst = (uint32_t)obj[1];
        const char* why = nullptr;
        uint32_t cls = 0, mem = 0;
        if (!readable(inst)) {
            why = "inst-unreadable";
        } else {
            cls = *(uint32_t*)(inst + 0x1C);            // SQInstance::_class
            if (!readable(cls)) {
                why = "class-bad";
            } else {
                mem = *(uint32_t*)(cls + 0x18);         // SQClass::_members
                if (!readable(mem)) why = "members-bad";
            }
        }
        if (why) {
            const char* ks = "<non-string-key>";
            if (key && (uint32_t)key[0] == 0x08000010u && key[1])
                ks = (const char*)((uintptr_t)key[1] + 0x1C);
            static int quota = 12;
            if (quota > 0) {
                --quota;
                const uint32_t* w = readable(inst) ? (const uint32_t*)inst
                                                   : nullptr;
                log_printf("[sqtrace] CORRUPT get  frame=%d rb=%d  why=%s\n"
                           "          member='%s'\n"
                           "          operand@%p  region=%s\n"
                           "          vm=%p  caller_ret=%p\n"
                           "          inst=%08x region=%s  _class=%08x  _members=%08x\n"
                           "          inst[0..7]=%08x %08x %08x %08x %08x %08x %08x %08x\n",
                           gekko_bridge::g_trace_frame, gekko_bridge::g_trace_rb,
                           why, ks,
                           (void*)obj, region_of((uint32_t)(uintptr_t)obj),
                           self_vm, ret,
                           inst, region_of(inst), cls, mem,
                           w ? w[0] : 0, w ? w[1] : 0, w ? w[2] : 0, w ? w[3] : 0,
                           w ? w[4] : 0, w ? w[5] : 0, w ? w[6] : 0, w ? w[7] : 0);
            }
            return 0;  // member-not-found — skip the faulting dispatch
        }
    }
    return g_h.unsafe_thiscall<char>(self_vm, obj, key, out, a5, a6);
}

} // namespace

void install() {
    g_h = safetyhook::create_inline((void*)SQVM_GET, (void*)hook_get);
    log_printf("[sqtrace] SQVM__Get hook: %d\n", g_h.enabled());
}

} // namespace sq_trace
