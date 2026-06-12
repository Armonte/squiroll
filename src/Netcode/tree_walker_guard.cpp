// safetyhook MUST be included before any squiroll header.
#include <safetyhook.hpp>

#include <windows.h>
#include <stdint.h>
#include <atomic>

#include "patch_utils.h"
#include "util.h"
#include "log.h"
#include "cpp_arena.h"     // is_resim()

#include "tree_walker_guard.h"

namespace tree_walker_guard {

// Three identically-shaped RB-tree walkers in boost::signals2's
// grouped_list machinery. Each has the same loop pattern:
//   loop_top: mov ecx, [eax+10h]       ; read node key — FAULTS if eax=0
//             ... compare ...
//             mov eax, [eax+8]          ; node = node->right — FAULTS
//             cmp byte [eax+0Dh], 0    ; is_nil? — FAULTS
//             jz loop_top              ; loop while not nil
//   epilogue: mov eax, esi (or similar) ; return parent
//             pop ... ; retn
//
// When the tree gets corrupted during a rollback re-sim (the
// boost::signals2 connection state lives in Win32 heap and isn't
// snapshotted), `eax` becomes NULL or a non-canonical pointer.
// Every read at [eax+...] faults; crash_handler's universal NULL-skip
// advances EIP past the faulting instruction but does NOT update eax,
// so the loop spins forever — visible in `aocf_crash.log` as a NULL-
// skip storm (we observed 352K hits in one re-sim).
//
// PREVIOUS APPROACH (SafetyHookInline replacement of the whole
// function) broke healthy boot-time inserts — the Texture/Effect CSV
// resource registration relies on `insert_at_pos` actually inserting
// new entries, and our blanket "return duplicate" caused the stage
// to fail to load.
//
// CURRENT APPROACH: SafetyHookMid at each loop-top. The original
// function still runs every iteration; we just check at the top
// whether `eax` (the current-node ptr) is canonical. If not, we
// redirect EIP straight to the function's epilogue, which still
// returns whatever the function's "parent" register (esi, edx) holds
// at that point — i.e. the lower_bound / upper_bound / insert-position
// computed up to the corruption point. Healthy trees never hit this
// branch.
//
// Loop top + epilogue addresses (verified at disasm):
//   lower_bound      loop_top=0x14B16  epilogue=0x14B3E
//   upper_bound      loop_top=0x14B66  epilogue=0x14B8E
//   insert_at_pos    loop_top=0x191F2  epilogue=0x19211 (LABEL_7 area
//                    that exits the loop with v8/v10 carrying the
//                    parent and proceeds to the post-loop logic
//                    safely)
#define LB_LOOP_TOP   (0x14B16_R)
#define LB_EPILOGUE   (0x14B3E_R)
#define UB_LOOP_TOP   (0x14B66_R)
#define UB_EPILOGUE   (0x14B8E_R)
#define IP_LOOP_TOP   (0x191F2_R)
#define IP_LOOP_EXIT  (0x19211_R)
// PREVIOUSLY: kill-switch on ConnectRenderSlot @ 0xC2650 (skip whole
// function during re-sim). Reverted — broke cpp_arena coherence.
// Forward sim's ConnectRenderSlot allocates boost::signals2 connection
// objects via operator_new → cpp_arena bump pointer advances. Re-sim
// skipping the function leaves cpp_arena's bump pointer behind, so
// page 0 (the arena metadata) diverges and downstream allocations
// land at different addresses. Result: 8+ divergent pages across
// sq_arena and cpp_arena, broken rendering.
//
// Conclusion: surgical hooks that DON'T affect allocation balance
// are the only safe approach. The mid-hooks above qualify (they
// don't change the program's allocation behaviour, just redirect
// EIP within an already-running function on corruption).

namespace {

static SafetyHookMid g_mid_lb{};
static SafetyHookMid g_mid_ub{};
static SafetyHookMid g_mid_ip{};

static std::atomic<uint64_t> g_lb_redirects{0};
static std::atomic<uint64_t> g_ub_redirects{0};
static std::atomic<uint64_t> g_ip_redirects{0};

static inline bool node_ok(uintptr_t v) {
    // User-space pointer that isn't NULL and isn't into the kernel
    // boundary. The lower bound 0x10000 stays well above the NULL
    // page guard region (the first 64KB are always reserved).
    return v >= 0x10000u && v < 0xFFFF0000u;
}

// Lower_bound's loop reads `[eax+10h]`. If eax is corrupted, redirect
// to 0x14B3E where the function does `mov eax, esi; pop esi; pop ebp;
// retn 4`. esi still holds the most-recent "parent" candidate that
// the loop assigned via `mov esi, eax` before descending left.
static void lb_loop_top(SafetyHookContext& ctx) {
    if (!node_ok(ctx.eax)) {
        uint64_t n = g_lb_redirects.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (n <= 4 || (n & 0x3FF) == 0) {
            log_printf("[treeguard] lower_bound CORRUPTION at loop "
                       "top: eax=%08X (hit #%llu) — redirecting to "
                       "epilogue\n",
                       (uint32_t)ctx.eax, (unsigned long long)n);
        }
        ctx.eip = LB_EPILOGUE;
    }
}

// Upper_bound's loop reads `[eax+10h]`. Epilogue at 0x14B8E does
// `mov eax, edx; pop ebp; retn 4`. edx holds the parent (saved via
// `mov edx, eax` on the left-descend branch).
static void ub_loop_top(SafetyHookContext& ctx) {
    if (!node_ok(ctx.eax)) {
        uint64_t n = g_ub_redirects.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (n <= 4 || (n & 0x3FF) == 0) {
            log_printf("[treeguard] upper_bound CORRUPTION at loop "
                       "top: eax=%08X (hit #%llu)\n",
                       (uint32_t)ctx.eax, (unsigned long long)n);
        }
        ctx.eip = UB_EPILOGUE;
    }
}

// insert_at_pos's loop reads `[edx+10h]` (the walker is in edx, not
// eax). On corruption, jump to 0x19211 (LABEL_7-ish — the path that
// exits the loop and proceeds with v8/edi as the parent). Edi already
// holds the previous parent via `mov edi, edx` at 0x191F5; that path
// is the function's "found duplicate / use existing position" branch
// and the subsequent code only reads from v15 (= v8 = edi), no
// further tree walks.
static void ip_loop_top(SafetyHookContext& ctx) {
    if (!node_ok(ctx.edx)) {
        uint64_t n = g_ip_redirects.fetch_add(
            1, std::memory_order_relaxed) + 1;
        if (n <= 4 || (n & 0x3FF) == 0) {
            log_printf("[treeguard] insert_at_pos CORRUPTION at loop "
                       "top: edx=%08X (hit #%llu)\n",
                       (uint32_t)ctx.edx, (unsigned long long)n);
        }
        ctx.eip = IP_LOOP_EXIT;
    }
}

} // namespace

void install() {
    g_mid_lb = safetyhook::create_mid((void*)LB_LOOP_TOP, lb_loop_top);
    g_mid_ub = safetyhook::create_mid((void*)UB_LOOP_TOP, ub_loop_top);
    g_mid_ip = safetyhook::create_mid((void*)IP_LOOP_TOP, ip_loop_top);
    log_printf("[treeguard] mid-hooks installed: "
               "lower_bound loop@0x%X %s, "
               "upper_bound loop@0x%X %s, "
               "insert_at_pos loop@0x%X %s "
               "(corruption check at loop-top only, no replacement)\n",
               (uint32_t)LB_LOOP_TOP, g_mid_lb.enabled() ? "OK" : "FAIL",
               (uint32_t)UB_LOOP_TOP, g_mid_ub.enabled() ? "OK" : "FAIL",
               (uint32_t)IP_LOOP_TOP, g_mid_ip.enabled() ? "OK" : "FAIL");
}

} // namespace tree_walker_guard
