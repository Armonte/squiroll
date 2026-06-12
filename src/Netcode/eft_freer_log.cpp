// safetyhook MUST be included before any squiroll header.
#include <safetyhook.hpp>

#include <windows.h>
#include <stdint.h>
#include <atomic>

#include "patch_utils.h"
#include "util.h"
#include "log.h"

#include "eft_freer_log.h"

namespace eft_freer_log {

// IDB addresses (RVA, IDB image-base 0).
//   0xEC130  Manbow__EwCEftGroupMgr__PruneAndDispatchCallbacks  size 0x14B
//   0xECB00  Manbow__EwCEftGroup__Dtor (body)                   size 0x3B2
//   0xECAD0  Manbow__EwCEftGroup__VectorDeletingDtor (vtable[0])
//   0x4DB0C8 g_ewEftGroupMgr (Ew::sEffect singleton *)
//   0xE5470  Manbow__EwCEftResChain__StepLayerMember            size 0x138
//
// Vector layout inside sEffect (the cEftGroup live-list):
//   sEffect + 0xEC  cEftGroup**  begin
//   sEffect + 0xF0  cEftGroup**  end   (= begin + 4*size)
//   sEffect + 0xF4  cEftGroup**  cap
//
// The sEffect chain is one suspect, but the RUNAWAY 15M-hits/sec NULL-skip
// storm we observe in solo stress is actually inside StepLayerMember, where
// `this` (a layer-member ptr loaded from sTask+0x180A4 + layer*0x148
// + array_index) is NULL. The crash_handler's universal instruction-skip
// recovery advances EIP one insn at a time and returns whatever eax/eip
// happen to land on — for StepLayerMember that translates to "return 1
// (still-alive)" most of the time, so the caller NEVER removes the stale
// NULL entry. Hence the perpetual hit storm.
//
// THE FIX (and the diagnostic): a SafetyHookInline on StepLayerMember that
// checks `this == NULL` and short-circuits with a clean `return 0`. The
// caller (StepLayers32 = sub_E5FE0) sees alive=false, calls memmove to
// remove the NULL slot, and the vector self-purges within a frame or two.
#define PRUNE_AND_DISPATCH_ADDR (0xEC130_R)
#define EFT_GROUP_DTOR_ADDR     (0xECB00_R)
#define STEP_LAYER_MEMBER_ADDR  (0xE5470_R)
#define STEP_LAYERS32_ADDR      (0xE5FE0_R)
#define G_EW_EFT_GROUP_MGR_ADDR (0x4DB0C8_R)
#define G_EW_EFT_RES_CHAIN_ADDR (0x4DB0B8_R)
#define EFT_VEC_BEGIN_OFF       0xEC
#define EFT_VEC_END_OFF         0xF0
#define EFT_VEC_CAP_OFF         0xF4
// sTask layer-arrays — per the StepLayers32 disasm:
//   ebx = esi + 0x18024              (layer 0 vector-triple begin)
//   per-layer increment: ebx += 0x0C (12) for next vector-triple
//   32 layers total
#define EFT_RES_LAYERS_BASE_OFF 0x18024
#define EFT_RES_LAYER_STRIDE    0x0C
#define EFT_RES_LAYER_COUNT     32

namespace {

static SafetyHookInline g_h_prune{};
static SafetyHookInline g_h_dtor{};
static SafetyHookInline g_h_step{};
static SafetyHookInline g_h_layers{};

// Counters for the StepLayerMember NULL-this guard. The call-site
// records sit below CallSiteRecord's definition.
static std::atomic<uint64_t> g_step_total{0};
static std::atomic<uint64_t> g_step_null_this{0};

// Re-entrancy counter for the prune walk. The prune walk calls
// vtable[0] (= vector deleting dtor) → sub_ECB00 (the dtor body),
// which we also hook. When this counter is non-zero, a dtor entry
// is from the legitimate prune path and not a buggy freer.
//
// The game is effectively single-threaded for the effect manager
// (the manager's own mutex serialises access). A plain global is
// fine; if we ever see it go negative or unbounded the counts will
// surface that.
static thread_local int g_in_prune = 0;

// Counters — published every 256 events so a long session has a
// readable trail without flooding the log.
static std::atomic<uint64_t> g_dtor_total{0};
static std::atomic<uint64_t> g_dtor_legit_in_prune{0};
static std::atomic<uint64_t> g_dtor_outside_prune{0};
static std::atomic<uint64_t> g_dtor_buggy_still_in_vec{0};

// Cap per-return-address logging to avoid flooding when the same
// freer fires every frame. We remember up to 32 distinct call
// sites and only log the first hit (plus a periodic re-emit).
struct CallSiteRecord {
    uintptr_t ret_addr;
    uint64_t  hits;
};
static CallSiteRecord g_callsites[32]{};
static int g_callsite_count = 0;

// Separate table for the StepLayerMember NULL-this hits so each
// callsite diagnostic stays attributable.
static CallSiteRecord g_step_callsites[16]{};
static int g_step_callsite_count = 0;

static CallSiteRecord* record_step_callsite(uintptr_t ret_addr) {
    for (int i = 0; i < g_step_callsite_count; ++i) {
        if (g_step_callsites[i].ret_addr == ret_addr) {
            ++g_step_callsites[i].hits;
            return &g_step_callsites[i];
        }
    }
    if (g_step_callsite_count <
            (int)(sizeof(g_step_callsites) / sizeof(g_step_callsites[0]))) {
        g_step_callsites[g_step_callsite_count].ret_addr = ret_addr;
        g_step_callsites[g_step_callsite_count].hits = 1;
        return &g_step_callsites[g_step_callsite_count++];
    }
    return nullptr;
}

static CallSiteRecord* record_callsite(uintptr_t ret_addr) {
    for (int i = 0; i < g_callsite_count; ++i) {
        if (g_callsites[i].ret_addr == ret_addr) {
            ++g_callsites[i].hits;
            return &g_callsites[i];
        }
    }
    if (g_callsite_count < (int)(sizeof(g_callsites) / sizeof(g_callsites[0]))) {
        g_callsites[g_callsite_count].ret_addr = ret_addr;
        g_callsites[g_callsite_count].hits = 1;
        return &g_callsites[g_callsite_count++];
    }
    return nullptr;  // table full — silently drop further distinct sites
}

// Resolve module base so we can log a portable RVA. The IDB has
// imagebase 0; the live module loads at ::base_address (util.h).
static inline uintptr_t to_rva(uintptr_t ea) {
    uintptr_t mb = ::base_address;
    if (mb && ea >= mb && (ea - mb) < 0x1000000u) return ea - mb;
    return ea;
}

// Scan sEffect's live-group vector for `this`. Returns the vector
// slot pointer if found, else nullptr. The vector is small (a few
// hundred groups peak); linear scan is fine. The manager's mutex
// is NOT held here — racing with a concurrent insert is acceptable
// for a diagnostic (worst case: a false negative).
static void** find_in_live_vec(void* group) {
    uintptr_t mgr_addr = G_EW_EFT_GROUP_MGR_ADDR;
    uintptr_t mgr = *(uintptr_t*)mgr_addr;
    if (!mgr) return nullptr;
    void** begin = *(void***)(mgr + EFT_VEC_BEGIN_OFF);
    void** end   = *(void***)(mgr + EFT_VEC_END_OFF);
    if (!begin || begin >= end) return nullptr;
    // Bounds check the cap too — a wildly mangled triple would
    // walk us into someone else's memory.
    void** cap   = *(void***)(mgr + EFT_VEC_CAP_OFF);
    if (cap < end) return nullptr;
    uintptr_t span = (uintptr_t)end - (uintptr_t)begin;
    if (span > 0x40000u) return nullptr;  // > 16k groups → reject
    for (void** p = begin; p != end; ++p) {
        if (*p == group) return p;
    }
    return nullptr;
}

// Pre-step scan: walk all 32 sTask layer-arrays before
// StepLayers32 runs and memmove-remove any entry whose object's
// vtable has been zeroed. Sibling-pattern to pre_prune_scan but on a
// different vector. The bug shape is identical: a freed layer member
// (cEftPrim / cEftCamera) has its arena memory zeroed by leak-on-free
// while its pointer remains in the layer's `actor` vector. When
// StepLayers32 reaches it, `mov ecx, [esi]; call StepLayerMember`
// crashes inside the function (this+5/+6 read on a zeroed-vtable
// object) and the destroy path then calls `vtable[0]` which is NULL.
//
// Removing the slot pre-step skips both faults entirely.
static std::atomic<uint64_t> g_layers_total{0};
static std::atomic<uint64_t> g_layer_bad_slots{0};

static void pre_step_scan(void* res_chain_this) {
    char* chain = (char*)res_chain_this;
    int total_total = 0;
    int total_bad = 0;
    for (int i = 0; i < EFT_RES_LAYER_COUNT; ++i) {
        char* trip = chain + EFT_RES_LAYERS_BASE_OFF + i * EFT_RES_LAYER_STRIDE;
        void** begin = *(void***)(trip + 0);
        void** end   = *(void***)(trip + 4);
        void** cap   = *(void***)(trip + 8);
        if (!begin || begin >= end || cap < end) continue;
        uintptr_t span = (uintptr_t)end - (uintptr_t)begin;
        if (span > 0x40000u) continue;
        int n_total = (int)(end - begin);
        total_total += n_total;

        void** p = begin;
        while (p != end) {
            void* entry = *p;
            bool bad = false;
            const char* reason = nullptr;
            uintptr_t ep = (uintptr_t)entry;
            if (!entry) {
                bad = true; reason = "nullptr-in-slot";
            } else if (ep < 0x1000u || ep >= 0xFFFF0000u) {
                bad = true; reason = "non-canonical-ptr";
            } else if (*(void**)entry == nullptr) {
                bad = true; reason = "vtable-zeroed";
            }

            if (bad) {
                ++total_bad;
                uint64_t bad_n = ++g_layer_bad_slots;
                if (bad_n <= 8 || (bad_n & 0x3F) == 0) {
                    const char* arena = "system";
                    if (ep >= 0x037B0000u && ep < 0x077B0000u) arena = "sq_arena";
                    else if (ep >= 0x077B0000u && ep < 0x0F7B0000u) arena = "cpp_arena";
                    else if (ep >= 0x0F7B0000u && ep < 0x117B0000u) arena = "bullet_arena";
                    log_printf("[eftfreer] PRE-STEP layer %d slot %d/%d: "
                               "entry=%p (%s) reason=%s; bad_total=%llu — "
                               "REMOVING from layer-array (sTask+0x%X)\n",
                               i, (int)(p - begin), n_total,
                               entry, arena, reason,
                               (unsigned long long)bad_n,
                               EFT_RES_LAYERS_BASE_OFF +
                                   i * EFT_RES_LAYER_STRIDE);
                }
                if ((p + 1) < end) {
                    memmove(p, p + 1, (size_t)((char*)end - (char*)(p + 1)));
                }
                --end;
                *(void***)(trip + 4) = end;
                continue;
            }
            ++p;
        }
    }
    if (total_bad > 0) {
        log_printf("[eftfreer] PRE-STEP summary: bad=%d/%d across 32 layers "
                   "(step#%llu)\n",
                   total_bad, total_total,
                   (unsigned long long)g_layers_total.load());
    }
}

static void thiscall layers_hook(void* res_chain_this) {
    uint64_t n = ++g_layers_total;
    if (n == 1 || (n & 0x3FF) == 0) {
        log_printf("[eftfreer] layers_hook fire #%llu chain=%p\n",
                   (unsigned long long)n, res_chain_this);
    }
    pre_step_scan(res_chain_this);
    g_h_layers.unsafe_thiscall<void>(res_chain_this);
}

// Hook on StepLayerMember (sub_E5470). __thiscall: this = layer_member_ptr.
//
// Original signature: char(float*) — returns 1 if the member is still
// alive (caller keeps it in the array), 0 if dead (caller memmove-removes
// the slot). We short-circuit the NULL-this case with a clean `return 0`,
// avoiding the universal NULL-skip's per-instruction trampolining and
// making the caller's vector self-purge instead of looping forever.
//
// Logging: per-distinct-caller first hit, then every 64th. The caller
// is always StepLayers32 (sub_E5FE0), but the call site within it pins
// which loop iteration we're in (StepLayers32 has multiple inlined calls).
static char thiscall step_member_hook(float* this_) {
    uint64_t total = ++g_step_total;
    if (total == 1) {
        log_printf("[eftfreer] step_member_hook FIRST fire this=%p\n", this_);
    }

    // Extended NULL guard: this can be non-NULL but POINT TO a zeroed
    // object (vtable == 0). The faulting site at e5537 reads
    // `[eax+0x20]` where `eax = *(this)`, so a NULL vtable faults at
    // address 0x20. Treat both as dead, so the caller (StepLayers32)
    // memmove-removes the slot.
    if (this_ && *(void**)this_ == nullptr) {
        uint64_t n = ++g_step_null_this;
        if (n <= 4 || (n & 0x3F) == 0) {
            uintptr_t mb = ::base_address;
            uintptr_t ra[5] = {0};
            ra[0] = (uintptr_t)__builtin_return_address(0);
#if defined(__clang__) || defined(__GNUC__)
            ra[1] = (uintptr_t)__builtin_return_address(1);
            ra[2] = (uintptr_t)__builtin_return_address(2);
            ra[3] = (uintptr_t)__builtin_return_address(3);
#endif
            uintptr_t caller = 0;
            for (auto a : ra) {
                if (mb && a >= mb && (a - mb) < 0x500000u) { caller = a; break; }
            }
            uintptr_t tp = (uintptr_t)this_;
            const char* arena = "system";
            if (tp >= 0x037B0000u && tp < 0x077B0000u) arena = "sq_arena";
            else if (tp >= 0x077B0000u && tp < 0x0F7B0000u) arena = "cpp_arena";
            else if (tp >= 0x0F7B0000u && tp < 0x117B0000u) arena = "bullet_arena";
            log_printf("[eftfreer] StepLayerMember(vtable=NULL): this=%p (%s) "
                       "caller_ret=0x%X (RVA 0x%X) hit#%llu — returning 0 "
                       "(caller will memmove the slot)\n",
                       this_, arena,
                       (uint32_t)caller, (uint32_t)to_rva(caller),
                       (unsigned long long)n);
        }
        return 0;
    }

    if (!this_) {
        uint64_t n = ++g_step_null_this;

        // Capture caller chain through safetyhook's trampoline frames
        // — the relevant one is the first frame inside the th155 image.
        uintptr_t mb = ::base_address;
        uintptr_t ret_addrs[5] = {0};
        ret_addrs[0] = (uintptr_t)__builtin_return_address(0);
#if defined(__clang__) || defined(__GNUC__)
        ret_addrs[1] = (uintptr_t)__builtin_return_address(1);
        ret_addrs[2] = (uintptr_t)__builtin_return_address(2);
        ret_addrs[3] = (uintptr_t)__builtin_return_address(3);
        ret_addrs[4] = (uintptr_t)__builtin_return_address(4);
#endif
        uintptr_t caller_ret = 0;
        for (auto a : ret_addrs) {
            if (mb && a >= mb && (a - mb) < 0x500000u) {
                caller_ret = a;
                break;
            }
        }

        CallSiteRecord* rec = record_step_callsite(caller_ret);
        bool emit = (!rec) || (rec->hits == 1) || ((rec->hits & 0x3F) == 0);
        if (emit) {
            log_printf("[eftfreer] StepLayerMember(NULL): caller_ret=0x%X "
                       "(RVA 0x%X) chain={0x%X,0x%X,0x%X,0x%X,0x%X} "
                       "site_hits=%llu total_null=%llu (returning 0 so "
                       "caller removes the stale slot)\n",
                       (uint32_t)caller_ret, (uint32_t)to_rva(caller_ret),
                       (uint32_t)to_rva(ret_addrs[0]),
                       (uint32_t)to_rva(ret_addrs[1]),
                       (uint32_t)to_rva(ret_addrs[2]),
                       (uint32_t)to_rva(ret_addrs[3]),
                       (uint32_t)to_rva(ret_addrs[4]),
                       rec ? (unsigned long long)rec->hits : 0ull,
                       (unsigned long long)n);
        }
        return 0;  // dead → caller will memmove-remove the slot
    }

    // Periodic alive heartbeat — confirms the hook is on the critical
    // path and not silently dead-coded.
    if ((total & 0x7FFF) == 0) {
        log_printf("[eftfreer] StepLayerMember heartbeat: total=%llu "
                   "null=%llu\n",
                   (unsigned long long)total,
                   (unsigned long long)g_step_null_this.load());
    }

    return g_h_step.unsafe_thiscall<char>(this_);
}

// Hook on PruneAndDispatchCallbacks (sub_EC130). __thiscall: this = mgr.
//
// The increment/decrement bracket lets dtor_hook tell legit (called from
// inside the prune walk's vtable[0] invocation) from buggy (a freer
// elsewhere). The original is always called — we are observing.
static std::atomic<uint64_t> g_prune_total{0};
static std::atomic<uint64_t> g_pre_prune_bad_slots{0};

// Walk sEffect's live-groups vector BEFORE the prune runs and count
// any entries whose group memory has already been zeroed (vtable == 0
// or group ptr non-canonical). Each is a stale entry left by some
// earlier freer that did not remove itself from the manager.
//
// Logging is rate-limited: we always log when a NEW bad slot appears
// (a vtable-zeroed group not seen before) and emit a per-prune summary
// only if there is at least one bad slot, so quiet runs stay quiet.
static void pre_prune_scan(void* mgr_this) {
    char* mgr = (char*)mgr_this;
    void** begin = *(void***)(mgr + EFT_VEC_BEGIN_OFF);
    void** end   = *(void***)(mgr + EFT_VEC_END_OFF);
    void** cap   = *(void***)(mgr + EFT_VEC_CAP_OFF);
    if (!begin || begin >= end || cap < end) return;
    uintptr_t span = (uintptr_t)end - (uintptr_t)begin;
    if (span > 0x40000u) return;

    int n_total = (int)(end - begin);
    int n_bad = 0;
    // Walk forward; when we hit a bad slot, memmove the tail down by
    // one slot and decrement `end`. p stays where it is so the
    // newly-shifted-in value at p is re-checked. This is exactly the
    // shape PruneAndDispatchCallbacks uses for its real prune, just
    // gated on "memory zeroed" instead of "dead flag / empty".
    //
    // Doing the removal up front matters because the prune walk's
    // very first action on a slot is `mov ebx, [edx+138h]` at EC172
    // — there is no NULL check that early, so just zeroing the slot
    // ptr would only relocate the crash from `[eax]` to `[edx+138h]`.
    // A genuinely-NULL ptr at the slot WOULD pass the later EC1C3
    // null check, but only after the unsafe +0x138 read. Removing
    // the slot entirely sidesteps both.
    void** p = begin;
    while (p != end) {
        void* group = *p;
        bool bad = false;
        const char* reason = nullptr;
        uintptr_t gp = (uintptr_t)group;

        if (!group) {
            bad = true; reason = "nullptr-in-slot";
        } else if (gp < 0x1000u || gp >= 0xFFFF0000u) {
            bad = true; reason = "non-canonical-ptr";
        } else if (*(void**)group == nullptr) {
            bad = true; reason = "vtable-zeroed";
        }

        if (bad) {
            ++n_bad;
            uint64_t bad_n = ++g_pre_prune_bad_slots;
            if (bad_n <= 8 || (bad_n & 0x3F) == 0) {
                const char* arena = "system";
                if (gp >= 0x037B0000u && gp < 0x077B0000u) arena = "sq_arena";
                else if (gp >= 0x077B0000u && gp < 0x0F7B0000u) arena = "cpp_arena";
                else if (gp >= 0x0F7B0000u && gp < 0x117B0000u) arena = "bullet_arena";
                log_printf("[eftfreer] PRE-PRUNE bad slot %d/%d: "
                           "group=%p (%s) reason=%s; bad_total=%llu — "
                           "REMOVING from sEffect+0x%X vec\n",
                           (int)(p - begin), n_total, group, arena, reason,
                           (unsigned long long)bad_n, EFT_VEC_BEGIN_OFF);
            }
            // memmove [p+1, end) → [p, end-1); end -= 1. Then re-check
            // the new value at p without advancing.
            if ((p + 1) < end) {
                memmove(p, p + 1, (size_t)((char*)end - (char*)(p + 1)));
            }
            --end;
            // Write the new end back to the manager's vector triple so
            // PruneAndDispatchCallbacks sees the shortened vector.
            *(void***)((char*)mgr_this + EFT_VEC_END_OFF) = end;
            // Do NOT ++p — re-check the shifted-in element.
            continue;
        }
        ++p;
    }
    if (n_bad > 0) {
        log_printf("[eftfreer] PRE-PRUNE summary: total=%d bad=%d "
                   "(prune#%llu) — vec shrunk to %d entries before prune ran\n",
                   n_total, n_bad, (unsigned long long)g_prune_total.load(),
                   n_total - n_bad);
    }
}

static void thiscall prune_hook(void* mgr_this) {
    uint64_t n = ++g_prune_total;
    if (n == 1 || (n & 0x3FF) == 0) {
        log_printf("[eftfreer] prune_hook fire #%llu mgr=%p\n",
                   (unsigned long long)n, mgr_this);
    }

    // Snapshot bad-slot state BEFORE the prune walk so we attribute
    // the corruption to a prior freer, not to the prune itself.
    pre_prune_scan(mgr_this);

    ++g_in_prune;
    g_h_prune.unsafe_thiscall<void>(mgr_this);
    --g_in_prune;
}

// Hook on cEftGroup dtor body (sub_ECB00). __thiscall: this = group.
//
// We capture the return address from the call site (one level up the
// stack — sub_ECB00's caller). In the prune-legit path the return
// address is inside the vector_deleting_dtor wrapper (0xECAD0..0xECAF6);
// outside the prune walk it's whatever code freed the group.
//
// SafetyHookInline's trampoline preserves __thiscall + sets up its own
// stack frame, so reading the caller's return address has to go through
// the runtime stack at the point the original function would see it.
// __builtin_return_address(0) inside this hook returns the trampoline's
// return; (0) and (1) both point inside safetyhook. To get the *real*
// caller (the freer), we instead read the value off [esp+0] BEFORE
// any prologue — using a naked-ish thunk would be cleanest but adds
// MSVC/Clang divergence. Practical compromise: log when found-in-vec,
// and capture __builtin_return_address(N) for N=0..3 as a chain so
// the analyser can pick out the non-safetyhook frame.
static void thiscall dtor_hook(void* this_) {
    uint64_t total = ++g_dtor_total;
    // First fire confirms the hook is reached at all.
    if (total == 1) {
        log_printf("[eftfreer] dtor_hook FIRST fire this=%p in_prune=%d\n",
                   this_, g_in_prune);
    }

    if (g_in_prune > 0) {
        ++g_dtor_legit_in_prune;
        // Periodic heartbeat.
        if ((total & 0xFF) == 0) {
            log_printf("[eftfreer] heartbeat: total=%llu prune_legit=%llu "
                       "outside=%llu buggy=%llu (in_prune=%d)\n",
                       (unsigned long long)total,
                       (unsigned long long)g_dtor_legit_in_prune.load(),
                       (unsigned long long)g_dtor_outside_prune.load(),
                       (unsigned long long)g_dtor_buggy_still_in_vec.load(),
                       g_in_prune);
        }
    } else {
        ++g_dtor_outside_prune;
        // Outside the prune walk — capture the call chain. Safetyhook
        // wraps us with its own stack frame so the immediate
        // __builtin_return_address(0) is inside the trampoline, not the
        // game. Walk a few frames to find the first return address that
        // is in the th155 module text.
        uintptr_t mb = ::base_address;
        uintptr_t ret_addrs[5] = {0};
        ret_addrs[0] = (uintptr_t)__builtin_return_address(0);
#if defined(__clang__) || defined(__GNUC__)
        ret_addrs[1] = (uintptr_t)__builtin_return_address(1);
        ret_addrs[2] = (uintptr_t)__builtin_return_address(2);
        ret_addrs[3] = (uintptr_t)__builtin_return_address(3);
        ret_addrs[4] = (uintptr_t)__builtin_return_address(4);
#endif
        // Pick the first frame inside the th155 image.
        uintptr_t freer_ret = 0;
        for (auto a : ret_addrs) {
            if (mb && a >= mb && (a - mb) < 0x500000u) {
                freer_ret = a;
                break;
            }
        }

        void** in_vec = find_in_live_vec(this_);
        if (in_vec) {
            uint64_t bug_n = ++g_dtor_buggy_still_in_vec;
            CallSiteRecord* rec = record_callsite(freer_ret);

            // Log first hit for any distinct call site; thereafter
            // every 64th hit to keep the log readable.
            bool emit = (!rec) || (rec->hits == 1) || ((rec->hits & 0x3F) == 0);
            if (emit) {
                log_printf("[eftfreer] !!! BUGGY FREER: cEftGroup=%p still "
                           "in sEffect+0x%X vec slot=%p; ret_addr=0x%08X "
                           "(RVA 0x%X) chain={0x%X,0x%X,0x%X,0x%X,0x%X} "
                           "site_hits=%llu total_bug=%llu\n",
                           this_, EFT_VEC_BEGIN_OFF, in_vec,
                           (uint32_t)freer_ret, (uint32_t)to_rva(freer_ret),
                           (uint32_t)to_rva(ret_addrs[0]),
                           (uint32_t)to_rva(ret_addrs[1]),
                           (uint32_t)to_rva(ret_addrs[2]),
                           (uint32_t)to_rva(ret_addrs[3]),
                           (uint32_t)to_rva(ret_addrs[4]),
                           rec ? (unsigned long long)rec->hits : 0ull,
                           (unsigned long long)bug_n);
            }
        } else {
            // Outside-prune dtor on a group that ISN'T in the live
            // vector — totally fine (e.g. the early-abort in
            // CreateEffectGroup when the group has zero primitives).
            // Quiet, but log a per-256 heartbeat so we can confirm
            // the path is reachable.
            uint64_t outside = g_dtor_outside_prune.load();
            if ((outside & 0xFF) == 0) {
                log_printf("[eftfreer] outside-prune dtor (not in vec): "
                           "this=%p ret=0x%X (RVA 0x%X) outside_n=%llu\n",
                           this_, (uint32_t)freer_ret,
                           (uint32_t)to_rva(freer_ret),
                           (unsigned long long)outside);
            }
        }
    }

    // ALWAYS call the original — we are observing, not replacing.
    // Skipping the dtor body leaks all of cEftGroup's owned vectors
    // (the operator_delete on the cEftGroup memory itself still runs
    // because that's in the vector_deleting_dtor wrapper at 0xECAD0,
    // not in the body we're hooking) but leaves a long tail of
    // misbehaviour downstream — the std::functions and the parent
    // backptr cleanup never run, so other groups end up holding stale
    // pointers that fault later.
    g_h_dtor.unsafe_thiscall<void>(this_);
}

} // namespace

void install() {
    g_h_prune = safetyhook::create_inline((void*)PRUNE_AND_DISPATCH_ADDR,
                                          (void*)prune_hook);
    g_h_dtor  = safetyhook::create_inline((void*)EFT_GROUP_DTOR_ADDR,
                                          (void*)dtor_hook);
    g_h_step  = safetyhook::create_inline((void*)STEP_LAYER_MEMBER_ADDR,
                                          (void*)step_member_hook);
    g_h_layers = safetyhook::create_inline((void*)STEP_LAYERS32_ADDR,
                                           (void*)layers_hook);
    log_printf("[eftfreer] hook PruneAndDispatchCallbacks @ 0x%X %s, "
               "cEftGroup dtor @ 0x%X %s, StepLayerMember @ 0x%X %s, "
               "StepLayers32 @ 0x%X %s; "
               "sEffect=*0x%X vec=+0x%X..+0x%X\n",
               (uint32_t)PRUNE_AND_DISPATCH_ADDR,
               g_h_prune.enabled() ? "OK" : "FAIL",
               (uint32_t)EFT_GROUP_DTOR_ADDR,
               g_h_dtor.enabled() ? "OK" : "FAIL",
               (uint32_t)STEP_LAYER_MEMBER_ADDR,
               g_h_step.enabled() ? "OK" : "FAIL",
               (uint32_t)STEP_LAYERS32_ADDR,
               g_h_layers.enabled() ? "OK" : "FAIL",
               (uint32_t)G_EW_EFT_GROUP_MGR_ADDR,
               EFT_VEC_BEGIN_OFF, EFT_VEC_END_OFF);
}

} // namespace eft_freer_log
