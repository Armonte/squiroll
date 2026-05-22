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

namespace sq_trace {
namespace {

static SafetyHookInline g_h{};

static char thiscall hook_inst_get(uint8_t* inst, int* key, int* out) {
    uint8_t*  cls     = inst ? *(uint8_t**)(inst + 0x1C) : nullptr;
    uint32_t  members = cls  ? *(uint32_t*)(cls + 0x18)  : 1u;
    if (members == 0) {
        // Reproduce SQInstance__Get's exact deref chain — this is the
        // precise condition that faults two calls deeper in SQTable__Get.
        const char* ks = "<non-string-key>";
        if (key && (uint32_t)key[0] == 0x08000010u && key[1]) {
            ks = (const char*)((uintptr_t)key[1] + 0x1C);  // SQString chars
        }
        static int quota = 80;
        if (quota > 0) {
            --quota;
            log_printf("[sqtrace] SQInstance__Get: class=%p has NULL _members "
                       "(inst=%p) member='%s'\n", cls, inst, ks);
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
