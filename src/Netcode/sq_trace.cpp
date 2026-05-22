// safetyhook MUST be included before any squiroll header (calling-convention
// macro collision — same constraint as sq_arena.cpp).
#include <safetyhook.hpp>

#include <windows.h>
#include <stdint.h>

#include "sq_trace.h"
#include "patch_utils.h"
#include "util.h"
#include "log.h"

// SQInstance__Get (th155 0x180AD0): SQTable__Get(inst->_class[+0x1C]->
// _members[+0x18], key). The dual rollback crash is _members == NULL.
#define SQ_INSTANCE_GET (0x180AD0_R)

// Published by gekko_bridge — the frame being advanced + rollback flag.
namespace gekko_bridge { extern int g_trace_frame; extern int g_trace_rb; }

namespace sq_trace {
namespace {

static SafetyHookInline g_h{};

static char thiscall hook_inst_get(uint8_t* inst, int* key, int* out) {
    uint8_t* cls = inst ? *(uint8_t**)(inst + 0x1C) : nullptr;
    // A real SQClass pointer is a normal heap address. NULL, or a small
    // value (a Squirrel type tag like 0x08000010 mis-stored as the
    // class), means inst is not a real instance — the crash condition.
    bool bad_class = ((uintptr_t)cls < 0x00100000u);
    uint32_t members = (!bad_class) ? *(uint32_t*)(cls + 0x18) : 0u;
    if (bad_class || members == 0) {
        const char* ks = "<non-string-key>";
        if (key && (uint32_t)key[0] == 0x08000010u && key[1]) {
            ks = (const char*)((uintptr_t)key[1] + 0x1C);  // SQString chars
        }
        static int quota = 16;
        if (quota > 0) {
            --quota;
            // Dump the bad object's first 8 dwords — a real SQInstance
            // begins with a vtable pointer; a spliced one will show
            // Squirrel type tags (0x0X0000XX) instead.
            const uint32_t* w = (const uint32_t*)inst;
            log_printf("[sqtrace] CORRUPT instance-get  frame=%d rb=%d  "
                       "member='%s'\n"
                       "          inst=%p  _class(+0x1C)=%p\n"
                       "          inst[0..7] = %08x %08x %08x %08x  "
                       "%08x %08x %08x %08x\n",
                       gekko_bridge::g_trace_frame, gekko_bridge::g_trace_rb,
                       ks, inst, cls,
                       w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        }
        return 0;  // member-not-found — skip the SQTable__Get(NULL) fault
    }
    return g_h.unsafe_thiscall<char>(inst, key, out);
}

} // namespace

void install() {
    g_h = safetyhook::create_inline((void*)SQ_INSTANCE_GET, (void*)hook_inst_get);
    log_printf("[sqtrace] SQInstance__Get hook: %d\n", g_h.enabled());
}

} // namespace sq_trace
