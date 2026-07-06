#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "snapshot_ring.h"
#include "sq_arena.h"
#include "bullet_arena.h"
#include "cpp_arena.h"
#include "tf4_arena.h"       // arenas 3/4 — the two engine-private TF4 mspace pools
#include "battle_pools.h"    // nochecksum_spans — render-ptr fields masked from the fold
#include "sync_pin.h"        // lock pinning across restore (locks never roll back)
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
    {23739, {}, false},   // RESIDUAL (post FPS-fix + HUD-geom exclude) — the combo's ptr target
    {23396, {}, false},   // combo digit geometry (now excluded from diagnostic)
    {23404, {}, false},   // FPS digit geometry (fixed by GetFPS patch)
    {23397, {}, false},   // boost::signals2 connection_body
    {8222,  {}, false},   // residual
    {10234, {}, false},   // boost::log shared_count
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
static constexpr int      NARENA = 5;    // 0 = sq_arena, 1 = bullet_arena, 2 = cpp_arena,
                                         // 3 = tf4 mspace A (~128MB), 4 = tf4 mspace B (~32MB)
// Arenas [0, NDIAG) carry the bump/phash-fold/divf diagnostic machinery. The
// tf4 pools (3,4) are dlmalloc mspaces with no bump field and legitimately
// differ fwd-vs-resim (signals2 control blocks / Sqrat holders), so they are
// capture/restore-ONLY — excluded from every checksum + divergence diagnostic.
static constexpr int      NDIAG  = 3;

// Largest arena an arm() can register. tf4 region A is 128 MB + 64 KB (the
// allocation-granularity round-up in tf4_mspace_create: (gran + 0x8000377) &
// ~(gran-1) with gran = 0x10000 -> 0x8010000, 32784 pages). Sizes the arm()
// guard AND restore()'s per-arena `seen` bitmap, so both must exceed it.
static constexpr uint32_t MAX_ARENA_BYTES = 144u * 1024 * 1024;   // headroom over 128MB+64KB

// Per-slot dirty-page delta capacity. A 2D-fighter frame writes far less; the
// cap only guards a pathological frame, which is logged loudly if hit.
// tf4 A (arena 3) dirties ~1 page/frame -> 8 MB is ample. tf4 B (arena 4, the
// 32 MB secondary pool) is written ~3550 pages (~14.5 MB) EVERY frame by a
// background (audio/streaming decoder) thread that lives in the TF4 mspace, so
// its cap is raised to 20 MB (~5100 records) to capture the whole region B
// churn without the delta `break` that would leave it half-captured.
static const uint32_t DELTA_CAP[NARENA] = { 8u * 1024 * 1024, 4u * 1024 * 1024,
                                            8u * 1024 * 1024, 8u * 1024 * 1024,
                                            20u * 1024 * 1024 };
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
// FAST MODE: the per-frame divergence diagnostics ([cspart]/[comp]/[divf], the
// cppb full-arena byte-hash of ~100MB, the all-page phash recompute) are pure
// tracing and are what pin the sim to ~5fps. OFF by default. The desync
// CHECKSUM (fold_checksum over incrementally-maintained phash) + capture/restore
// are UNAFFECTED — desync is still detected and the abort still dumps.
// SQUIROLL_DIAG=1 re-enables the heavy tracing for a hunt.
static bool    g_diag = false;
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

    // tf4 arenas 3/4 are optional — registered only if tf4_arena intercepted
    // both pools (early enough, not disabled). Otherwise size 0 -> every loop
    // skips them gracefully and rollback runs on the three core arenas as before.
    //
    // Region B ('stl', 32MB) is NOT registered by default: measured ~3550 dirty
    // pages (~14.5MB) EVERY frame from a background audio/streaming thread —
    // wholesale snapshot/restore races that thread (torn dlmalloc state ->
    // intermittent ~f=12 abort). Its sim-relevant pool BLOCKS stay covered by
    // the targeted boostpool capture. SQUIROLL_TF4B=1 re-enables for experiments.
    // Region A ('system', 128MB) is ~1 dirty page/frame in battle — capture it.
    const bool tf4_on  = tf4_arena::ready();
    const bool tf4b_on = tf4_on && getenv("SQUIROLL_TF4B");
    struct Src { uint8_t* base; uint32_t size; };
    Src src[NARENA] = {
        { sq_arena::base(),     sq_arena::capacity()     },
        { bullet_arena::base(), bullet_arena::capacity() },
        { cpp_arena::base(),    cpp_arena::capacity()    },
        { tf4_on  ? tf4_arena::base(0) : nullptr, tf4_on  ? tf4_arena::size(0) : 0 },
        { tf4b_on ? tf4_arena::base(1) : nullptr, tf4b_on ? tf4_arena::size(1) : 0 },
    };

    uint32_t maxpages = 0;
    for (int a = 0; a < NARENA; ++a) {
        Arena& A = g_ar[a];
        A.base   = src[a].base;
        A.size   = src[a].size;
        A.npages = src[a].size / PAGE;
        // Zero-size arena (e.g. tf4 pools unavailable): skip it entirely. All
        // per-arena loops (capture/restore/report) tolerate size==0 and no-op.
        if (A.size == 0) {
            A.mirror = nullptr;
            A.phash  = nullptr;
            continue;
        }
        if (A.npages > maxpages) maxpages = A.npages;
        // restore()'s `seen` bitmap is sized for MAX_ARENA_BYTES. A larger
        // arena would overflow it — fail to arm rather than corrupt memory.
        if (src[a].size > MAX_ARENA_BYTES) {
            log_printf("[snapshot_ring] !! arm: arena %d too large (%u MB) — "
                       "raise MAX_ARENA_BYTES / `seen` bitmap size\n",
                       a, src[a].size / (1024*1024));
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
            // DIAGNOSTIC: phash snapshot, npages * 4. Cheap. Skip zero-size
            // arenas (VirtualAlloc(0) fails) — their phash_snap is never read
            // (divf/diagnostics run only for arenas [0, NDIAG)).
            if (g_ar[a].npages) {
                S.phash_snap[a] = (uint32_t*)VirtualAlloc(nullptr,
                                      g_ar[a].npages * 4u,
                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (!S.phash_snap[a]) delta_ok = false;
            } else {
                S.phash_snap[a] = nullptr;
            }
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
    g_diag  = (getenv("SQUIROLL_DIAG") != nullptr);
    log_printf("[snapshot_ring] armed (diag=%d): sq=%uMB bullet=%uMB cpp=%uMB "
               "tf4A=%uMB tf4B=%uMB ring=%d (tf4 %s)\n",
               (int)g_diag,
               g_ar[0].size / (1024 * 1024), g_ar[1].size / (1024 * 1024),
               g_ar[2].size / (1024 * 1024), g_ar[3].size / (1024 * 1024),
               g_ar[4].size / (1024 * 1024), RING,
               tf4_on ? "ON" : "OFF");
}

bool armed() { return g_armed; }
bool diag_on() { return g_diag; }

namespace {
// Offset of the size-classed arena's `bump` (high-water) field within its
// Meta header — sq_arena Meta.bump@0, bullet_arena Meta.bump@4,
// cpp_arena Meta.bump@4 (Meta = {magic, bump, ...}). fold_checksum hashes
// only [base, base+bump): the dead space beyond the high-water is not part
// of the game state and including it makes the checksum disagree with a
// fresh full hash of the live arena (a false-positive desync).
// tf4 entries (3,4) are unused — the tf4 pools are dlmalloc mspaces with no
// bump field and are excluded from every checksum/diagnostic (see NDIAG).
static const uint32_t BUMP_OFF[NARENA] = { 0, 4, 4, 0, 0 };

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

// Cross-peer diagnosis: the last fold's per-component sub-hashes. Comparing
// these between two peers at the same gekko frame says WHICH component
// (sq_arena / bullet_arena / small blob) diverged, instead of just "the
// combined checksum differs". Written on every fold_checksum call.
static uint32_t g_sub_sq = 0, g_sub_bt = 0, g_sub_sblob = 0;

static uint32_t fold_checksum(const uint8_t* sblob, uint32_t sblob_len) {
    uint32_t h = 2166136261u;
    uint32_t sub[2] = { 2166136261u, 2166136261u };  // per-arena (sq, bullet)
    for (int a = 0; a < NARENA; ++a) {
        // Skip cpp (render state) AND the tf4 pools (3,4): the sim checksum is
        // sq(0)+bullet(1)+sblob only. cpp/tf4 legitimately differ fwd-vs-resim.
        if (a >= CPP_ARENA) continue;
        uint32_t bump = *(const uint32_t*)(g_ar[a].base + BUMP_OFF[a]);
        uint32_t upg  = (bump + PAGE - 1) / PAGE;
        if (upg > g_ar[a].npages) upg = g_ar[a].npages;
        for (uint32_t pg = 0; pg < upg; ++pg) {
            h ^= g_ar[a].phash[pg];
            h *= 16777619u;
            sub[a] ^= g_ar[a].phash[pg];
            sub[a] *= 16777619u;
        }
    }
    g_sub_sq = sub[0];
    g_sub_bt = sub[1];
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
    g_sub_sblob = (uint32_t)g ^ (uint32_t)(g >> 32);
    h ^= (uint32_t)g;        h *= 16777619u;
    h ^= (uint32_t)(g >> 32); h *= 16777619u;
    return h;
}
} // namespace

// Cross-peer sub-checksum accessor (see g_sub_* above).
void last_subchecksums(uint32_t* sq, uint32_t* bt, uint32_t* sblob) {
    if (sq) *sq = g_sub_sq;
    if (bt) *bt = g_sub_bt;
    if (sblob) *sblob = g_sub_sblob;
}

// ---- COMPLETE forward-state pin of the game-loop (render-dispatch) ScriptAPI graph ----
// The game-loop ScriptAPI (cpp_arena::gameloop_addr, *0x49AFBC) is FORWARD-ONLY render
// dispatch: the re-sim never runs it, so rolling its memory back leaves its boost::signals2
// connection graph at N-k while the forward main-loop RunOneFrame expects N -> stale slot-list
// -> shared_count use_count hits 0 -> spurious dispose -> heap-corruption abort. The graph is
// a small, BOUNDED set of arena blocks (object + container + shared_count + connection
// nodes/bodies; measured ~20 blocks / <1 KB — does NOT explode into sim). Pin it COMPLETELY:
// on each forward capture, BFS EVERY block transitively reachable from the object through the
// arena (validated by the 16-byte Hdr magic) and snapshot each; after every restore re-apply
// them all. Complete capture is the key: earlier object+node-walk pins were INCOMPLETE (missed
// blocks past +0x200, over-captured adjacent memory) -> left an inconsistency that still
// aborted. Because the graph is bounded and self-contained, the forward bytes are internally
// consistent and the cross-pointers reference deterministic (re-simmed-identical) sim
// addresses. SQUIROLL_GL_PIN=1 to enable.
static bool g_gl_pin_on = (getenv("SQUIROLL_GL_PIN") != nullptr);
static constexpr uint32_t GL_BUF_SZ  = 4u * 1024 * 1024;     // forward bytes of the reachable graph
static constexpr int      GL_MAX_BLK = 32768;
struct GlBlk { uint32_t off, pos, len, link, req; };  // link=caller rva, req=reqsize (block identity)
static uint8_t  g_gl_buf[GL_BUF_SZ];
static GlBlk    g_gl_blks[GL_MAX_BLK];
static int      g_gl_nblks = 0;
static bool     g_gl_valid = false;
static int      g_gl_cap_use = -1;          // slot-list use_count at the last capture

// Validate that an arena offset is the payload of a live block (16-byte Hdr at off-16,
// magic 'CAPB' at off-4, cls in [4,24]); returns the block's alloc size or 0.
static uint32_t gl_block_size(const uint8_t* base, uint32_t sz, uint32_t off) {
    if (off < 16 || off >= sz || (off & 15)) return 0;
    if (*(const uint32_t*)(base + off - 4) != 0x42504143u) return 0;
    uint32_t cls = *(const uint32_t*)(base + off - 16);
    if (cls < 4 || cls > 24) return 0;
    return 1u << cls;
}
static uint8_t  g_bfs_bits[(128u * 1024 * 1024 / 16) / 8];   // 1 MB: seen-bit per 16-byte slot

static void gl_apply(uint32_t off, const uint8_t* src, uint32_t len) {
    Arena& A = g_ar[CPP_ARENA];
    memcpy(A.base + off, src, len);
    memcpy(A.mirror + off, src, len);                 // keep mirror in sync (no spurious delta)
    for (uint32_t o = off & ~(PAGE - 1); o < off + len; o += PAGE)
        A.phash[o / PAGE] = hash_page(A.base + (o & ~(PAGE - 1)));
}

// FORWARD save: BFS every block reachable from the game-loop object and copy its payload.
static void gl_pin_save() {
    g_gl_valid = false; g_gl_nblks = 0;
    const uint8_t* base = g_ar[CPP_ARENA].base;
    uint32_t cb = (uint32_t)(uintptr_t)base, sz = g_ar[CPP_ARENA].size;
    uint32_t gla = cpp_arena::gameloop_addr();
    if (!gla || gla < cb) return;
    uint32_t seed = gla - cb;
    if (!gl_block_size(base, sz, seed)) return;
    // Only refresh the snapshot from a KNOWN-GOOD slot-list (use_count == 1 = just the
    // signal's ref). A prior rollback can leave the use_count corrupted at 0 even at the
    // RunOneFrame-end; capturing that 0 and re-applying it perpetuates the dispose crash.
    // At this (stable) capture point the count is 1 the vast majority of frames, so keeping
    // the last good snapshot over a rare corrupt one is fresh, not stale.
    {
        uint32_t scv0 = *(const uint32_t*)(base + seed + 4);
        int u0 = (scv0 >= cb && scv0 + 8 <= cb + sz) ? *(const int*)(base + (scv0 - cb) + 4) : -9;
        if (u0 != 1 && g_gl_valid) return;        // keep last good capture
    }
    memset(g_bfs_bits, 0, sizeof g_bfs_bits);
    uint32_t pos = 0; int head = 0;
    g_gl_blks[g_gl_nblks++] = { seed, 0, 0, 0, 0 };
    g_bfs_bits[(seed / 16) >> 3] |= (uint8_t)(1u << ((seed / 16) & 7));
    while (head < g_gl_nblks && g_gl_nblks < GL_MAX_BLK) {
        uint32_t off = g_gl_blks[head].off;
        uint32_t paylen = gl_block_size(base, sz, off) - 16;
        if (pos + paylen > GL_BUF_SZ) break;          // out of buffer — stop (logged below)
        g_gl_blks[head].pos = pos; g_gl_blks[head].len = paylen;
        g_gl_blks[head].link = *(const uint32_t*)(base + off - 8);   // caller rva (block identity)
        g_gl_blks[head].req  = *(const uint32_t*)(base + off - 12);  // reqsize
        memcpy(g_gl_buf + pos, base + off, paylen);
        pos += paylen;
        const uint32_t* dw = (const uint32_t*)(base + off);
        for (uint32_t i = 0; i < paylen / 4; ++i) {
            uint32_t d = dw[i];
            if (d < cb || d >= cb + sz) continue;
            uint32_t toff = d - cb;
            if (!gl_block_size(base, sz, toff)) continue;
            uint32_t bit = toff / 16;
            if (g_bfs_bits[bit >> 3] & (1u << (bit & 7))) continue;
            g_bfs_bits[bit >> 3] |= (uint8_t)(1u << (bit & 7));
            if (g_gl_nblks < GL_MAX_BLK) g_gl_blks[g_gl_nblks++] = { toff, 0, 0, 0, 0 };
        }
        ++head;
    }
    g_gl_valid = true;
    // diag: the game-loop slot-list use_count being captured (sc = *(gla+4), use at sc+4)
    uint32_t scv = *(const uint32_t*)(base + seed + 4); int u = -9; bool insc = (scv >= cb && scv + 8 <= cb + sz);
    if (insc) u = *(const int*)(base + (scv - cb) + 4);
    g_gl_cap_use = u;
    static int ns = 0;
    if (ns < 40 && (u != 1 || ns < 8)) { ++ns; log_printf("[glpin] SAVE blocks=%d bytes=%u use=%d capped=%d\n",
                                   g_gl_nblks, pos, u, g_gl_nblks >= GL_MAX_BLK); }
}
// After a rollback restore, re-apply every captured forward block.
static int gl_live_use() {   // current game-loop slot-list use_count (or sentinel)
    const uint8_t* base = g_ar[CPP_ARENA].base;
    uint32_t cb = (uint32_t)(uintptr_t)base, sz = g_ar[CPP_ARENA].size;
    uint32_t gla = cpp_arena::gameloop_addr();
    if (!gla || gla < cb + 4 || gla >= cb + sz) return -9;
    uint32_t scv = *(const uint32_t*)(base + (gla - cb) + 4);
    if (scv < cb || scv + 8 > cb + sz) return -8;
    return *(const int*)(base + (scv - cb) + 4);
}
static void gl_pin_restore() {
    if (!g_gl_valid) return;
    const uint8_t* base = g_ar[CPP_ARENA].base;
    uint32_t sz = g_ar[CPP_ARENA].size;
    int skipped = 0;
    for (int i = 0; i < g_gl_nblks; ++i) {
        GlBlk& b = g_gl_blks[i];
        if (!b.len) continue;
        // Identity check: is the slot STILL the same allocation we captured (same caller-rva
        // + reqsize)? After a rollback the re-sim may have freed+reused this slot for a SIM
        // object — re-applying our render bytes there would corrupt it. Skip if it changed.
        if (!gl_block_size(base, sz, b.off) ||
            *(const uint32_t*)(base + b.off - 8)  != b.link ||
            *(const uint32_t*)(base + b.off - 12) != b.req) { ++skipped; continue; }
        gl_apply(b.off, g_gl_buf + b.pos, b.len);
    }
    static int nr = 0;
    if (nr < 30 && skipped) { ++nr;
        log_printf("[glpin] RESTORE blocks=%d skipped(reused)=%d\n", g_gl_nblks, skipped); }
}

// kept for the one-shot reachability log
static uint32_t g_bfs_q[1u << 16];
static void gl_bfs_measure() {
    static bool done = false;
    if (done) return;
    const uint8_t* base = g_ar[CPP_ARENA].base;
    uint32_t cb = (uint32_t)(uintptr_t)base, sz = g_ar[CPP_ARENA].size;
    uint32_t gla = cpp_arena::gameloop_addr();
    if (!gla || gla < cb) return;
    uint32_t seed = gla - cb;
    if (!gl_block_size(base, sz, seed)) { return; }
    done = true;
    memset(g_bfs_bits, 0, sizeof g_bfs_bits);
    int qn = 0, head = 0; uint64_t total = 0;
    g_bfs_q[qn++] = seed; g_bfs_bits[(seed / 16) >> 3] |= (uint8_t)(1u << ((seed / 16) & 7));
    while (head < qn && qn < (int)(1u << 16)) {
        uint32_t off = g_bfs_q[head++];
        uint32_t blk = gl_block_size(base, sz, off);
        if (!blk) continue;
        uint32_t paylen = blk - 16;
        total += paylen;
        const uint32_t* dw = (const uint32_t*)(base + off);
        for (uint32_t i = 0; i < paylen / 4; ++i) {
            uint32_t d = dw[i];
            if (d < cb || d >= cb + sz) continue;
            uint32_t toff = d - cb;
            if (!gl_block_size(base, sz, toff)) continue;
            uint32_t bit = toff / 16;
            if (g_bfs_bits[bit >> 3] & (1u << (bit & 7))) continue;
            g_bfs_bits[bit >> 3] |= (uint8_t)(1u << (bit & 7));
            if (qn < (int)(1u << 16)) g_bfs_q[qn++] = toff;
        }
    }
    uint32_t seedblk = gl_block_size(base, sz, seed);
    log_printf("[bfs] game-loop graph: seed_off=%X seed_blk=%u blocks=%d total_bytes=%llu capped=%d\n",
               seed, seedblk, qn, (unsigned long long)total, qn >= (int)(1u << 16));
    for (int k = 0; k < qn && k < 10; ++k)
        log_printf("[bfs]   blk[%d] off=%X size=%u\n", k, g_bfs_q[k], gl_block_size(base, sz, g_bfs_q[k]));
}

// Capture the game-loop render-dispatch graph at a STABLE point (called from the game-loop
// ScriptAPI's RunOneFrame end, where its slot-list refcount is balanced). Re-applied after
// every rollback restore so the forward-only render state never rolls back.
void gl_capture() { if (g_gl_pin_on && g_armed) gl_pin_save(); }

// RAII: hold cpp_arena's rollback lock for the whole call so a concurrent
// game-loop/bg-thread RunOneFrame (which acquires the same lock in runone_hook)
// cannot walk the arena connection lists while capture reads / restore rewrites
// them. Recursive CS -> the sim thread's own nested locks are harmless.
struct RollbackGuard {
    RollbackGuard()  { cpp_arena::rollback_lock(); }
    ~RollbackGuard() { cpp_arena::rollback_unlock(); }
};

uint32_t capture(uint32_t frame, const uint8_t* sblob, uint32_t sblob_len, uint32_t nocsum_tail) {
    if (!g_armed) return 0;
    // NB: capture does NOT hold g_rollback_cs (unlike restore). Capture only
    // READS the arena; a concurrent game-loop/bg RunOneFrame walking its own
    // connection list is fine (both readers, and cpp is excluded from the desync
    // checksum, so a mid-write render node in the snapshot can't desync). Locking
    // it was over-serialization: the game-loop thread holds g_rollback_cs across
    // its whole RunOneFrame, so a capture ~10x/frame at distance=10 would block
    // on it, and if that RunOneFrame waits on an event the blocked sim must
    // signal -> the intermittent WaitForSingleObject hang (0xC0DE/0x1234/0xFACE).
    // Only RESTORE (which rewrites the arena) needs the mutual exclusion.
    Slot& S = g_ring[frame % RING];
    // Detect "the slot already holds this frame" BEFORE we overwrite S.frame
    // below — this is how the per-frame phash-snap diagnostic distinguishes
    // a re-sim re-capture from a fresh forward save of frame N.
    const bool re_capture_diag = (S.frame == (int32_t)frame);
    S.frame = (int32_t)frame;
    // NB: the game-loop graph snapshot is NOT taken here (capture() can fire mid-RunOneFrame,
    // an unbalanced lock state). It's taken at the stable RunOneFrame end via gl_capture().
    if (!re_capture_diag && g_gl_pin_on) gl_bfs_measure();

    LARGE_INTEGER pt0; QueryPerformanceCounter(&pt0);
    uint64_t t_ww = 0, t_dirty = 0;
    for (int a = 0; a < NARENA; ++a) {
        Arena& A = g_ar[a];
        if (A.size == 0) { S.dn[a] = 0; continue; }   // unregistered arena (tf4 off)
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
            // NB: tried skipping the cpp render region [112MB,end) from capture to take
            // forward-only render state out of rollback — but the SIM holds references
            // INTO render objects, so dangling them broke sq/eng. Render and sim are not
            // cleanly separable at the page level. Reverted.
            // NB: we do NOT skip TF4_Number HUD pages from CAPTURE — the small digit
            // buffers share 4KB pages with rollback-critical data, so dropping whole
            // pages from the snapshot loses that data (divergence + crash). The pages
            // still roll back; they're only excluded from the divergence DIAGNOSTIC
            // (cppb/[comp]/divf below) so the HUD-number render geometry doesn't flag.
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

    // ACCURATE cpp phash: recompute ALL pages [0, bump), not just dirty ones, so the
    // divf/[comp]/phash_snap reflect real content (the dirty-only update leaves stale
    // hashes on pages that the write-watch reset stopped flagging -> false divergence).
    // Gated to early frames (cost ~one cppb pass) — enough to localize the race.
    if (g_diag && frame <= 12) {
        Arena& Ac = g_ar[CPP_ARENA];
        uint32_t bump = *(const uint32_t*)(Ac.base + BUMP_OFF[CPP_ARENA]);
        uint32_t upg = (bump + PAGE - 1) / PAGE;
        if (upg > Ac.npages) upg = Ac.npages;
        for (uint32_t pg = 0; pg < upg; ++pg)
            Ac.phash[pg] = hash_page(Ac.base + pg * PAGE);
    }

    // FULL latch of tracked cpp pages on every forward save (not just dirty), so the
    // divword can compare forward-frame-N vs resim-frame-N even for pages whose
    // forward-only write happened on an EARLIER frame (so they're not dirty now).
    // NB: gate on !re_capture_diag (the reliable "first save of this frame = forward"
    // signal), NOT is_resim() — set_resim(false) runs BEFORE the save (gekko:1603),
    // so is_resim() is already false here and would let the re-sim clobber the latch.
    if (!re_capture_diag) {
        for (int i = 0; i < N_TRACKED; ++i) {
            TrackedPage& t = g_tracked[i];
            if (t.pg < g_ar[CPP_ARENA].npages) {
                memcpy(t.bytes, g_ar[CPP_ARENA].base + (size_t)t.pg * PAGE, PAGE);
                t.captured = true;
            }
        }
        // One-shot dump of page 23404 header + the diverging array context, to ID the
        // object (vtable ptr at the block start, struct stride around dw 103).
        static bool dumped = false;
        if (!dumped && frame >= 5) {
            dumped = true;
            const uint32_t* p = (const uint32_t*)(g_ar[CPP_ARENA].base + (size_t)23404 * PAGE);
            for (int i = 0; i < 16; ++i)
                log_printf("[pgdump] dw%02d 0x%X = %08X %08X %08X %08X\n",
                           i*4, 0x5B6C000 + i*16,
                           p[i*4], p[i*4+1], p[i*4+2], p[i*4+3]);
            for (int i = 24; i < 40; ++i)   // around the diverging array (dw 96-160)
                log_printf("[pgdump] dw%02d 0x%X = %08X %08X %08X %08X\n",
                           i*4, 0x5B6C000 + i*16,
                           p[i*4], p[i*4+1], p[i*4+2], p[i*4+3]);
            // Scan for the arena block header(s) covering this region (HDR_MAGIC
            // 0x42504143 at +12) to get the allocating caller rva (h->link at +8).
            const uint8_t* abase = g_ar[CPP_ARENA].base;
            for (uint32_t o = 0x5B6A000; o < 0x5B6D000; o += 16) {
                const uint32_t* hh = (const uint32_t*)(abase + o);
                if (hh[3] == 0x42504143u)
                    log_printf("[blkhdr] off=0x%X cls=%u reqsize=%u link_rva=%08X\n",
                               o, hh[0], hh[1], hh[2]);
            }
        }
    }

    if (sblob_len > SMALL_CAP) {
        log_printf("[snapshot_ring] !! sblob blob too big (%u > %u)\n",
                   sblob_len, SMALL_CAP);
        sblob_len = SMALL_CAP;
    }
    memcpy(S.sblob, sblob, sblob_len);        // FULL blob stored for restore
    S.sblob_len = sblob_len;

    LARGE_INTEGER pt1; QueryPerformanceCounter(&pt1);
    // Exclude the restore-but-not-checksum tail (the RNG section) from the fold:
    // it is stored+restored above but its bytes are render-contaminated.
    uint32_t csum_len = (nocsum_tail <= sblob_len) ? sblob_len - nocsum_tail : sblob_len;
    // Render-heap (0x1a) pointer fields inside the bp section are restore-only:
    // battle_pools::save() emitted byte-exact nochecksum spans into this very
    // sblob (font / sprite-backing / effect-resource handles — they re-allocate
    // at a different 0x1a address when their owner is destroyed+recreated inside
    // the rollback window, while the gameplay sim matches byte-for-byte; 0xEAC9
    // BP@2231). The blob must keep the REAL bytes (it is the restore image), so
    // hash a scratch copy with the excluded spans removed.
    //
    // COMPACT (skip), not zero-in-place: the peer_local input pools
    // (InputSingle/Multi/Command) hold a DIFFERENT number of live objects on
    // each peer (each side sets up its own local player's input devices), so
    // their excluded byte range has a DIFFERENT LENGTH cross-peer. Zeroing keeps
    // that differing length, which shifts every trailing checksummed byte and
    // leaves the fold diverging. Copying only the KEPT bytes drops the excluded
    // regions entirely, so the compacted stream is identical cross-peer. For
    // constant-length exclusions (render fields) compaction is equivalent to the
    // old zeroing (both timelines drop the same-length range), so solo
    // determinism is unchanged. Spans arrive in ascending order (save() emits
    // them in serialization order); sort defensively and merge-skip.
    static uint8_t* s_csum_scratch = nullptr;   // sim-thread only
    static uint32_t s_csum_cap = 0;
    const uint8_t* fold_src = sblob;
    uint32_t fold_len = csum_len;
    {
        static const int SPX = 2064;
        static const uint8_t *lo[SPX], *hi[SPX];
        int nc = battle_pools::nochecksum_spans(lo, hi, SPX);
        // Keep only valid, non-empty, in-range spans; index-sort ascending by lo.
        static int ord[SPX];
        int m = 0;
        for (int i = 0; i < nc; ++i)
            if (lo[i] >= sblob && hi[i] <= sblob + csum_len && lo[i] < hi[i])
                ord[m++] = i;
        for (int a = 1; a < m; ++a) {           // insertion sort (near-sorted, small)
            int k = ord[a], b = a - 1;
            while (b >= 0 && lo[ord[b]] > lo[k]) { ord[b + 1] = ord[b]; --b; }
            ord[b + 1] = k;
        }
        if (m > 0) {
            if (s_csum_cap < csum_len) {
                if (s_csum_scratch) VirtualFree(s_csum_scratch, 0, MEM_RELEASE);
                s_csum_cap = (csum_len + 0xFFFFFu) & ~0xFFFFFu;   // 1MB granularity
                s_csum_scratch = (uint8_t*)VirtualAlloc(nullptr, s_csum_cap,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (!s_csum_scratch) s_csum_cap = 0;
            }
            if (s_csum_scratch) {
                uint32_t w = 0;
                const uint8_t* cur = sblob;
                const uint8_t* endp = sblob + csum_len;
                for (int a = 0; a < m; ++a) {
                    const uint8_t* L = lo[ord[a]];
                    const uint8_t* H = hi[ord[a]];
                    if (L < cur) L = cur;                    // overlap: clamp
                    if (L > cur) { memcpy(s_csum_scratch + w, cur, (size_t)(L - cur));
                                   w += (uint32_t)(L - cur); }
                    if (H > cur) cur = H;                    // advance past the hole
                }
                if (cur < endp) { memcpy(s_csum_scratch + w, cur, (size_t)(endp - cur));
                                  w += (uint32_t)(endp - cur); }
                fold_src = s_csum_scratch;
                fold_len = w;
            }
        }
    }
    uint32_t cs = fold_checksum(fold_src, fold_len);
    LARGE_INTEGER pt2; QueryPerformanceCounter(&pt2);

    // PHASH TRIPWIRE (ungated, write-once): bp+eng are byte-identical at the
    // round-transition desync (f=542), so the diverging checksum input is an
    // sq(0)/bt(1) arena page. On the forward save snapshot the sq/bt phash into
    // the slot; on a resim re-capture of the same frame compare and report the
    // FIRST diverging page -> arena base+offset, once, so it can be Dr0'd / IDA'd.
    if (!g_diag) {   // in diag mode the [divf] block below already does this
        Slot& St = g_ring[frame % RING];
        // Optional byte shadow of one sq/bt page (SQUIROLL_SHADOW_PG=N, arena
        // SQUIROLL_SHADOW_ARENA=0|1) so the tripwire can byte-diff it and print
        // the exact diverging dwords + values (frame counter? pointer? RNG?).
        static int shadow_pg = -2, shadow_a = 0;
        static uint8_t shadow[PAGE];
        if (shadow_pg == -2) { char b[12] = {0};
            shadow_pg = (GetEnvironmentVariableA("SQUIROLL_SHADOW_PG", b, sizeof b) > 0)
                        ? (int)strtoul(b, nullptr, 0) : -1;
            char c[4] = {0};
            if (GetEnvironmentVariableA("SQUIROLL_SHADOW_ARENA", c, sizeof c) > 0)
                shadow_a = (c[0] == '1') ? 1 : 0; }
        if (!re_capture_diag) {
            for (int a = 0; a <= 1; ++a)
                if (St.phash_snap[a])
                    memcpy(St.phash_snap[a], g_ar[a].phash, g_ar[a].npages * 4u);
            if (shadow_pg >= 0 && (uint32_t)shadow_pg < g_ar[shadow_a].npages)
                memcpy(shadow, g_ar[shadow_a].base + shadow_pg * PAGE, PAGE);
        } else {
            static bool ph_dumped = false;
            static const char* nm[2] = { "sq", "bt" };
            for (int a = 0; a <= 1 && !ph_dumped; ++a) {
                if (!St.phash_snap[a]) continue;
                uint32_t bump = *(const uint32_t*)(g_ar[a].base + BUMP_OFF[a]);
                uint32_t upg  = (bump + PAGE - 1) / PAGE;
                if (upg > g_ar[a].npages) upg = g_ar[a].npages;
                for (uint32_t pg = 0; pg < upg; ++pg) {
                    if (St.phash_snap[a][pg] != g_ar[a].phash[pg]) {
                        ph_dumped = true;
                        log_printf("[phtrip] FIRST arena divergence f=%u arena=%s "
                                   "pg=%u addr=0x%08X fwd_hash=%08x resim_hash=%08x\n",
                                   frame, nm[a], pg,
                                   (uint32_t)(uintptr_t)g_ar[a].base + pg * PAGE,
                                   St.phash_snap[a][pg], g_ar[a].phash[pg]);
                        // Byte-diff the shadowed page (if this is it) to name dwords.
                        if (a == shadow_a && pg == (uint32_t)shadow_pg) {
                            const uint32_t* fwd = (const uint32_t*)shadow;
                            const uint32_t* now = (const uint32_t*)(g_ar[a].base + pg * PAGE);
                            int shown = 0;
                            for (uint32_t o = 0; o < PAGE / 4 && shown < 16; ++o)
                                if (fwd[o] != now[o]) {
                                    log_printf("[phdiff]   +0x%03X (addr 0x%08X) fwd=%08X resim=%08X\n",
                                               o * 4, (uint32_t)(uintptr_t)g_ar[a].base + pg * PAGE + o * 4,
                                               fwd[o], now[o]);
                                    ++shown;
                                }
                        }
                        break;
                    }
                }
            }
        }
    }

    // TRACE: ungated per-component checksum for EVERY save of the first frames, so
    // the residual intermittent f=2 desync (cs diverges fwd-vs-resim while [comp]
    // sq/bt look clean) can be pinned to the exact component — sq fold, bt fold, or
    // the sblob (bp/mp/eng/irec/ihist). The 'fwd ' vs 'RESIM' tag + frame let us
    // diff a frame's forward save against each of its re-sim saves.
    if (g_diag && frame <= 3) {
        uint32_t sqh = 2166136261u, bth = 2166136261u, sbh = 2166136261u;
        for (int a = 0; a <= 1; ++a) {
            uint32_t bump = *(const uint32_t*)(g_ar[a].base + BUMP_OFF[a]);
            uint32_t upg  = (bump + PAGE - 1) / PAGE;
            if (upg > g_ar[a].npages) upg = g_ar[a].npages;
            uint32_t h = 2166136261u;
            for (uint32_t pg = 0; pg < upg; ++pg) { h ^= g_ar[a].phash[pg]; h *= 16777619u; }
            if (a == 0) sqh = h; else bth = h;
        }
        for (uint32_t i = 0; i < sblob_len; ++i) { sbh ^= sblob[i]; sbh *= 16777619u; }
        log_printf("[cspart] f=%u %s sq=%08x bt=%08x sblob=%08x cs=%08x\n",
                   frame, re_capture_diag ? "RESIM" : "fwd ", sqh, bth, sbh, cs);
    }

    // DIAGNOSTIC: per-arena page-hash fold, so a desync can be pinned to the
    // exact arena (sq / bullet / cpp) that diverged rather than one opaque
    // combined checksum. Fast mode skips it (the [divf] + cppb byte-hash below
    // are the ~5fps cost); the real desync checksum `cs` above is unaffected.
    if (g_diag) {
        uint32_t hc[NARENA], bc[NARENA];
        for (int a = 0; a < NDIAG; ++a) {   // tf4 pools (3,4) have no bump field — excluded
            uint32_t bump = *(const uint32_t*)(g_ar[a].base + BUMP_OFF[a]);
            bc[a] = bump;
            uint32_t upg  = (bump + PAGE - 1) / PAGE;
            if (upg > g_ar[a].npages) upg = g_ar[a].npages;
            uint32_t h = 2166136261u;
            for (uint32_t pg = 0; pg < upg; ++pg) {
                if (a == CPP_ARENA && cpp_arena::is_excluded_page(pg)) continue;  // HUD numbers
                h ^= g_ar[a].phash[pg]; h *= 16777619u;
            }
            hc[a] = h;
        }
        // DEFINITIVE: real dword-hash of cpp bytes [0, bump) — independent of the
        // phash machinery. If this is identical fwd-vs-resim while [comp] cpp= (phash
        // fold) differs, the phash divergence is a stale-hash artifact and the cpp
        // arena is actually byte-deterministic. Gated to early frames (cost).
        uint32_t cppb = 0;
        // RUN-TO-RUN NONDETERMINISM TRACE: compute the full non-HUD cpp content hash on every
        // FORWARD save across many frames. Run the same seed twice and diff the [comp] fwd cppb
        // per frame — the FIRST frame whose cppb differs run-to-run is where nondeterminism entered.
        if (frame <= 120 && !re_capture_diag) {
            cppb = 2166136261u;
            const uint32_t* dw = (const uint32_t*)g_ar[CPP_ARENA].base;
            uint32_t ndw = bc[CPP_ARENA] / 4;
            for (uint32_t i = 0; i < ndw; ++i) {
                if (cpp_arena::is_excluded_page((i * 4) / PAGE)) continue;  // HUD numbers
                cppb ^= dw[i]; cppb *= 16777619u;
            }
        }
        log_printf("[comp] f=%u sq=%08x/%u bt=%08x/%u cpp=%08x/%u cppb=%08x %s\n",
                   frame, hc[0], bc[0], hc[1], bc[1], hc[2], bc[2], cppb,
                   re_capture_diag ? "RESIM" : "fwd");

        // RUN-TO-RUN ARENA DIFF (SQUIROLL_DUMP=N): with addresses fixed (ASLR off + fixed arena
        // base), dump cpp [0,bump) to disk on the forward save of frame N. Run twice, binary-diff
        // the two dumps — the differing offsets are the EXACT residual nondeterminism. Map each
        // offset to its arena block (HDR_MAGIC 0x42504143 at payload-16, link_rva at +8) to name
        // the allocating site, so we know if it's a few handles/DLL-ptrs (excludable) or pervasive.
        {
            static int dump_frame = -2;
            if (dump_frame == -2) { char b[8]={0}; DWORD n=GetEnvironmentVariableA("SQUIROLL_DUMP",b,sizeof b);
                                    dump_frame = (n>0)? atoi(b) : -1; }
            if (dump_frame > 0 && (int)frame == dump_frame && !re_capture_diag) {
                HANDLE h = CreateFileA("aocf_cpp_dump.bin", GENERIC_WRITE, 0, nullptr,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    DWORD w = 0; WriteFile(h, g_ar[CPP_ARENA].base, bc[CPP_ARENA], &w, nullptr);
                    CloseHandle(h);
                    log_printf("[dump] f=%u wrote cpp [0,%u) -> aocf_cpp_dump.bin\n", frame, bc[CPP_ARENA]);
                }
            }
        }
    }

    // DIAGNOSTIC: every-frame divergence detection via per-page hash. At the
    // first capture of frame N (the forward save), snapshot live phash[]
    // into the slot. On any later re-capture of N (a rollback re-sim's
    // save), compare current phash[] against the snapshot — any page whose
    // hash differs is where the re-sim's frame-N output deviates from the
    // forward run's. No arbitrary frame threshold; the EARLIEST divergent
    // frame surfaces on its first re-capture. Fast mode skips it entirely
    // (phash_snap/page_snap are used only here) — the ~100K page comparisons +
    // per-page divword dumps are a big chunk of the tracing cost.
    if (g_diag) {
        Slot& S = g_ring[frame % RING];
        if (re_capture_diag) {
            static const char* names[NARENA] = { "sq", "bt", "cpp", "tf4A", "tf4B" };
            static bool first_dump_done = false;   // dump page bytes ONCE
            (void)first_dump_done;                 // kept for binary stability
            int totalp = 0;
            for (int a = 0; a < NDIAG; ++a) {   // tf4 pools (3,4) excluded from divf
                // cpp divf ON: bump is consistent fwd-vs-resim, so the divergence is
                // a CONTENT write at a stable address — trace which pages/dwords.
                int hits = 0;
                for (uint32_t pg = 0; pg < g_ar[a].npages && hits < 6; ++pg) {
                    if (a == CPP_ARENA && cpp_arena::is_excluded_page(pg)) continue;  // HUD numbers
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
                            extern bool divword_seen(uint32_t);
                            // Per-page dword diff against the save-time
                            // latch — only for tracked pages, only once
                            // per session, so it gives us the bytes that
                            // are flipping for the persistent divergent
                            // sources (connection_body refcount, etc).
                            TrackedPage* tp = find_tracked(pg);
                            static int g_dwcount = 0;
                            if (tp && tp->captured && g_dwcount < 80) {
                                ++g_dwcount;
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
                                        // (cpp_arena::attribute walks disabled —
                                        // they faulted on the divergent arena.)
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
            // subsequent re-capture detects only NEW divergences. (Diag arenas
            // only — tf4 pools have no phash_snap and are excluded from divf.)
            for (int a = 0; a < NDIAG; ++a)
                memcpy(S.phash_snap[a], g_ar[a].phash, g_ar[a].npages * 4);
            for (int i = 0; i < N_TARGETS; ++i) {
                int a = TARGET_PAGES[i].arena; uint32_t off = TARGET_PAGES[i].off;
                if (off + PAGE <= g_ar[a].size)
                    memcpy(S.page_snap[i], g_ar[a].base + off, PAGE);
            }
        } else {
            // First capture (forward) of this frame in the current ring
            // window — snapshot the per-page hashes AND the target page bytes
            // for later comparison. (Diag arenas only — see above.)
            for (int a = 0; a < NDIAG; ++a)
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
    static uint32_t prc = 0, psq = 0, pbt = 0, pcpp = 0, pt4a = 0, pt4b = 0;
    static uint64_t a_ww = 0, a_dirty = 0, a_rest = 0, a_fold = 0;
    psq    += S.dn[0];
    pbt    += S.dn[1];
    pcpp   += S.dn[2];
    pt4a   += S.dn[3];
    pt4b   += S.dn[4];
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
        log_printf("[snapshot_ring] dirty/cap sq=%u bt=%u cpp=%u tf4A=%u tf4B=%u "
                   "(%uKB)  us: getww=%u dirtycopy=%u sblobcopy=%u fold=%u\n",
                   psq / prc, pbt / prc, pcpp / prc, pt4a / prc, pt4b / prc,
                   (psq + pbt + pcpp + pt4a + pt4b) / prc * 4,
                   us(a_ww), us(a_dirty), us(a_rest), us(a_fold));
        prc = psq = pbt = pcpp = pt4a = pt4b = 0;
        a_ww = a_dirty = a_rest = a_fold = 0;
    }

    g_cur = (int64_t)frame;
    return cs;
}

const uint8_t* restore(uint32_t frame, uint32_t* sblob_len) {
    *sblob_len = 0;
    if (!g_armed) return nullptr;
    RollbackGuard _rb;   // block game-loop/bg RunOneFrame during the arena rewrite
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

    // NB: capture-before-restore here was WORSE (4/8 vs 2/8) — the graph snapshot isn't the
    // residual; the alloc COLLISION is (forward copy-on-write render blocks land above the
    // bump where the re-sim reuses the slots). Snapshot stays at the stable RunOneFrame-end.

    // NB: a DISPATCH-SIGNAL forward-state pin (create_and_bind 0x56AB5 blocks,
    // saved live here / re-applied after the reverse-apply, gl_pin-style) was
    // tried 2026-07-03 and REVERTED: those signals are NOT purely forward-only —
    // the SIM touches them on effect-creation connect, so pinning forward state
    // changed what a re-sim's connect allocates (one 0x40 connection node) ->
    // the whole sim-region alloc stream shifted 0x40 -> instant f≈47 desync
    // (caught by [bpreport]: every render field shifted +0x40, eng boost-pool
    // heads shifted +0x40). Evidence first — the round-end crash root was the
    // tEftElect NULL-linked-particle job (cl_iter_guard updeff hook), not these.

    // The game-loop slot-list shared_count (use/weak) is a LIVE boost::signals2 iteration
    // refcount — it counts shared_ptr temporaries that live on the STACK/registers, which the
    // rollback does NOT restore. Rolling the count back to a snapshot value desyncs it from the
    // still-live shared_ptrs (a reverted lock whose release still fires -> use->0 -> dispose ->
    // the ~f10-70 abort). Capture it now and re-apply after the reverse-apply, exactly like we
    // never roll back the CRT stack. sc is stable (gla+0x1C0), confirmed via haspend.
    uint32_t gl_sc_off = 0; int gl_use_b = 0, gl_weak_b = 0;
    {
        const uint8_t* B = g_ar[CPP_ARENA].base;
        uint32_t CB = (uint32_t)(uintptr_t)B, SZ = g_ar[CPP_ARENA].size;
        uint32_t gla = cpp_arena::gameloop_addr();
        if (gla >= CB + 8 && gla < CB + SZ) {
            uint32_t sc = *(const uint32_t*)(B + (gla - CB) + 4);
            if (sc >= CB && sc + 12 <= CB + SZ) {
                gl_sc_off = sc - CB;
                gl_use_b  = *(const int*)(B + gl_sc_off + 4);
                gl_weak_b = *(const int*)(B + gl_sc_off + 8);
            }
        }
    }

    // SYNC-PRIMITIVE PIN, part 1: snapshot the LIVE bytes of every registered
    // lock (CRITICAL_SECTIONs + std::mutex imps, see sync_pin.h) BEFORE any
    // memory moves. Locks are wall-clock state — a reverted lock under a live
    // thread => _Mtx_lock error -> throw -> abort (the f~248 streaming-slot
    // FASTFAIL) or a silent deadlock. Re-applied after the reverse-apply.
    static uint8_t       s_spbuf[512 * 1024];
    static sync_pin::Ent s_spent[8192];
    // PROFILE: restore-phase timers (step0 getww / step0 copy / reverse-apply /
    // syncpin / final resetww), reported per 120 restores.
    static uint64_t r_sp = 0, r_s0ww = 0, r_s0cp = 0, r_rev = 0, r_pin = 0, r_rww = 0;
    static uint32_t r_n = 0;
    LARGE_INTEGER rt0; QueryPerformanceCounter(&rt0);
    int spn = sync_pin::snapshot_live(s_spbuf, sizeof s_spbuf, s_spent, 8192);
    LARGE_INTEGER rt1; QueryPerformanceCounter(&rt1); r_sp += rt1.QuadPart - rt0.QuadPart;

    // STEP 0 — revert the POST-CAPTURE window (the crash-root fix, 2026-07-01).
    // Writes made since capture(g_cur) — the forward-only game-loop ScriptAPI
    // dispatch (RunOneFrame connection-node inserts/erases/cursor advances) and
    // the render pass both run AFTER the save — live in NO ring delta. The
    // reverse-apply below therefore left them in place while everything else
    // reverted, producing a MIXED state: e.g. the restored DrawCommandSlot head
    // (frame-target value) pointing at a node whose page kept its post-capture
    // "erased/freed/reused" bytes -> head -> 0xFFFFFF00 vertex colors -> the
    // +0x305AE abort (Dr0-proven: the fatal value was never written to the
    // slot itself; it sat one level deeper in a non-reverted page). The mirror
    // holds every page's state as of capture(g_cur), so reverting the current
    // write-watch dirty set to the mirror completes the chain:
    //     live --(mirror)--> g_cur --(ring deltas)--> target.
    // [#28 experiment] SQUIROLL_NO_STEP0: skip the post-capture revert for the
    // tf4 arenas (3/4) — the render pass writes draw-command/vertex scratch into
    // region A AFTER the save, and the D3D driver retains pointers into that
    // scratch from the last Present. STEP 0 rewinds those pages under the driver,
    // dangling its pointers -> nvwgf2um exec-at-heap. Leaving the tf4 post-capture
    // writes LIVE keeps the driver's scratch pointers valid. Diagnostic gate:
    //   =0 (default) revert all arenas (current behaviour)
    //   =1 skip revert for tf4 arenas only (3,4) — test the driver-crash cause
    //   =2 skip revert for ALL arenas
    static const int no_step0 = []{
        const char* e = getenv("SQUIROLL_NO_STEP0"); return e ? atoi(e) : 0; }();
    for (int a = 0; a < NARENA; ++a) {
        Arena& A = g_ar[a];
        if (!A.size) continue;
        ULONG_PTR count = A.npages;
        ULONG     gran  = 0;
        LARGE_INTEGER w0; QueryPerformanceCounter(&w0);
        UINT rc = GetWriteWatch(WRITE_WATCH_FLAG_RESET, A.base, A.size,
                                g_pgbuf, &count, &gran);
        LARGE_INTEGER w1; QueryPerformanceCounter(&w1); r_s0ww += w1.QuadPart - w0.QuadPart;
        if (rc != 0) {
            log_printf("[snapshot_ring] !! restore GetWriteWatch failed arena=%d\n", a);
            continue;
        }
        const bool skip_revert = (no_step0 == 2) || (no_step0 == 1 && (a == 3 || a == 4));
        if (!skip_revert) {
            for (ULONG_PTR i = 0; i < count; ++i) {
                uint32_t off = (uint32_t)((uint8_t*)g_pgbuf[i] - A.base);
                if (off + PAGE > A.size) continue;
                memcpy(A.base + off, A.mirror + off, PAGE);   // live -> state(g_cur)
                // mirror/phash already hold the capture(g_cur) value — untouched.
            }
        }
        LARGE_INTEGER w2; QueryPerformanceCounter(&w2); r_s0cp += w2.QuadPart - w1.QuadPart;
    }

    // Coalesced reverse-apply. A hot page (the VM operand stack) is dirtied
    // every frame, so it appears in every rolled-back frame's delta — the
    // naive newest-first apply would copy it once per frame. Instead walk
    // OLDEST->newest and restore each page exactly once, from the first
    // (oldest) delta that holds it: that pre-image is the page's value at
    // frame target-... = frame `target` (it was not dirtied in between).
    // 1 bit/page; sized for the largest arena. tf4 region A is 128 MB + 64 KB
    // (32784 pages) — larger than cpp's 128 MB — so this is sized for
    // MAX_ARENA_BYTES, matching the arm() guard.
    LARGE_INTEGER rv0; QueryPerformanceCounter(&rv0);
    static uint8_t seen[NARENA][(MAX_ARENA_BYTES / PAGE) / 8];
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
    LARGE_INTEGER rv1; QueryPerformanceCounter(&rv1); r_rev += rv1.QuadPart - rv0.QuadPart;

    // SYNC-PRIMITIVE PIN, part 2: re-apply each pinned lock's live bytes over
    // whatever the restore just wrote (base + mirror + phash, exactly like the
    // gl_sc treatment below). Only entries inside a snapshotted arena matter —
    // the restore never touched anything else.
    {
        uint32_t pos = 0;
        int applied = 0;
        for (int i = 0; i < spn; ++i) {
            uint32_t addr = s_spent[i].addr, len = s_spent[i].len;
            for (int a = 0; a < NARENA; ++a) {
                Arena& A = g_ar[a];
                if (!A.size) continue;
                uint32_t lo = (uint32_t)(uintptr_t)A.base;
                if (addr < lo || addr + len > lo + A.size) continue;
                uint32_t off = addr - lo;
                memcpy(A.base   + off, s_spbuf + pos, len);
                memcpy(A.mirror + off, s_spbuf + pos, len);
                for (uint32_t o = off & ~(PAGE - 1); o < off + len; o += PAGE)
                    A.phash[o / PAGE] = hash_page(A.base + o);
                ++applied;
                break;
            }
            pos += len;
        }
        static int nlog = 0;
        if (nlog < 6 && applied) { ++nlog;
            log_printf("[sync_pin] restore re-applied %d/%d pinned locks\n", applied, spn); }
    }

    // use_count "preservation" — DISABLED by default since restore step-0 (2026-07-01).
    // This pre-step-0 hack copied the LIVE use/weak over the restored values every
    // rollback. With step-0 the restore chain is complete, so the restored count IS
    // correct (no game-loop lock is held at restore time — the dispatch runs in
    // window_render, not inside tick). Worse, copying the live value PERPETUATES
    // corruption: once any transient glitch leaves use=0 live, this re-applied 0
    // over the correct restored 1 on every restore -> permanently dead signal ->
    // the infinite HasPendingFrame catch-up hang (use=0 weak=0 spin, 5/10 runs).
    // SQUIROLL_GLSC=1 re-enables for comparison runs.
    static const bool g_glsc_preserve = (getenv("SQUIROLL_GLSC") != nullptr);
    if (gl_sc_off && g_glsc_preserve) {
        Arena& A = g_ar[CPP_ARENA];
        *(int*)(A.base   + gl_sc_off + 4) = gl_use_b;
        *(int*)(A.base   + gl_sc_off + 8) = gl_weak_b;
        *(int*)(A.mirror + gl_sc_off + 4) = gl_use_b;     // keep mirror in sync (no spurious delta)
        *(int*)(A.mirror + gl_sc_off + 8) = gl_weak_b;
        A.phash[gl_sc_off / PAGE] = hash_page(A.base + (gl_sc_off & ~(PAGE - 1)));
    }

    // Forward-state pin: the reverse-apply above rolled the game-loop ScriptAPI object
    // (render-dispatch, forward-only) back to `target`, reverting its slot-list shared_count
    // use_count to a dead state. Re-apply the live forward copy so HasPendingFrame/RunOneFrame
    // see a valid refcount instead of disposing a live signal. Done BEFORE ResetWriteWatch so
    // these writes don't show up as the re-sim's dirty pages.
    if (g_gl_pin_on) gl_pin_restore();

    LARGE_INTEGER rp1; QueryPerformanceCounter(&rp1); r_pin += rp1.QuadPart - rv1.QuadPart;

    // Discard the write-watch entries our own restore writes just produced,
    // so the next capture sees only the re-sim advance's dirty pages.
    for (int a = 0; a < NARENA; ++a)
        if (g_ar[a].size) ResetWriteWatch(g_ar[a].base, g_ar[a].size);
    LARGE_INTEGER rp2; QueryPerformanceCounter(&rp2); r_rww += rp2.QuadPart - rp1.QuadPart;
    if (++r_n >= 120) {
        LARGE_INTEGER fr; QueryPerformanceFrequency(&fr); uint64_t hz = fr.QuadPart;
        auto us = [&](uint64_t t){ return (uint32_t)(t * 1000000ull / hz / r_n); };
        log_printf("[perf-restore] us/restore: syncpin=%u step0_getww=%u step0_copy=%u "
                   "reverse=%u pin_reapply=%u final_resetww=%u\n",
                   us(r_sp), us(r_s0ww), us(r_s0cp), us(r_rev), us(r_pin), us(r_rww));
        r_sp = r_s0ww = r_s0cp = r_rev = r_pin = r_rww = 0; r_n = 0;
    }

    Slot& T = g_ring[frame % RING];
    if (T.frame != (int32_t)frame) {
        log_printf("[snapshot_ring] !! restore: no sblob blob for f=%u\n", frame);
        return nullptr;
    }
    *sblob_len = T.sblob_len;
    return T.sblob;
}

} // namespace snapshot_ring
