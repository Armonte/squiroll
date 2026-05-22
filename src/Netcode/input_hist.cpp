// safetyhook MUST be included before any squiroll header — util.h #defines
// the calling-convention keywords as attribute macros and safetyhook uses
// those identifiers as method names.
#include <safetyhook.hpp>

#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "input_hist.h"
#include "patch_utils.h"   // _R address literal
#include "util.h"          // thiscall
#include "log.h"

namespace input_hist {
namespace {

// input_history_u16__append(this, a2) — appends one u16 to the player's
// input-history vector, growing it (2*count+1) when full.
#define INPUT_HIST_APPEND (0x169A20_R)
// vector_u16__resize(this=&vec, new_capacity) — grows the u16 vector storage.
#define VECTOR_U16_RESIZE (0x39860_R)

static constexpr int      MAX_OBJ     = 4;       // expect 2 (one per player)
static constexpr uint32_t OBJ_BYTES   = 0x80;    // count + vector header + lock
static constexpr uint32_t VEC_OFF     = 0x10;    // std::vector<u16> @ obj+0x10
// Fixed pre-grown capacity (u16 elements). A match never reaches this, so the
// vector never reallocates and its backing buffer keeps a stable address.
static constexpr uint32_t PREGROW_CAP = 65536;

static uint32_t g_obj[MAX_OBJ];
static int      g_nobj     = 0;
static bool     g_pregrown = false;

static SafetyHookInline g_hook{};

typedef int (thiscall* append_fn)(int* self, int* a2);
typedef int (thiscall* resize_fn)(void* vec, uint32_t cap);

// Discovery hook: record each distinct InputHistory object the first time it
// is appended to.
static int thiscall hook_append(int* self, int* a2) {
    if (self && g_nobj < MAX_OBJ) {
        uint32_t a = (uint32_t)(uintptr_t)self;
        bool seen = false;
        for (int i = 0; i < g_nobj; ++i)
            if (g_obj[i] == a) { seen = true; break; }
        if (!seen) {
            g_obj[g_nobj++] = a;
            log_printf("[input_hist] discovered object #%d @%08X\n", g_nobj, a);
        }
    }
    return g_hook.unsafe_thiscall<int>(self, a2);
}

} // namespace

void install() {
    g_hook = safetyhook::create_inline((void*)INPUT_HIST_APPEND,
                                       (void*)hook_append);
    log_printf("[input_hist] install: hook=%d\n", (int)g_hook.enabled());
}

void pregrow() {
    if (g_pregrown) return;
    g_pregrown = true;
    if (g_nobj == 0) {
        log_printf("[input_hist] !! pregrow: no objects discovered yet\n");
        return;
    }
    resize_fn resize = (resize_fn)VECTOR_U16_RESIZE;
    for (int i = 0; i < g_nobj; ++i) {
        uint8_t* obj = (uint8_t*)(uintptr_t)g_obj[i];
        uint32_t cap0 = (*(uint32_t*)(obj + VEC_OFF + 4) -
                         *(uint32_t*)(obj + VEC_OFF)) >> 1;
        if (cap0 < PREGROW_CAP)
            resize((void*)(obj + VEC_OFF), PREGROW_CAP);
        log_printf("[input_hist] pregrow obj#%d @%08X cap %u -> %u buf=%08X\n",
                   i, g_obj[i], cap0,
                   (*(uint32_t*)(obj + VEC_OFF + 4) -
                    *(uint32_t*)(obj + VEC_OFF)) >> 1,
                   *(uint32_t*)(obj + VEC_OFF));
    }
}

// Blob: [magic][nobj] then per object
//   [obj-addr][OBJ_BYTES of object][vec-begin addr][buf-len][buf-len bytes]
uint32_t save(uint8_t* out, uint32_t cap) {
    uint8_t* p   = out;
    uint8_t* end = out + cap;
    auto put = [&](const void* s, uint32_t n) -> bool {
        if (p + n > end) return false;
        memcpy(p, s, n);
        p += n;
        return true;
    };
    uint32_t magic = 0x54534948;            // 'HIST'
    uint32_t n     = (uint32_t)g_nobj;
    if (!put(&magic, 4) || !put(&n, 4)) return 0;
    for (int i = 0; i < g_nobj; ++i) {
        uint8_t* obj   = (uint8_t*)(uintptr_t)g_obj[i];
        uint32_t count = *(uint32_t*)obj;                       // obj+0x00
        uint32_t vbeg  = *(uint32_t*)(obj + VEC_OFF);           // obj+0x10
        uint32_t blen  = count * 2;                             // live bytes
        if (!put(&g_obj[i], 4) || !put(obj, OBJ_BYTES) ||
            !put(&vbeg, 4) || !put(&blen, 4))
            return 0;
        if (blen && vbeg && !put((const void*)(uintptr_t)vbeg, blen))
            return 0;
    }
    return (uint32_t)(p - out);
}

void load(const uint8_t* blob, uint32_t len) {
    const uint8_t* p = blob;
    const uint8_t* e = blob + len;
    auto get = [&](void* d, uint32_t n) -> bool {
        if (p + n > e) return false;
        memcpy(d, p, n);
        p += n;
        return true;
    };
    uint32_t magic = 0, n = 0;
    if (!get(&magic, 4) || !get(&n, 4) || magic != 0x54534948) return;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t objaddr = 0, vbeg = 0, blen = 0;
        uint8_t  objbytes[OBJ_BYTES];
        if (!get(&objaddr, 4) || !get(objbytes, OBJ_BYTES) ||
            !get(&vbeg, 4) || !get(&blen, 4))
            return;
        if (p + blen > e) return;
        if (objaddr) memcpy((void*)(uintptr_t)objaddr, objbytes, OBJ_BYTES);
        if (vbeg && blen) memcpy((void*)(uintptr_t)vbeg, p, blen);
        p += blen;
    }
}

} // namespace input_hist
