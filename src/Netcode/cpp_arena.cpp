// safetyhook MUST be included before any squiroll header — util.h #defines
// the calling-convention keywords as attribute macros and safetyhook uses
// those identifiers as method names. (Same ordering constraint as
// sq_arena.cpp.)
#include <safetyhook.hpp>

#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "cpp_arena.h"
#include "crash_handler.h" // register_thread for the hardware watchpoint
#include "patch_utils.h"   // _R address literal
#include "util.h"
#include "log.h"

// operator new(size_t) — th155.exe 0x2E15AB. The single scalar throwing
// operator new: std::vector / std::list (_Buynode0) / std::allocator and
// effectively every C++ heap object th155 allocates routes through it.
// Hooking it captures th155's whole C++ heap into a snapshot-able arena —
// the same model sq_arena uses for the Squirrel heap. (Hooking th155.exe's
// operator new does NOT touch squiroll/GekkoNet allocations: Netcode.dll
// has its own statically-linked CRT, so the arena stays battle-scoped.)
#define OPERATOR_NEW (0x2E15AB_R)
// _free_base — th155.exe 0x312347. The CRT free chokepoint (free / operator
// delete funnel here). Range-routed: an arena pointer goes to arena_free.
#define FREE_BASE    (0x312347_R)
// malloc — th155.exe 0x306FBC. The universal C/C++ heap chokepoint: operator
// new, operator new[] and direct malloc all funnel here. Hooked only to
// record call sites (DIAGNOSTIC) — not routed.
#define MALLOC_FN    (0x306FBC_R)
// _beginthreadex — th155.exe 0x3105AC. Every th155 worker thread (audio,
// loaders, ...) is spawned here. Hooked so each thread can be tagged by its
// start routine at creation: the audio thread is then excluded from the
// arena from boot, before it allocates anything.
#define BEGINTHREADEX (0x3105AC_R)

namespace cpp_arena {
namespace {

// 128 MB. Holds th155's live C++ heap; the per-frame snapshot copies only
// [base, base+bump). Overflow falls back to the real allocator gracefully.
// Sized for the WHOLE process's C++ heap (arena armed for the process
// lifetime), not just the battle.
static constexpr uint32_t ARENA_SIZE = 128u * 1024 * 1024;
static constexpr int      CLS_MIN_SH = 4;    // 16-byte smallest block
static constexpr int      CLS_MAX_SH = 24;   // 16 MB largest block
static constexpr int      NCLS       = CLS_MAX_SH - CLS_MIN_SH + 1;
static constexpr uint32_t HDR_MAGIC  = 0x42504143;  // 'CAPB' — allocated block
static constexpr uint32_t META_MAGIC = 0x4D504143;  // 'CAPM'

// 16-byte block header; keeps the payload 16-byte aligned.
struct Hdr {
    uint32_t cls;        // size-class shift, CLS_MIN_SH..CLS_MAX_SH
    uint32_t reqsize;    // requested payload size
    uint32_t link;       // free block: free-list link (arena offset of next).
                         // allocated block: caller RVA of the operator-new /
                         // raw_alloc site — DIAGNOSTIC, lets a rollback-
                         // divergent block be attributed to the code (battle
                         // vs audio vs ...) that allocated it.
    uint32_t magic;      // HDR_MAGIC while allocated, 0 while free
};
static_assert(sizeof(Hdr) == 16, "Hdr must be 16 bytes");

// Allocator metadata at arena offset 0 — a single memcpy of [base,bump)
// captures the allocator's own state AND the whole heap, so allocation is
// bit-reproducible across a rollback re-simulation.
struct Meta {
    uint32_t magic;
    uint32_t bump;             // offset of next fresh block (high-water)
    uint32_t live_bytes;       // currently-handed-out payload bytes
    uint32_t free_off[NCLS];   // per-class free-list head offset (0 = empty)
    uint32_t reserved[8];
};

static uint8_t* g_base      = nullptr;
static Meta*    g_meta      = nullptr;
static bool     g_installed = false;
static bool     g_armed     = false;   // route operator new -> arena only while a match is armed
static bool     g_resim     = false;   // a rollback re-simulation advance is in progress
static uint32_t g_warn      = 8;
static uint32_t g_resim_skips = 0;     // real-heap frees suppressed during re-sim
static uint32_t g_xthr_skips  = 0;     // arena frees from a non-sim thread, leaked
static DWORD    g_sim_tid   = 0;       // simulation thread; once set, ONLY this
                                       // thread's operator new -> arena (keeps
                                       // the audio thread out of the snapshot)

// DIAGNOSTIC: caller of the in-flight operator new, + a quota for logging
// size-class-13 (8 KB) bump-allocations — the rollback re-sim does one extra,
// diverging cpp_arena (the DESYNC frame=11 root).
static uint32_t g_opnew_caller = 0;
static int      g_cls13_log    = 0;   // [cpp13] dump DISABLED (cpp excluded from checksum)

// DIAGNOSTIC (Phase 1, post-input-fix residual): per-advance count of the two
// divergent allocators — DrawCommandSlot reset (0x57DC0) and boost::signals2
// shared_count (0x31C00) — to test the forward-vs-re-sim asymmetry hypothesis.
static uint32_t g_diag_render = 0;
static uint32_t g_diag_signal = 0;
static uint32_t g_diag_signal_off = 0;   // signal-node allocs on a NON-sim thread

static SafetyHookInline g_h_opnew{};
static SafetyHookInline g_h_free{};
static SafetyHookInline g_h_malloc{};
static SafetyHookInline g_h_bthreadex{};

// Threads whose operator new must NEVER reach the arena — the audio thread
// above all. Populated by the _beginthreadex hook from each thread's start
// routine, so the exclusion is live from the thread's first instruction.
static DWORD    g_excl_tid[16] = {0};
static int      g_n_excl_tid   = 0;
// Start-routine RVA window that marks a thread as audio/non-sim. Filled in
// from the [thread] log; 0,0 = match nothing (log-only first pass).
static uint32_t g_audio_lo = 0;
static uint32_t g_audio_hi = 0;

// "Deterministic workers" — additional threads beyond g_sim_tid that ARE
// allowed to allocate into the arena. The Ew::cJobThread workers spawned
// via Manbow__EwCJobThread__StartAddress (RVA 0x1205A0) handle per-frame
// task dispatch and DO mutate game state (boost::signals2 grouped_list
// connection inserts, etc). Until 2026-05-27 these were locked out of the
// arena by the sim-only gate, so their allocations landed in the real
// Win32 heap which is NOT snapshotted — that's the source of the freeze-
// on-hit during rollback re-sim: re-sim's boost::signals2 mutations
// duplicated the forward-sim state, eventually corrupting the
// grouped_list trees with cycles.
//
// We identify these threads at _beginthreadex time by matching start
// against the cJobThread StartAddress thunk. Once added here, the same
// gate that admits the sim thread admits these too.
static DWORD    g_worker_tid[64] = {0};
static int      g_n_worker_tid   = 0;
// Start-routine RVA window for the cJobThread StartAddress thunk.
// 0x1205A0 is the single entry point (StartAddress in IDB) — we match
// it exactly. lo<=srva<hi range is left for any future cohort that needs
// wider matching.
static uint32_t g_worker_start_lo = 0x1205A0;
static uint32_t g_worker_start_hi = 0x1205A1;

static bool thread_excluded(DWORD tid) {
    for (int i = 0; i < g_n_excl_tid; ++i)
        if (g_excl_tid[i] == tid) return true;
    return false;
}

static bool thread_is_worker(DWORD tid) {
    for (int i = 0; i < g_n_worker_tid; ++i)
        if (g_worker_tid[i] == tid) return true;
    return false;
}

// The arena is armed for the whole process lifetime and th155 hits operator
// new / _free_base from MULTIPLE THREADS (background resource loading). The
// bump pointer and per-class free lists are not atomic, so arena_alloc and
// arena_free serialise on this lock. A Win32 CRITICAL_SECTION is recursive,
// so nested entry (any future arena_realloc -> arena_alloc) is safe.
// Initialised by install() before the hooks go live.
static CRITICAL_SECTION g_lock;

// DIAGNOSTIC: ring of recent th155 malloc() calls (caller RVA + size + ptr),
// recorded only while armed. trace_alloc() walks it to attribute a divergent
// pointer to the exact allocation call site.
struct MallocRec { uint32_t caller; uint32_t size; uint32_t ptr; };
static constexpr int MW_RING = 16384;
static MallocRec  g_mw[MW_RING];
static uint32_t   g_mw_idx = 0;

// DIAGNOSTIC: per-frame arena alloc/free sequence trace. Records every
// arena event of a chosen frame so the forward run and a rollback re-sim
// of that SAME frame can be diffed event-by-event — the first mismatch is
// the exact point the re-sim deviates. See trace_reset / trace_check.
struct TraceEv { uint8_t op; uint32_t size, caller, off; };  // op 1=alloc 2=free
static constexpr uint32_t TRACE_CAP   = 65536;
static TraceEv  g_tr[TRACE_CAP];          // the current advance's events
static uint32_t g_tr_n        = 0;
// Per-frame ring of forward alloc sequences: a re-sim of ANY frame diffs
// against that frame's forward sequence, so the divergence ONSET frame is
// found automatically (it is run-variable). RING > rollback window (8).
static constexpr uint32_t TR_RING     = 12;
static constexpr uint32_t TR_SLOT_CAP = 8192;
static TraceEv  g_tr_ring[TR_RING][TR_SLOT_CAP];
static uint32_t g_tr_ring_n[TR_RING]     = {0};
static int32_t  g_tr_ring_frame[TR_RING];          // frame in each slot (-1 = empty)
static bool     g_tr_ring_init = false;
static int      g_tr_div_log   = 8;                // log the first N divergent frames

static void trace_rec(uint8_t op, uint32_t size, uint32_t caller, uint32_t off) {
    if (g_tr_n < TRACE_CAP) {
        TraceEv& e = g_tr[g_tr_n++];
        e.op = op; e.size = size; e.caller = caller; e.off = off;
    }
}

// Smallest class shift whose block (1<<sh) holds sizeof(Hdr)+n.
static int class_for(size_t n) {
    size_t need = n + sizeof(Hdr);
    int sh = CLS_MIN_SH;
    while (((size_t)1 << sh) < need) {
        if (sh >= CLS_MAX_SH) return CLS_MAX_SH + 1;  // too big for the arena
        ++sh;
    }
    return sh;
}

static void* arena_alloc(size_t n) {
    int sh = class_for(n);
    if (sh > CLS_MAX_SH) return nullptr;
    int      ci  = sh - CLS_MIN_SH;
    uint32_t blk = 1u << sh;
    uint32_t off;
    // Lock spans every read/write of g_meta->bump and g_meta->free_off[].
    EnterCriticalSection(&g_lock);
    if (g_meta->free_off[ci]) {
        off = g_meta->free_off[ci];
        g_meta->free_off[ci] = ((Hdr*)(g_base + off))->link;
    } else {
        if ((uint64_t)g_meta->bump + blk > ARENA_SIZE) {
            if (g_warn) {
                --g_warn;
                log_printf("[cpp_arena] !! ARENA FULL bump=%u +%u\n",
                           g_meta->bump, blk);
            }
            LeaveCriticalSection(&g_lock);
            return nullptr;
        }
        off = g_meta->bump;
        g_meta->bump += blk;
        // DIAGNOSTIC: cls-13 (8 KB) bump-grows are the cpp_arena divergence.
        // Scan the stack for th155 return addresses to get the call chain.
        if (sh == 13 && g_cls13_log > 0) {
            --g_cls13_log;
            log_printf("[cpp13] BUMP cls13 size=%u caller_rva=%08X bump->%u "
                       "chain:\n", (uint32_t)n,
                       g_opnew_caller - (uint32_t)base_address, g_meta->bump);
            const uint32_t* sp = (const uint32_t*)&blk;
            for (int k = 0; k < 110; ++k) {
                uint32_t rva = sp[k] - (uint32_t)base_address;
                if (rva >= 0x1000 && rva < 0x300000)
                    log_printf("[cpp13]   stack[+0x%02X] rva=%08X\n", k * 4, rva);
            }
        }
    }
    Hdr* h = (Hdr*)(g_base + off);
    h->cls       = (uint32_t)sh;
    h->reqsize   = (uint32_t)n;
    h->link      = g_opnew_caller
                 ? (uint32_t)(g_opnew_caller - (uint32_t)base_address) : 0;
    h->magic     = HDR_MAGIC;
    g_meta->live_bytes += (uint32_t)n;
    trace_rec(1, (uint32_t)n, h->link, off);
    LeaveCriticalSection(&g_lock);
    return g_base + off + sizeof(Hdr);
}

static void arena_free(void* p) {
    Hdr* h = (Hdr*)((uint8_t*)p - sizeof(Hdr));
    // Lock spans every read/write of g_meta->free_off[] (and the header
    // mutation that links the block onto a free list).
    EnterCriticalSection(&g_lock);
    if (h->magic != HDR_MAGIC) {
        if (g_warn) {
            --g_warn;
            log_printf("[cpp_arena] !! free of bad/double block magic=%08x\n",
                       h->magic);
        }
        LeaveCriticalSection(&g_lock);
        return;
    }
    int ci = (int)h->cls - CLS_MIN_SH;
    if (ci < 0 || ci >= NCLS) {
        LeaveCriticalSection(&g_lock);
        return;
    }
    g_meta->live_bytes -= h->reqsize;
    uint32_t foff = (uint32_t)((uint8_t*)h - g_base);
    trace_rec(2, h->reqsize, 0, foff);
    // EXPERIMENTAL — option (ii): instead of putting the block on its
    // size-class free list, LEAK it. arena_alloc always bumps to fresh
    // memory, no recycle. Used to test the hypothesis that cpp_arena's
    // deterministic recycle exposes a latent use-after-free in TH155
    // (a freed list node gets reused immediately and the new owner's
    // bytes look like list pointers when the stale list walks it).
    //
    // If this fixes the hit-crash, the recycle is the cause and we
    // need either a quarantine (delay recycle N frames) or to find
    // the offending freer. Memory cost is real — full 128MB capacity
    // gets exhausted faster — but should comfortably last one match.
    // Zero-fill the payload too so any stale read sees null, not the
    // previous live data.
    memset(p, 0, h->reqsize);
    h->magic     = 0;
    // Free-list link NOT updated: block stays orphaned, arena_alloc
    // will skip the free list and bump.
    (void)foff;
    (void)ci;
    LeaveCriticalSection(&g_lock);
}

static inline bool in_arena(const void* p) {
    return p && (const uint8_t*)p > g_base
             && (const uint8_t*)p < g_base + ARENA_SIZE;
}

// --- hooks --------------------------------------------------------------

// Caller RVA ranges whose operator-new allocations must NOT enter the arena.
// The arena is the rollback snapshot; an object a non-deterministic subsystem
// (the audio thread) mutates would make the snapshot diverge on a re-sim.
// These ranges are found with cpp_arena::attribute() — it names the caller of
// any block that diverges, so this list grows from evidence, not guesses.
struct ExclRange { uint32_t lo, hi; };   // RVA half-open [lo, hi)
static const ExclRange g_excl[] = {
    // tf4_ogg_alloc_shared — operator new shared_ptr<TF4::Ogg>, the Ogg/Vorbis
    // audio decoder object; the audio thread mutates it every frame.
    { 0x16B380u, 0x16B400u },
};
static bool caller_excluded(uint32_t abs_caller) {
    uint32_t rva = abs_caller - (uint32_t)base_address;
    for (const ExclRange& e : g_excl)
        if (rva >= e.lo && rva < e.hi) return true;
    return false;
}

// operator new: while a rollback session is armed, serve from the arena so
// the battle's C++ heap is part of the snapshot. Outside a match (boot,
// menus) pass straight to the real allocator — that keeps the arena, hence
// the per-frame snapshot, bounded to battle-era allocations. A caller in the
// excluded set (audio) also passes through: its objects are non-deterministic
// and must stay out of the snapshot. On overflow likewise fall through.
static void* cdecl hook_op_new(size_t size) {
    if (g_armed && g_meta) {
        uint32_t caller = (uint32_t)(uintptr_t)_ReturnAddress();
        g_opnew_caller = caller;
        // DIAGNOSTIC: tally the residual render/signal allocators (Phase 1).
        uint32_t crva = caller - (uint32_t)base_address;
        // render = DrawCommandSlot create site (Manbow__DrawCommandSlot__create_and_bind);
        // signal = boost::signals2 grouped connection-list node alloc (_Buynode0 0x13F10).
        if (crva >= 0x56A90 && crva < 0x56AD0) ++g_diag_render;
        else if (crva >= 0x13F00 && crva < 0x13F40) {
            ++g_diag_signal;
            // Is this connection-list node allocated on the SIM thread or a
            // cJobThread WORKER? Worker-thread allocs at non-deterministic times
            // swap node addresses (same total bump) -> the residual swap.
            // (tid isn't computed until further down, so query directly here.)
            if (g_sim_tid && GetCurrentThreadId() != g_sim_tid) ++g_diag_signal_off;
            // One-shot caller-chain dump for the per-frame signals2 connection-
            // list node alloc (_Buynode0) — names the connect() SITE up the
            // stack so we can see what orders the 8-10 per-frame connections
            // (the swap source after the input fix).
            // Skip the f=0 setup burst (~1105 connections) so the chain we dump
            // is a PER-FRAME (battle) connect — the divergent kind (high-addr).
            static uint32_t sig_skip = 1150;
            static int sig_chain_log = 12;
            if (sig_skip) {
                --sig_skip;
            } else if (!g_resim && sig_chain_log > 0) {
                --sig_chain_log;
                volatile uint32_t marker = 0;
                const uint32_t* sp = (const uint32_t*)&marker;
                log_printf("[sigchain] connlist node alloc — caller chain:\n");
                for (int k = 0; k < 48; ++k) {
                    uint32_t r = sp[k] - (uint32_t)base_address;
                    if (r >= 0x1000 && r < 0x300000)
                        log_printf("[sigchain]   stk[+0x%02X] rva=%08X\n", k * 4, r);
                }
            }
        }
        // Thread gate: once the simulation thread is known, only IT may draw
        // from the arena. Background threads — the audio thread above all —
        // go to the real heap; their non-deterministic alloc/free would
        // otherwise churn the arena free-lists and shift where battle objects
        // land, diverging the rollback snapshot. A thread tagged at creation
        // (the _beginthreadex hook) is excluded even before the sim thread is
        // known, so audio objects never enter the baseline.
        // STRICT: only the simulation thread, and only once it is known.
        // Before that (boot / engine init) operator new goes to the real
        // heap — those objects are not battle state. better_game_loop sets
        // the sim thread very early, before menus / vs.Initialize.
        DWORD tid = GetCurrentThreadId();
        bool sim    = (g_sim_tid != 0) && (tid == g_sim_tid);
        bool worker = thread_is_worker(tid);
        // PRE-GATE window: between cpp_arena::install() and
        // set_sim_thread(), g_sim_tid is 0. Without this branch the
        // sim-only gate rejects EVERY allocation in that window, so
        // bootstrap objects (boost::signals2 signal owners, the
        // grouped_list_state structs the Actor2D render slots connect
        // INTO, font tables, etc.) land in the real Win32 heap and
        // permanently escape the rollback snapshot. That's the source
        // of the freeze-on-hit: re-sim mutates the in-arena connection
        // node list, but the signal owner that POINTS into that list
        // is at a fixed real-heap address holding stale forward-sim
        // pointers — eventually a cycle, then the walk loops forever.
        //
        // Pre-gate we admit ALL threads (still respecting excluded
        // sets / callers). The arena is 128 MB which comfortably
        // covers boot. Once sim_tid is set the gate narrows to
        // sim + workers + non-excluded — same as before.
        bool pre_gate = (g_sim_tid == 0);
        bool admitted = sim || worker || pre_gate;
        if (admitted && !thread_excluded(tid) && !caller_excluded(caller)) {
            void* p = arena_alloc(size);
            if (p) return p;
        }
    }
    return g_h_opnew.unsafe_ccall<void*>(size);
}

// _free_base: an arena pointer is detected by address range — exact, no
// caller check, and arena_free IS rolled back with the snapshot.
//
// A NON-arena (real Win32 heap) pointer is the problem: the real heap is not
// part of the rollback snapshot. During a rollback re-simulation the forward
// run has ALREADY freed that block — re-freeing it is a double-free, which
// RtlFreeHeap turns into a STATUS_HEAP_CORRUPTION fastfail. So a real-heap
// free issued from inside a re-sim advance is suppressed (the block stays
// freed from the forward pass; the bounded leak is the transient buffers a
// growing std::vector sheds — they stop once capacity settles).
static void cdecl hook_free(void* block) {
    if (in_arena(block)) {
        // Non-sim, non-worker threads (the audio thread) freeing an arena
        // block — necessarily a pre-gate baseline object — would push it
        // onto a free-list at a non-deterministic time, shifting where the
        // next simulation allocation lands and diverging the snapshot.
        // Leak it instead; the set of such blocks is bounded (pre-gate
        // only). Worker threads (Ew::cJobThread) ARE allowed to free —
        // their allocations come from the arena too and live in the
        // rollback snapshot just like the sim thread's.
        DWORD tid = GetCurrentThreadId();
        bool admitted = (g_sim_tid == 0)                       // pre-gate
                     || (g_sim_tid != 0 && tid == g_sim_tid)   // sim
                     || thread_is_worker(tid);                 // worker
        if (g_sim_tid != 0 && !admitted) {
            if (g_warn && (g_xthr_skips & 0xFF) == 0)
                log_printf("[cpp_arena] off-thread arena free leaked %p (#%u)\n",
                           block, g_xthr_skips);
            ++g_xthr_skips;
            return;
        }
        arena_free(block);
        return;
    }
    if (g_resim) {
        // DIAGNOSTIC: log caller RVA every 64 suppressions so we can
        // identify the divergence sources (real-heap frees that the
        // forward run does but the re-sim suppresses). The forward run's
        // matching free DOES happen — the asymmetry is exactly here.
        if ((g_resim_skips & 0x3F) == 0) {
            uint32_t caller = (uint32_t)(uintptr_t)_ReturnAddress();
            log_printf("[cpp_arena] re-sim: suppressed real-heap free %p "
                       "(#%u) by rva=%08X\n", block, g_resim_skips,
                       caller - (uint32_t)base_address);
        }
        ++g_resim_skips;
        return;
    }
    g_h_free.unsafe_ccall<void>(block);
}

// malloc: pure diagnostic — call the original, then record {caller,size,ptr}
// in the ring while armed. The caller address is malloc's return address
// (safetyhook inline preserves the original call frame).
static void* cdecl hook_malloc(size_t size) {
    void* p = g_h_malloc.unsafe_ccall<void*>(size);
    if (g_armed) {
        uint32_t i = (g_mw_idx++) & (MW_RING - 1);
        g_mw[i].caller = (uint32_t)(uintptr_t)_ReturnAddress();
        g_mw[i].size   = (uint32_t)size;
        g_mw[i].ptr    = (uint32_t)(uintptr_t)p;
    }
    return p;
}

// _beginthreadex: tag every th155 worker thread by its start routine at
// creation. A thread whose start routine falls in the audio window is
// excluded from the arena before it runs a single instruction, so no audio
// object ever enters the rollback snapshot — not even in the baseline.
static uintptr_t cdecl hook_beginthreadex(void* sec, unsigned stk, void* start,
                                          void* arg, unsigned flag,
                                          unsigned* tidp) {
    uintptr_t h = g_h_bthreadex.unsafe_ccall<uintptr_t>(sec, stk, start, arg,
                                                        flag, tidp);
    if (h) {
        DWORD tid = (tidp && *tidp) ? (DWORD)*tidp
                                    : GetThreadId((HANDLE)h);
        uint32_t srva = (uint32_t)((uintptr_t)start - base_address);
        bool audio = (g_audio_hi > g_audio_lo) &&
                     (srva >= g_audio_lo && srva < g_audio_hi);
        // Ew::cJobThread workers spawn at Manbow__EwCJobThread__StartAddress
        // (0x1205A0). These are deterministic per-frame task workers and
        // their allocations MUST land in cpp_arena so the rollback snapshot
        // covers them (the freeze-on-hit issue was their boost::signals2
        // grouped_list inserts landing on the un-snapshotted real heap).
        bool worker = (g_worker_start_hi > g_worker_start_lo) &&
                      (srva >= g_worker_start_lo && srva < g_worker_start_hi);
        if (audio && g_n_excl_tid < 16) g_excl_tid[g_n_excl_tid++] = tid;
        if (worker && g_n_worker_tid <
                          (int)(sizeof(g_worker_tid) / sizeof(g_worker_tid[0])))
            g_worker_tid[g_n_worker_tid++] = tid;
        crash_handler::register_thread((uint32_t)tid);
        log_printf("[cpp_arena] thread spawned tid=%u start_rva=%08X%s%s\n",
                   tid, srva,
                   audio  ? "  [excluded from arena]" : "",
                   worker ? "  [WORKER: allocations -> cpp_arena]" : "");
    }
    return h;
}

} // namespace

void install() {
    if (g_installed) return;

    // Serialises arena_alloc/arena_free across th155's threads. Created
    // before the hooks go live so the very first hooked call is already
    // protected.
    InitializeCriticalSection(&g_lock);

    // MEM_WRITE_WATCH: a snapshot module tracks which pages each frame
    // dirties (GetWriteWatch), so a rollback snapshot copies only what
    // changed, not the whole arena.
    g_base = (uint8_t*)VirtualAlloc(nullptr, ARENA_SIZE,
                                    MEM_COMMIT | MEM_RESERVE | MEM_WRITE_WATCH,
                                    PAGE_READWRITE);
    if (!g_base) {
        log_printf("[cpp_arena] !! VirtualAlloc(%u) failed — C++ heap stays "
                   "on the CRT heap\n", ARENA_SIZE);
        return;
    }
    g_meta = (Meta*)g_base;
    g_meta->magic      = META_MAGIC;
    g_meta->bump       = (sizeof(Meta) + 15u) & ~15u;  // first block 16-aligned
    g_meta->live_bytes = 0;
    for (int i = 0; i < NCLS; ++i) g_meta->free_off[i] = 0;

    // Install the FREE hook first: the instant the operator-new hook goes
    // live it hands out arena pointers, and their frees must already be
    // range-routed — otherwise a real _free_base on an arena pointer in the
    // install window would corrupt the CRT heap.
    g_h_free   = safetyhook::create_inline((void*)FREE_BASE,    (void*)hook_free);
    g_h_opnew  = safetyhook::create_inline((void*)OPERATOR_NEW, (void*)hook_op_new);
    g_h_malloc = safetyhook::create_inline((void*)MALLOC_FN,    (void*)hook_malloc);
    g_h_bthreadex = safetyhook::create_inline((void*)BEGINTHREADEX,
                                              (void*)hook_beginthreadex);

    int ok = g_h_free.enabled() + g_h_opnew.enabled();
    g_installed = (ok == 2);
    log_printf("[cpp_arena] install: arena=%p %uMB hooks=%d/2 mallocwatch=%d\n",
               g_base, ARENA_SIZE / (1024u * 1024u), ok,
               (int)g_h_malloc.enabled());
}

void trace_alloc(uint32_t addr) {
    const MallocRec* best = nullptr;
    for (int k = 0; k < MW_RING; ++k) {
        const MallocRec& r = g_mw[k];
        if (r.ptr && addr >= r.ptr && addr < r.ptr + r.size) {
            // Prefer the tightest enclosing block (the most recent reuse).
            if (!best || r.size < best->size) best = &r;
        }
    }
    if (best) {
        log_printf("[mallocwatch] %08X is inside malloc block [%08X+%X]  "
                   "caller=%08X  rva=%08X\n",
                   addr, best->ptr, best->size, best->caller,
                   (uint32_t)(best->caller - base_address));
    } else {
        log_printf("[mallocwatch] %08X not in malloc ring "
                   "(predates the ring, or not allocated via malloc)\n", addr);
    }
}

void     set_armed(bool on) { g_armed = on; }
bool     is_armed()         { return g_armed; }
void     set_resim(bool on) { g_resim = on; }
bool     is_resim()         { return g_resim; }

// DIAGNOSTIC (Phase 1): log + reset the per-advance render/signal alloc counts.
void diag_alloc_counts(int frame, int rb) {
    if (g_diag_render || g_diag_signal)
        log_printf("[allocdiag] f=%d rb=%d render=%u signal=%u signal_offthread=%u\n",
                   frame, rb, g_diag_render, g_diag_signal, g_diag_signal_off);
    g_diag_render = 0;
    g_diag_signal = 0;
    g_diag_signal_off = 0;
}

// Designate the simulation thread — call from the battle/game thread once,
// before snapshot_ring::arm() takes the baseline. From here on only this
// thread's operator new is routed into the arena.
void set_sim_thread(uint32_t tid) {
    if (g_sim_tid == 0 && tid != 0) {
        g_sim_tid = (DWORD)tid;
        log_printf("[cpp_arena] sim thread = %u — arena now thread-gated\n", tid);
    }
}

bool is_sim_thread() {
    return g_sim_tid != 0 && GetCurrentThreadId() == g_sim_tid;
}

// Public raw allocation — hand a block straight out of the arena, bypassing
// the operator-new routing. Used to re-home th155's TF4 Squirrel-instance
// object pool (which would otherwise carve its slabs from the TF4-engine
// mspace, a heap that cannot be rollback-snapshotted because the audio
// thread keeps live decoder state there). Returns nullptr on overflow, so
// the caller can fall back. The block is range-routed back to arena_free
// like any other arena pointer, so a later free is handled.
void* raw_alloc(uint32_t n, uint32_t caller_abs) {
    if (!g_meta) return nullptr;
    // Tag the block with the real th155 call site (passed through by the
    // redirect shim) so attribute() names actual code, not the Netcode.dll
    // shim. See Hdr::link / attribute().
    g_opnew_caller = caller_abs ? caller_abs
                   : (uint32_t)(uintptr_t)_ReturnAddress();
    return arena_alloc(n);
}

// DIAGNOSTIC: attribute an arena byte offset to the block that owns it and
// log the allocating caller's RVA. Lets a rollback-divergent arena region
// be traced to the exact code — and subsystem (battle / audio / ...) — that
// allocated the object, instead of guessing. Walks the bump arena block by
// block (each block is 1<<cls bytes, contiguous, no gaps).
void attribute(uint32_t off) {
    if (!g_meta) return;
    uint32_t p    = (sizeof(Meta) + 15u) & ~15u;   // first block — see install()
    uint32_t bump = g_meta->bump;
    while (p < bump) {
        Hdr* h = (Hdr*)(g_base + p);
        if (h->cls < (uint32_t)CLS_MIN_SH || h->cls > (uint32_t)CLS_MAX_SH) {
            log_printf("[cpp_attr] off=0x%X: block walk lost at 0x%X\n", off, p);
            return;
        }
        uint32_t blk = 1u << h->cls;
        if (off >= p && off < p + blk) {
            bool live = (h->magic == HDR_MAGIC);
            log_printf("[cpp_attr] off=0x%X -> block@0x%X blk=%u reqsize=%u %s "
                       "caller_rva=%08X\n", off, p, blk, h->reqsize,
                       live ? "LIVE" : "free", live ? h->link : 0u);
            return;
        }
        p += blk;
    }
    log_printf("[cpp_attr] off=0x%X: not in any block (bump=0x%X)\n", off, bump);
}

// DIAGNOSTIC: clear the per-frame arena event trace (call when a frame's
// advance is about to begin recording).
void trace_reset() { g_tr_n = 0; }

// DIAGNOSTIC: at frame `frame`'s capture, if it is the traced frame, save
// the forward run's event sequence the first time and diff a later (re-sim)
// pass against it — the first differing event is where the re-sim deviates.
void trace_check(uint32_t frame, int rb) {
    if (!g_tr_ring_init) {
        for (uint32_t i = 0; i < TR_RING; ++i) g_tr_ring_frame[i] = -1;
        g_tr_ring_init = true;
    }
    uint32_t slot = frame % TR_RING;
    if (!rb) {   // forward (g_resim is already reset to 0 by this point — use rb)
        // Forward: stash this frame's alloc sequence for the later re-sim diff.
        uint32_t n = g_tr_n < TR_SLOT_CAP ? g_tr_n : TR_SLOT_CAP;
        memcpy(g_tr_ring[slot], g_tr, sizeof(TraceEv) * n);
        g_tr_ring_n[slot]     = n;
        g_tr_ring_frame[slot] = (int32_t)frame;
        return;
    }
    // Re-sim: diff against this frame's saved forward sequence; report the
    // first frame whose ALLOC ORDER diverges (the swap onset).
    if (g_tr_ring_frame[slot] != (int32_t)frame || g_tr_div_log <= 0) return;
    uint32_t rn  = g_tr_n < TR_SLOT_CAP ? g_tr_n : TR_SLOT_CAP;
    uint32_t sn  = g_tr_ring_n[slot];
    uint32_t lim = rn < sn ? rn : sn;
    uint32_t i = 0;
    for (; i < lim; ++i) {
        const TraceEv& a = g_tr_ring[slot][i];
        const TraceEv& b = g_tr[i];
        if (a.op != b.op || a.size != b.size || a.caller != b.caller || a.off != b.off)
            break;
    }
    if (i >= lim && rn == sn) return;     // identical — nothing to report
    --g_tr_div_log;
    log_printf("[cpptrace] f=%u FIRST DIVERGENT ALLOC @ event #%u (fwd %u events, "
               "resim %u)\n", frame, i, sn, rn);
    uint32_t lo = i >= 3 ? i - 3 : 0;
    uint32_t hi = (i + 4 < lim) ? i + 4 : lim;
    for (uint32_t k = lo; k < hi; ++k) {
        const TraceEv& a = g_tr_ring[slot][k];
        const TraceEv& b = g_tr[k];
        log_printf("[cpptrace]   #%u  FWD %s sz=%u caller=%08X off=%X  vs  RESIM %s "
                   "sz=%u caller=%08X off=%X %s\n", k,
                   a.op == 1 ? "alloc" : "free ", a.size, a.caller, a.off,
                   b.op == 1 ? "alloc" : "free ", b.size, b.caller, b.off,
                   k == i ? "<-- FIRST DIFF" : "");
    }
}
uint8_t* base()      { return g_base; }
uint32_t used()      { return g_meta ? g_meta->bump : 0; }
uint32_t capacity()  { return ARENA_SIZE; }
size_t   live_bytes(){ return g_meta ? g_meta->live_bytes : 0; }

uint32_t save(uint8_t* out, uint32_t cap) {
    if (!g_meta) return 0;
    uint32_t n = g_meta->bump;
    if (n > cap) {
        log_printf("[cpp_arena] !! save OVERFLOW used=%u > cap=%u\n", n, cap);
        return 0;
    }
    memcpy(out, g_base, n);
    return n;
}

void load(const uint8_t* blob, uint32_t len) {
    if (!g_base || len < sizeof(Meta) || len > ARENA_SIZE) return;
    if (((const Meta*)blob)->magic != META_MAGIC) {
        log_printf("[cpp_arena] load: bad magic\n");
        return;
    }
    memcpy(g_base, blob, len);
}

} // namespace cpp_arena
