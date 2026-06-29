#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "snapshot_ring.h"
#include "sq_arena.h"
#include "bullet_arena.h"
#include "cpp_arena.h"
#include "crash_handler.h"   // watchpoint_arm — auto-attribute first divergence
#include "patch_utils.h"     // base_address (vtable -> RVA resolution in the f=15 probe)
#include "log.h"

namespace snapshot_ring {

// Per-session bitset of cpp_arena pages we've already attributed via
// cpp_arena::attribute(). 32k pages × 1 bit = 4 KB. Indexing: page N
// is bit N. Used by the divf logging to fire one [cpp_attr] line per
// page per session (the persistently-divergent pages — 23397, 23396,
// 23424 in the user's repro — would otherwise spam every re-sim).
static uint32_t g_attr_seen[32768 / 32] = {0};
bool divf_attr_seen(uint32_t pg) {
    if (pg >= 32768) return true;
    uint32_t idx = pg >> 5;
    uint32_t bit = 1u << (pg & 31);
    if (g_attr_seen[idx] & bit) return true;
    g_attr_seen[idx] |= bit;
    return false;
}

// Per-session bitset for per-page byte-dump diagnostic. When a cpp_arena
// page diverges between forward and re-sim, log the first 8 dwords that
// differ between the live bytes and the snapshot-saved bytes. The
// bitset rate-limits this to one dump per page so the persistent
// divergent pages (23397 / 23396 / 22A9 / etc) get sampled once each.
static uint32_t g_divword_seen[32768 / 32] = {0};
bool divword_seen(uint32_t pg) {
    if (pg >= 32768) return true;
    uint32_t idx = pg >> 5;
    uint32_t bit = 1u << (pg & 31);
    if (g_divword_seen[idx] & bit) return true;
    g_divword_seen[idx] |= bit;
    return false;
}

// Per-page latch of cpp_arena bytes captured at save time. When a page
// matches a "track this" allowlist (the persistent divergent pages),
// we copy its 4 KB into the latch on every save. On re-sim divergence,
// we compare live bytes against the latch and log the offending dwords.
//
// Allowlist hits per the 2026-05-27 diagnostic: pages 23397, 23396,
// 8873, 10234, 10235, 23424, 23706 — all cpp_arena. Total memory
// footprint = 7 × 4 KB = 28 KB. Trivial.
struct TrackedPage {
    uint32_t pg;
    uint8_t  bytes[4096];
    bool     captured;
};
static TrackedPage g_tracked[] = {
    {23397, {}, false},   // boost::signals2 connection_body
    {23396, {}, false},   // World2D__Init  4-byte flag
    {8873,  {}, false},   // Ew::sTask per-worker stats
    {10234, {}, false},   // boost::log shared_count
    {10235, {}, false},   // boost::log shared_count slot
    {23424, {}, false},   // sub_E0F00? freed neighbour
    {23706, {}, false},   // UDPInnerD 256KB buffer (first page)
};
static constexpr int N_TRACKED =
    sizeof(g_tracked) / sizeof(g_tracked[0]);

TrackedPage* find_tracked(uint32_t pg) {
    for (int i = 0; i < N_TRACKED; ++i)
        if (g_tracked[i].pg == pg) return &g_tracked[i];
    return nullptr;
}
namespace {

static constexpr uint32_t PAGE   = 4096;
static constexpr int      RING   = 16;   // > GekkoConfig::check_distance (8)
static constexpr int      NARENA = 3;    // 0 = sq_arena, 1 = bullet_arena, 2 = cpp_arena

// Per-slot dirty-page delta capacity. A 2D-fighter frame writes far less; the
// cap only guards a pathological frame, which is logged loudly if hit.
static const uint32_t DELTA_CAP[NARENA] = { 8u * 1024 * 1024, 4u * 1024 * 1024,
                                            8u * 1024 * 1024 };
static constexpr uint32_t SMALL_CAP = 4u * 1024 * 1024;

// One dirty-page record in a delta buffer: [page-offset u32][PAGE bytes].
static constexpr uint32_t REC = 4 + PAGE;

struct Arena {
    uint8_t*  base;     // live arena base — the MEM_WRITE_WATCH region
    uint8_t*  mirror;   // full-size shadow == arena state as of g_cur
    uint32_t  size;     // arena reserved size
    uint32_t  npages;
    uint32_t* phash;    // per-page hash, npages entries — tracks live state
};
static Arena  g_ar[NARENA];
static void** g_pgbuf = nullptr;   // GetWriteWatch address scratch

struct Slot {
    int32_t   frame;             // frame held, -1 = empty
    uint32_t  dn[NARENA];        // dirty-page count per arena
    uint8_t*  delta[NARENA];     // dn[a] x REC — PRE-images (frame-1 values)
    uint32_t  sblob_len;
    uint8_t*  sblob;
    // DIAGNOSTIC: per-arena snapshot of the per-page hashes at the slot's
    // first capture of its current frame. On a re-capture (the rollback
    // re-sim hitting the same frame number), compare against the live phash
    // to pin the FRAME and PAGES where the re-sim's output diverges from
    // the forward run's — no arbitrary threshold, every frame is checked.
    uint32_t* phash_snap[NARENA];
    // DIAGNOSTIC: full 4 KB byte shadow of specific known-divergent pages,
    // populated at first capture so a re-capture byte-diffs to show the
    // exact dwords that diverged. Hardcoded list — see TARGET_PAGES.
    uint8_t*  page_snap[8];      // up to 8 targeted pages
};

// Hardcoded list of divergent (arena, byte-offset) pages identified by the
// per-frame phash-snap diagnostic. Targeted byte-level shadow runs only on
// these — tiny (≈ 8 × 4 KB × RING ≈ 384 KB) but produces forward-vs-re-sim
// bytes at the bytes that matter, no big shadow buffer needed.
static const struct { int arena; uint32_t off; } TARGET_PAGES[] = {
    { 0 /*sq*/, 0x9EE000 },
    { 0,        0xBF5000 },
    { 1 /*bt*/, 0x480000 },
    { 1,        0x680000 },
    { 1,        0x740000 },
    { 1,        0x741000 },
    { 1,        0x742000 },
};
static constexpr int N_TARGETS = (int)(sizeof(TARGET_PAGES) / sizeof(TARGET_PAGES[0]));
static Slot    g_ring[RING];
static bool    g_armed = false;
static int64_t g_cur   = -1;       // frame the live arenas currently hold

// 64-bit FNV-1a over one page, 8 bytes/step with two independent accumulators
// to break the multiply dependency chain — this runs once per dirty page in
// the capture/restore hot loop.
static uint32_t hash_page(const uint8_t* p) {
    static constexpr uint64_t P64 = 1099511628211ull;
    uint64_t g0 = 1469598103934665603ull, g1 = 1469598103934665603ull;
    const uint64_t* w = (const uint64_t*)p;
    for (uint32_t i = 0; i < PAGE / 8; i += 2) {   // PAGE/8 = 512 (even)
        g0 = (g0 ^ w[i])     * P64;
        g1 = (g1 ^ w[i + 1]) * P64;
    }
    uint64_t g = g0 ^ g1;
    return (uint32_t)g ^ (uint32_t)(g >> 32);
}

} // namespace

void arm() {
    if (g_armed) return;

    struct Src { uint8_t* base; uint32_t size; };
    Src src[NARENA] = {
        { sq_arena::base(),     sq_arena::capacity()     },
        { bullet_arena::base(), bullet_arena::capacity() },
        { cpp_arena::base(),    cpp_arena::capacity()    },
    };

    uint32_t maxpages = 0;
    for (int a = 0; a < NARENA; ++a) {
        Arena& A = g_ar[a];
        A.base   = src[a].base;
        A.size   = src[a].size;
        A.npages = src[a].size / PAGE;
        if (A.npages > maxpages) maxpages = A.npages;
        // restore()'s `seen` bitmap is sized for a 128 MB arena. A larger
        // arena would overflow it — fail to arm rather than corrupt memory.
        if (src[a].size > 128u * 1024 * 1024) {
            log_printf("[snapshot_ring] !! arm: arena %d too large (%u MB) — "
                       "raise `seen` bitmap size\n", a, src[a].size / (1024*1024));
            return;
        }
        if (!A.base) {
            log_printf("[snapshot_ring] !! arm: arena %d not installed\n", a);
            return;
        }
        A.mirror = (uint8_t*)VirtualAlloc(nullptr, A.size,
                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        A.phash  = (uint32_t*)VirtualAlloc(nullptr, A.npages * 4,
                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!A.mirror || !A.phash) {
            log_printf("[snapshot_ring] !! arm: alloc failed (arena %d)\n", a);
            return;
        }
        // Baseline: mirror = current arena; per-page hashes; clear the watch.
        memcpy(A.mirror, A.base, A.size);
        for (uint32_t pg = 0; pg < A.npages; ++pg)
            A.phash[pg] = hash_page(A.base + pg * PAGE);
        ResetWriteWatch(A.base, A.size);
    }

    g_pgbuf = (void**)VirtualAlloc(nullptr, (maxpages + 16) * sizeof(void*),
                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_pgbuf) {
        log_printf("[snapshot_ring] !! arm: pgbuf alloc failed\n");
        return;
    }

    for (int s = 0; s < RING; ++s) {
        Slot& S = g_ring[s];
        S.frame = -1;
        S.sblob = (uint8_t*)VirtualAlloc(nullptr, SMALL_CAP,
                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        bool delta_ok = true;
        for (int a = 0; a < NARENA; ++a) {
            S.delta[a] = (uint8_t*)VirtualAlloc(nullptr, DELTA_CAP[a],
                             MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!S.delta[a]) delta_ok = false;
            // DIAGNOSTIC: phash snapshot, npages * 4. Cheap.
            S.phash_snap[a] = (uint32_t*)VirtualAlloc(nullptr,
                                  g_ar[a].npages * 4u,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!S.phash_snap[a]) delta_ok = false;
        }
        // DIAGNOSTIC: per-target-page byte shadows (4 KB each).
        for (int i = 0; i < N_TARGETS; ++i) {
            S.page_snap[i] = (uint8_t*)VirtualAlloc(nullptr, PAGE,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!S.page_snap[i]) delta_ok = false;
        }
        if (!S.sblob || !delta_ok) {
            log_printf("[snapshot_ring] !! arm: ring slot %d alloc failed\n", s);
            return;
        }
    }

    g_cur   = -1;
    g_armed = true;
    log_printf("[snapshot_ring] armed: sq=%uMB bullet=%uMB cpp=%uMB ring=%d\n",
               g_ar[0].size / (1024 * 1024), g_ar[1].size / (1024 * 1024),
               g_ar[2].size / (1024 * 1024), RING);
}

bool armed() { return g_armed; }

namespace {
// Offset of the size-classed arena's `bump` (high-water) field within its
// Meta header — sq_arena Meta.bump@0, bullet_arena Meta.bump@4,
// cpp_arena Meta.bump@4 (Meta = {magic, bump, ...}). fold_checksum hashes
// only [base, base+bump): the dead space beyond the high-water is not part
// of the game state and including it makes the checksum disagree with a
// fresh full hash of the live arena (a false-positive desync).
static const uint32_t BUMP_OFF[NARENA] = { 0, 4, 4 };

// Full-state desync checksum: fold every in-use page hash (cheap — maintained
// incrementally) plus the small blob.
//
// The small blob is hashed 8 bytes at a time with two independent 64-bit FNV
// accumulators. A byte-wise hash of ~1 MB is a 1-million-long serial multiply
// chain (~3.6 ms — measured); striding by 8 and splitting the dependency
// chain in two cuts it ~16x.
// Arena 2 (cpp_arena) holds render-signal state — DrawCommandSlot objects and
// the boost::signals2 grouped-signal connection-list nodes bound to them. That
// is RENDER output, not simulation state, and the rollback re-sim is headless by
// design (it never renders), so cpp_arena legitimately differs forward-vs-re-sim
// (and, in real netcode, between a peer that rolled back and one that didn't).
// It is still dirty-page captured + restored below for visual correctness, but it
// is EXCLUDED from the desync checksum — folding it in produces false desyncs.
// The simulation is fully covered by arena 0 (sq_arena/Squirrel VM), arena 1
// (bullet_arena/LiquidFun) and the small blob (battle pools / engine .data /
// input). See ROLLBACK_NETCODE_PLAN.md "cpp_arena render divergence".
static constexpr int CPP_ARENA = 2;

static uint32_t fold_checksum(const uint8_t* sblob, uint32_t sblob_len) {
    uint32_t h = 2166136261u;
    for (int a = 0; a < NARENA; ++a) {
        if (a == CPP_ARENA) continue;   // render state — not part of the sim checksum
        uint32_t bump = *(const uint32_t*)(g_ar[a].base + BUMP_OFF[a]);
        uint32_t upg  = (bump + PAGE - 1) / PAGE;
        if (upg > g_ar[a].npages) upg = g_ar[a].npages;
        for (uint32_t pg = 0; pg < upg; ++pg) {
            h ^= g_ar[a].phash[pg];
            h *= 16777619u;
        }
    }
    static constexpr uint64_t P64 = 1099511628211ull;
    uint64_t g0 = 1469598103934665603ull, g1 = 1469598103934665603ull;
    uint32_t nw = sblob_len >> 3;
    const uint64_t* w = (const uint64_t*)sblob;
    uint32_t i = 0;
    for (; i + 1 < nw; i += 2) {
        g0 = (g0 ^ w[i])     * P64;
        g1 = (g1 ^ w[i + 1]) * P64;
    }
    for (; i < nw; ++i) g0 = (g0 ^ w[i]) * P64;
    for (uint32_t b = nw << 3; b < sblob_len; ++b)
        g0 = (g0 ^ sblob[b]) * P64;
    uint64_t g = g0 ^ g1;
    h ^= (uint32_t)g;        h *= 16777619u;
    h ^= (uint32_t)(g >> 32); h *= 16777619u;
    return h;
}
} // namespace

uint32_t capture(uint32_t frame, const uint8_t* sblob, uint32_t sblob_len) {
    if (!g_armed) return 0;
    Slot& S = g_ring[frame % RING];
    // Detect "the slot already holds this frame" BEFORE we overwrite S.frame
    // below — this is how the per-frame phash-snap diagnostic distinguishes
    // a re-sim re-capture from a fresh forward save of frame N.
    const bool re_capture_diag = (S.frame == (int32_t)frame);
    S.frame = (int32_t)frame;

    LARGE_INTEGER pt0; QueryPerformanceCounter(&pt0);
    uint64_t t_ww = 0, t_dirty = 0;
    for (int a = 0; a < NARENA; ++a) {
        Arena& A = g_ar[a];
        ULONG_PTR count = A.npages;
        ULONG     gran  = 0;
        LARGE_INTEGER w0; QueryPerformanceCounter(&w0);
        UINT rc = GetWriteWatch(WRITE_WATCH_FLAG_RESET, A.base, A.size,
                                g_pgbuf, &count, &gran);
        LARGE_INTEGER w1; QueryPerformanceCounter(&w1);
        t_ww += (uint64_t)(w1.QuadPart - w0.QuadPart);
        if (rc != 0) {
            log_printf("[snapshot_ring] !! GetWriteWatch failed arena=%d\n", a);
            count = 0;
        }
        LARGE_INTEGER d0; QueryPerformanceCounter(&d0);
        uint8_t* dp   = S.delta[a];
        uint8_t* dend = dp + DELTA_CAP[a];
        uint32_t n    = 0;
        for (ULONG_PTR i = 0; i < count; ++i) {
            uint32_t off = (uint32_t)((uint8_t*)g_pgbuf[i] - A.base);
            if (off + PAGE > A.size) continue;
            if (dp + REC > dend) {
                log_printf("[snapshot_ring] !! delta overflow arena=%d f=%u "
                           "(%u pages) — raise DELTA_CAP\n", a, frame,
                           (uint32_t)count);
                break;
            }
            *(uint32_t*)dp = off;                          // page offset
            memcpy(dp + 4, A.mirror + off, PAGE);          // PRE-image (frame-1)
            dp += REC;
            ++n;
            memcpy(A.mirror + off, A.base + off, PAGE);     // sync mirror -> frame
            A.phash[off / PAGE] = hash_page(A.base + off);  // hash -> frame
            // Tracked-page latch (cpp_arena only). On FORWARD save the
            // current bytes become the baseline for the next re-sim's
            // dword-level diff log. The gate on !is_resim() avoids the
            // re-sim overwriting the latch with its own (divergent)
            // bytes — we want the latch to hold the forward-sim
            // ground truth so divword can show what changed.
            if (a == 2 && !cpp_arena::is_resim()) {
                if (TrackedPage* tp = find_tracked(off / PAGE)) {
                    memcpy(tp->bytes, A.base + off, PAGE);
                    tp->captured = true;
                }
            }
        }
        S.dn[a] = n;
        LARGE_INTEGER d1; QueryPerformanceCounter(&d1);
        t_dirty += (uint64_t)(d1.QuadPart - d0.QuadPart);
    }

    if (sblob_len > SMALL_CAP) {
        log_printf("[snapshot_ring] !! sblob blob too big (%u > %u)\n",
                   sblob_len, SMALL_CAP);
        sblob_len = SMALL_CAP;
    }
    memcpy(S.sblob, sblob, sblob_len);
    S.sblob_len = sblob_len;

    LARGE_INTEGER pt1; QueryPerformanceCounter(&pt1);
    uint32_t cs = fold_checksum(sblob, sblob_len);
    LARGE_INTEGER pt2; QueryPerformanceCounter(&pt2);

    // DIAGNOSTIC: per-arena page-hash fold, so a desync can be pinned to the
    // exact arena (sq / bullet / cpp) that diverged rather than one opaque
    // combined checksum.
    {
        uint32_t hc[NARENA], bc[NARENA];
        for (int a = 0; a < NARENA; ++a) {
            uint32_t bump = *(const uint32_t*)(g_ar[a].base + BUMP_OFF[a]);
            bc[a] = bump;
            uint32_t upg  = (bump + PAGE - 1) / PAGE;
            if (upg > g_ar[a].npages) upg = g_ar[a].npages;
            uint32_t h = 2166136261u;
            for (uint32_t pg = 0; pg < upg; ++pg) { h ^= g_ar[a].phash[pg]; h *= 16777619u; }
            hc[a] = h;
        }
        log_printf("[comp] f=%u sq=%08x/%u bt=%08x/%u cpp=%08x/%u\n",
                   frame, hc[0], bc[0], hc[1], bc[1], hc[2], bc[2]);
    }

    // DIAGNOSTIC: every-frame divergence detection via per-page hash. At the
    // first capture of frame N (the forward save), snapshot live phash[]
    // into the slot. On any later re-capture of N (a rollback re-sim's
    // save), compare current phash[] against the snapshot — any page whose
    // hash differs is where the re-sim's frame-N output deviates from the
    // forward run's. No arbitrary frame threshold; the EARLIEST divergent
    // frame surfaces on its first re-capture.
    {
        Slot& S = g_ring[frame % RING];
        if (re_capture_diag) {
            static const char* names[NARENA] = { "sq", "bt", "cpp" };
            static bool first_dump_done = false;   // dump page bytes ONCE
            (void)first_dump_done;                 // kept for binary stability
            int totalp = 0;
            for (int a = 0; a < NARENA; ++a) {
                // cpp_arena (arena 2) is intentionally excluded from the desync
                // checksum (render state — see fold_checksum). It diverges every
                // frame by design, so do NOT diagnose/attribute it: the attribute
                // walk follows pointers in the divergent arena and can fault, and
                // it floods the log (the ~10 fps + the f≈27 crash). Only sq/bt
                // divergence is a real desync worth surfacing.
                if (a == CPP_ARENA) continue;
                int hits = 0;
                for (uint32_t pg = 0; pg < g_ar[a].npages && hits < 6; ++pg) {
                    if (S.phash_snap[a][pg] != g_ar[a].phash[pg]) {
                        log_printf("[divf] %s f=%u pg=%u off=0x%X "
                                   "fwd_hash=%08x now=%08x\n",
                                   names[a], frame, pg, pg * PAGE,
                                   S.phash_snap[a][pg], g_ar[a].phash[pg]);
                        // Attribute every divergent cpp_arena page so the
                        // persistent divergence sources (the late-pre-gate
                        // singletons that always flip the same two-value
                        // hash pattern across re-sim) get their allocating
                        // caller named. Rate-limited per-page-per-session
                        // via a small bitset so the log doesn't spam the
                        // same RVA every frame for the same offset.
                        if (a == 2) {
                            extern bool divf_attr_seen(uint32_t);
                            if (!divf_attr_seen(pg)) {
                                cpp_arena::attribute(pg * PAGE);
                            }
                            extern bool divword_seen(uint32_t);
                            // Per-page dword diff against the save-time
                            // latch — only for tracked pages, only once
                            // per session, so it gives us the bytes that
                            // are flipping for the persistent divergent
                            // sources (connection_body refcount, etc).
                            TrackedPage* tp = find_tracked(pg);
                            if (tp && tp->captured && !divword_seen(pg)) {
                                const uint32_t* live =
                                    (const uint32_t*)(g_ar[a].base + pg * PAGE);
                                const uint32_t* saved =
                                    (const uint32_t*)tp->bytes;
                                int diffs = 0;
                                for (uint32_t i = 0;
                                     i < PAGE / 4 && diffs < 16; ++i) {
                                    if (live[i] != saved[i]) {
                                        log_printf("[divword] pg=%u dw=%u "
                                                   "off=0x%X saved=%08x "
                                                   "live=%08x\n",
                                                   pg, i, pg * PAGE + i * 4,
                                                   saved[i], live[i]);
                                        // Attribute the divergent dword's OWN
                                        // block, and — when the flipping values
                                        // are cpp_arena pointers — what each
                                        // side points at (the per-frame object
                                        // whose address swaps).
                                        if (diffs == 0) {
                                            uint32_t b = (uint32_t)(uintptr_t)g_ar[a].base;
                                            uint32_t c = g_ar[a].size;
                                            cpp_arena::attribute(pg * PAGE + i * 4);
                                            if (saved[i] > b && saved[i] < b + c)
                                                cpp_arena::attribute(saved[i] - b);
                                            if (live[i] > b && live[i] < b + c)
                                                cpp_arena::attribute(live[i] - b);
                                        }
                                        ++diffs;
                                    }
                                }
                                if (diffs) {
                                    log_printf("[divword] pg=%u: %d dword(s) "
                                               "differ in first 4KB\n",
                                               pg, diffs);
                                }
                            }
                        }
                        ++hits;
                    }
                }
                totalp += hits;
            }
            if (totalp) {
                log_printf("[divf] f=%u — %d divergent pages across arenas\n",
                           frame, totalp);
                first_dump_done = true;
            }
            // Targeted byte-diff on the hardcoded TARGET_PAGES — produces
            // FORWARD vs RE-SIM dwords for the pages we already know diverge.
            // (DR0 arm moved out — see frame=0 / forward-save hardcode below.
            // Arming on a re-sim's divbyte detection is TOO LATE: the writer
            // we want fires in forward's advance(15), which finishes before
            // the first re-sim of f=15 ever reaches divbyte. By the time we
            // arm, the writer-of-3 in forward has long since left and only
            // re-sims run — and re-sims may not even write the dword at all
            // (as observed: 0 game writes after a re-sim-side arm, the
            // divergence is forward writing while re-sims don't.)
            for (int i = 0; i < N_TARGETS; ++i) {
                int       a   = TARGET_PAGES[i].arena;
                uint32_t  off = TARGET_PAGES[i].off;
                if (off + PAGE > g_ar[a].size) continue;
                const uint8_t* shadow = S.page_snap[i];
                const uint8_t* live   = g_ar[a].base + off;
                int dh = 0;
                for (uint32_t k = 0; k + 4 <= PAGE && dh < 5; k += 4) {
                    uint32_t sv = *(const uint32_t*)(shadow + k);
                    uint32_t lv = *(const uint32_t*)(live   + k);
                    if (sv != lv) {
                        log_printf("[divbyte] %s f=%u off=0x%X+0x%X fwd=%08x now=%08x\n",
                                   names[a], frame, off, k, sv, lv);
                        ++dh;
                    }
                }
            }
            // --- f=15 swapped-object IDENTITY probe ------------------------
            // Root: two adjacent sq objects A@+0x550 / B@+0x5D0 on the sq
            // 0x9EE000 page swap their refcounts. Dump each object header —
            // forward (shadow) vs re-sim (live) — with the leading vtable
            // dword resolved to an RVA so the C++ class can be named offline,
            // plus the parent reference region around sq 0xBF5328.
            {
                static int objq = 0;   // f=15 sq-hunt object dump DISABLED (sq clean)
                int sqi = -1, bpi = -1;
                for (int i = 0; i < N_TARGETS; ++i) {
                    if (TARGET_PAGES[i].arena == 0 && TARGET_PAGES[i].off == 0x9EE000) sqi = i;
                    if (TARGET_PAGES[i].arena == 0 && TARGET_PAGES[i].off == 0xBF5000) bpi = i;
                }
                if (objq > 0 && sqi >= 0) {
                    --objq;
                    const uint8_t* sh = S.page_snap[sqi];
                    const uint8_t* lv = g_ar[0].base + 0x9EE000;
                    static const uint32_t objoff[2] = { 0x550, 0x5D0 };
                    for (int o = 0; o < 2; ++o) {
                        const uint32_t* f = (const uint32_t*)(sh + objoff[o]);
                        const uint32_t* r = (const uint32_t*)(lv + objoff[o]);
                        log_printf("[objid] f=%u %s@sq+0x%X vtbl fwd=%08X(rva %08X) "
                                   "resim=%08X(rva %08X)\n", frame, o == 0 ? "A" : "B",
                                   0x9EE000 + objoff[o], f[0],
                                   (uint32_t)(f[0] - (uint32_t)base_address), r[0],
                                   (uint32_t)(r[0] - (uint32_t)base_address));
                        for (int d = 0; d < 14; ++d)
                            log_printf("[objid]   +0x%02X fwd=%08X resim=%08X%s\n",
                                       d * 4, f[d], r[d], f[d] != r[d] ? "  <-DIFF" : "");
                        // Chase closure._function (+0x20) -> SQFunctionProto;
                        // print its _name (FP+0x24 -> SQString+0x1C chars) and
                        // _sourcename (FP+0x1C). All derefs range-checked to the
                        // sq arena so a wrong offset can't fault.
                        uint32_t lo = (uint32_t)(uintptr_t)g_ar[0].base;
                        uint32_t hi = lo + g_ar[0].size;
                        uint32_t fp = r[8];                       // _function
                        if (fp >= lo && fp + 0x28 < hi) {
                            const uint32_t* fpw = (const uint32_t*)(uintptr_t)fp;
                            uint32_t nm = fpw[9], sn = fpw[7];    // _name / _sourcename values
                            const char* nms = (nm >= lo && nm + 0x40 < hi) ? (const char*)(uintptr_t)(nm + 0x1C) : "?";
                            const char* sns = (sn >= lo && sn + 0x40 < hi) ? (const char*)(uintptr_t)(sn + 0x1C) : "?";
                            log_printf("[objid]   %s _function rva=%08X  name='%.40s'  src='%.40s'\n",
                                       o == 0 ? "A" : "B", fp - lo, nms, sns);
                        }
                    }
                    if (bpi >= 0) {
                        const uint32_t* pf = (const uint32_t*)(S.page_snap[bpi] + 0x300);
                        const uint32_t* pr = (const uint32_t*)(g_ar[0].base + 0xBF5000 + 0x300);
                        log_printf("[objid] f=%u parent region sq+0xBF5300:\n", frame);
                        for (int d = 0; d < 18; ++d)
                            log_printf("[objid]   +0x%03X fwd=%08X resim=%08X%s\n",
                                       0x300 + d * 4, pf[d], pr[d],
                                       pf[d] != pr[d] ? "  <-DIFF" : "");
                    }
                }
            }
            // Refresh ALL snapshots to the new (re-sim) value — so any
            // subsequent re-capture detects only NEW divergences.
            for (int a = 0; a < NARENA; ++a)
                memcpy(S.phash_snap[a], g_ar[a].phash, g_ar[a].npages * 4);
            for (int i = 0; i < N_TARGETS; ++i) {
                int a = TARGET_PAGES[i].arena; uint32_t off = TARGET_PAGES[i].off;
                if (off + PAGE <= g_ar[a].size)
                    memcpy(S.page_snap[i], g_ar[a].base + off, PAGE);
            }
        } else {
            // First capture (forward) of this frame in the current ring
            // window — snapshot the per-page hashes AND the target page bytes
            // for later comparison.
            for (int a = 0; a < NARENA; ++a)
                memcpy(S.phash_snap[a], g_ar[a].phash, g_ar[a].npages * 4);
            for (int i = 0; i < N_TARGETS; ++i) {
                int a = TARGET_PAGES[i].arena; uint32_t off = TARGET_PAGES[i].off;
                if (off + PAGE <= g_ar[a].size)
                    memcpy(S.page_snap[i], g_ar[a].base + off, PAGE);
            }
            // ALSO arm DR0 on the known-divergent sq dword on the FORWARD
            // save of frame=14. Why here: the writer of the 3↔2 swap fires
            // in forward's advance(15), which runs after save(14) and
            // before save(15). Arming at save(14) means DR0 catches that
            // forward writer when it fires. Arming at save(15) (the first
            // divbyte detection) is too late — forward has already
            // written; only re-sims run after, and re-sims may not write
            // the dword at all.
            //
            // Address is sq_arena's first target page + the dword that
            // divbyte consistently shows diverging — see project_squiroll_
            // tf4_mspace.md. One-shot — never re-armed or disarmed.
            // DR0 now owned by input_global_sync (InputGlobal+4 writer hunt);
            // the sq 0x9EE554 dword is understood (a closure refcount).
            static bool g_sq_dr0_armed = true;
            if (!g_sq_dr0_armed && frame == 14 && g_ar[0].base) {
                void* sq_target = g_ar[0].base + 0x9EE554;
                crash_handler::watchpoint_arm(sq_target);
                log_printf("[snapshot_ring] DR0 -> sq 0x%X (arena=%p + "
                           "0x9EE554 = %p) — will fire on the forward "
                           "advance(15) writer of the 3<->2 swap dword\n",
                           0x9EE554, g_ar[0].base, sq_target);
                g_sq_dr0_armed = true;
            }
        }
    }

    // Periodic report — dirty pages + where capture's time goes.
    static uint32_t prc = 0, psq = 0, pbt = 0;
    static uint64_t a_ww = 0, a_dirty = 0, a_rest = 0, a_fold = 0;
    psq    += S.dn[0];
    pbt    += S.dn[1];
    a_ww   += t_ww;
    a_dirty += t_dirty;
    a_rest += (uint64_t)(pt1.QuadPart - pt0.QuadPart) - t_ww - t_dirty;
    a_fold += (uint64_t)(pt2.QuadPart - pt1.QuadPart);
    if (++prc >= 240) {
        LARGE_INTEGER fr; QueryPerformanceFrequency(&fr);
        uint64_t hz = (uint64_t)fr.QuadPart;
        auto us = [&](uint64_t t) -> uint32_t {
            return (uint32_t)(t * 1000000ull / hz / prc);
        };
        log_printf("[snapshot_ring] dirty/cap sq=%u bt=%u (%uKB)  us: "
                   "getww=%u dirtycopy=%u sblobcopy=%u fold=%u\n",
                   psq / prc, pbt / prc, (psq + pbt) / prc * 4,
                   us(a_ww), us(a_dirty), us(a_rest), us(a_fold));
        prc = psq = pbt = 0;
        a_ww = a_dirty = a_rest = a_fold = 0;
    }

    g_cur = (int64_t)frame;
    return cs;
}

const uint8_t* restore(uint32_t frame, uint32_t* sblob_len) {
    *sblob_len = 0;
    if (!g_armed) return nullptr;
    int64_t target = (int64_t)frame;
    if (target > g_cur) {
        log_printf("[snapshot_ring] !! restore future f=%u cur=%d\n",
                   frame, (int)g_cur);
        return nullptr;
    }

    // The whole reverse chain ring[target+1 .. g_cur] must be present.
    for (int64_t f = g_cur; f > target; --f) {
        Slot& S = g_ring[(uint32_t)(f % RING)];
        if (S.frame != (int32_t)f) {
            log_printf("[snapshot_ring] !! restore gap: want f=%d got slot "
                       "frame=%d\n", (int)f, S.frame);
            return nullptr;
        }
    }

    // Coalesced reverse-apply. A hot page (the VM operand stack) is dirtied
    // every frame, so it appears in every rolled-back frame's delta — the
    // naive newest-first apply would copy it once per frame. Instead walk
    // OLDEST->newest and restore each page exactly once, from the first
    // (oldest) delta that holds it: that pre-image is the page's value at
    // frame target-... = frame `target` (it was not dirtied in between).
    // 1 bit/page; sized for the largest arena — cpp_arena, 128 MB / 4 KB.
    static uint8_t seen[NARENA][(128u * 1024 * 1024 / PAGE) / 8];
    memset(seen, 0, sizeof(seen));
    for (int64_t f = target + 1; f <= g_cur; ++f) {
        Slot& S = g_ring[(uint32_t)(f % RING)];
        for (int a = 0; a < NARENA; ++a) {
            Arena&         A  = g_ar[a];
            const uint8_t* dp = S.delta[a];
            uint8_t*       sn = seen[a];
            for (uint32_t i = 0; i < S.dn[a]; ++i) {
                uint32_t       off = *(const uint32_t*)dp;
                const uint8_t* pre = dp + 4;
                uint32_t       pg  = off / PAGE;
                if (!(sn[pg >> 3] & (1u << (pg & 7)))) {
                    sn[pg >> 3] |= (uint8_t)(1u << (pg & 7));
                    memcpy(A.base   + off, pre, PAGE);   // live  -> frame target
                    memcpy(A.mirror + off, pre, PAGE);   // mirror tracks live
                    A.phash[pg] = hash_page(pre);
                }
                dp += REC;
            }
        }
    }
    g_cur = target;

    // Discard the write-watch entries our own restore writes just produced,
    // so the next capture sees only the re-sim advance's dirty pages.
    for (int a = 0; a < NARENA; ++a)
        ResetWriteWatch(g_ar[a].base, g_ar[a].size);

    Slot& T = g_ring[frame % RING];
    if (T.frame != (int32_t)frame) {
        log_printf("[snapshot_ring] !! restore: no sblob blob for f=%u\n", frame);
        return nullptr;
    }
    *sblob_len = T.sblob_len;
    return T.sblob;
}

} // namespace snapshot_ring
