#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "snapshot_ring.h"
#include "sq_arena.h"
#include "bullet_arena.h"
#include "cpp_arena.h"
#include "log.h"

namespace snapshot_ring {
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
};
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
static uint32_t fold_checksum(const uint8_t* sblob, uint32_t sblob_len) {
    uint32_t h = 2166136261u;
    for (int a = 0; a < NARENA; ++a) {
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

    // DIAGNOSTIC: cpp_arena re-sim divergence locator. Snapshot cpp_arena
    // once at an early frame; when a rollback re-sim re-captures that same
    // frame, dump the dwords that differ — their values reveal which heap
    // the divergent pointer targets (cpp_arena / sq / bullet / mspace / the
    // real Win32 heap).
    {
        Arena& C = g_ar[2];
        static uint8_t* shadow       = nullptr;
        static int32_t  shadow_frame = -1;
        static bool     diff_done    = false;
        if (!diff_done) {
            if (shadow_frame < 0) {
                if (!shadow)
                    shadow = (uint8_t*)VirtualAlloc(nullptr, C.size,
                                 MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                // Shadow the earliest re-simmable frame so the diff catches
                // the divergence at its ORIGIN, not several frames of cascade
                // later.
                if (shadow && frame >= 2) {
                    memcpy(shadow, C.base, C.size);
                    shadow_frame = (int32_t)frame;
                    log_printf("[cppdiff] shadow of cpp_arena taken at f=%u\n",
                               frame);
                }
            } else if ((int32_t)frame == shadow_frame) {
                const uint32_t* a = (const uint32_t*)shadow;
                const uint32_t* b = (const uint32_t*)C.base;
                uint32_t bump = *(const uint32_t*)(C.base + BUMP_OFF[2]);
                int hits = 0;
                for (uint32_t o = 0; o < bump && hits < 12; o += 4) {
                    uint32_t i = o >> 2;
                    if (a[i] != b[i]) {
                        log_printf("[cppdiff] f=%u off=0x%X fwd=%08x now=%08x\n",
                                   frame, o, a[i], b[i]);
                        cpp_arena::attribute(o);   // -> owning block + caller RVA
                        ++hits;
                    }
                }
                log_printf("[cppdiff] done — %d differing dwords "
                           "(cpp_arena base=%p)\n", hits, (void*)C.base);
                diff_done = true;
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
