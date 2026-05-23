#include <windows.h>
#include <stdint.h>

#include "tf4_pool.h"
#include "cpp_arena.h"
#include "patch_utils.h"   // _R address literal, hotpatch_rel32
#include "log.h"

namespace tf4_pool {
namespace {

// SQVM__Call_0 (th155 0x45D20) grows the Squirrel-instance object pool. At
// 0x45D99 it does `call TF4__MeshVertex__PoolAlloc` (E8 rel32) with the
// mspace in ecx and the slab size in edx (__fastcall). Redirecting that one
// call site moves every pool slab into cpp_arena without disturbing the
// mspace. hotpatch_rel32 takes the address of the rel32 OPERAND — one byte
// past the E8 opcode at 0x45D99.
//
// HISTORICAL NOTE: an inline-hook on TF4__MeshVertex__PoolAlloc's entry
// (catching every caller through one gate) was tested — it caught
// Manbow::NetworkNode::BeginStreaming's 342KB streaming buffer too, whose
// receive content is inherently non-deterministic, and produced an
// EARLIER cpp_arena divergence (f=10). The right discriminator isn't
// "sim thread" but "battle pool vs network/audio/etc" — narrow at the
// known battle call site for now; a per-caller exclusion list at the
// entry would be the next step if more battle pool grow sites surface.
#define POOL_ALLOC_REL32  (0x45D9A_R)

// The original TF4 mspace allocator we replaced. Used as the fallback for
// non-simulation-thread grows: the SAME generic tf4_objpool_grow services
// audio (Ogg memory-stream reader) pools too — sending those slabs into
// cpp_arena leaks the audio thread's non-deterministic position updates
// into the rollback snapshot (proven by the DR0 multi-thread watchpoint).
typedef void* (__fastcall *tf4_mspace_pool_alloc_t)(void* mspace, uint32_t size);
#define TF4_MESHVERTEX_POOLALLOC \
    ((tf4_mspace_pool_alloc_t)(uintptr_t)(0x356A0_R))

// __fastcall: arg1 in ecx (mspace), arg2 in edx (slab size in bytes).
// Simulation thread -> cpp_arena (slab is rollback-snapshotted, needed for
// battle pools). Any other thread -> fall through to the original mspace
// allocator so audio / loader / etc. slabs stay outside the snapshot.
static void* __fastcall pool_alloc_redirect(void* mspace, uint32_t size) {
    if (cpp_arena::is_sim_thread()) {
        // Pass our return address — the th155 pool-grow call site (inside
        // tf4_objpool_grow) — so the arena tags the slab with real th155
        // code rather than this shim (keeps cpp_arena::attribute meaningful).
        return cpp_arena::raw_alloc(size, (uint32_t)(uintptr_t)_ReturnAddress());
    }
    return TF4_MESHVERTEX_POOLALLOC(mspace, size);
}

} // namespace

void install() {
    hotpatch_rel32(POOL_ALLOC_REL32, pool_alloc_redirect);
    log_printf("[tf4_pool] Squirrel-instance pool slabs -> cpp_arena\n");
}

} // namespace tf4_pool
