#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "tf4_snap.h"
#include "patch_utils.h"   // _R address literal, mem_write
#include "log.h"

namespace tf4_snap {
namespace {

// th155 globals. dword_4DB48C caches a pointer to the TF4-engine mspace;
// dword_4DC0C0 is a second mspace struct embedded directly in .data. A
// dlmalloc msegment record is { void* base; size_t size; msegment* next;
// uint32 flags }; the first one is embedded in the mspace struct at
// offset 0x1C0 (448).
#define MSPACE_PTR_ADDR  (0x4DB48C_R)   // uint32 — pointer to the mspace
#define MSPACE_EMBEDDED  (0x4DC0C0_R)   // an mspace struct living in .data
static constexpr uint32_t SEG_LIST_OFF = 448;

static constexpr uint32_t PAGE     = 4096;
static constexpr int      RING     = 12;       // > GekkoConfig::check_distance (8)
static constexpr int      MAX_SEG  = 128;
// Per-frame compressed delta capacity. The mspace dirties ~14 MB of pages
// a frame, but only the byte-runs that differ from the mirror are stored;
// 6 MB is generous slack over the expected compressed size.
static constexpr uint32_t SLOT_CAP = 6u * 1024 * 1024;
// Differing byte-runs closer than this are merged into one record — keeps
// the record count down when a page has a few scattered small edits.
static constexpr uint32_t RUN_GAP  = 8;

struct Seg {
    uint8_t* base;
    uint32_t size;
    uint32_t npages;
    uint8_t* mirror;   // full copy — segment state as of frame g_cur
    uint8_t* cover;    // size/8 bytes — restore-time byte coverage bitmap
    bool     has_ww;   // MEM_WRITE_WATCH available (else mirror-scan)
};
static Seg g_seg[MAX_SEG];
static int g_nseg = 0;

// One delta record: [u32 absolute address][u16 run length][run bytes].
struct Slot {
    int32_t  frame;    // frame held, -1 = empty
    uint32_t len;      // bytes used in buf
    uint8_t* buf;      // SLOT_CAP
};
static Slot     g_ring[RING];
static void**   g_pgbuf  = nullptr;   // GetWriteWatch page-address scratch
static uint32_t g_maxpg  = 0;

static bool     g_armed  = false;
static int64_t  g_cur    = -1;        // newest captured frame

// Index of the tracked segment containing address `a`, or -1.
static int seg_of(uintptr_t a) {
    for (int i = 0; i < g_nseg; ++i)
        if (a >= (uintptr_t)g_seg[i].base &&
            a <  (uintptr_t)g_seg[i].base + g_seg[i].size)
            return i;
    return -1;
}

// Walk one mspace's embedded segment list, invoking add(base,size) per
// segment. `ms` is the mspace struct address.
template <class F>
static void walk_segments(uint8_t* ms, F&& add) {
    if (!ms) return;
    uint8_t* seg = ms + SEG_LIST_OFF;
    for (int guard = 0; guard < MAX_SEG && seg; ++guard) {
        uint8_t* base = *(uint8_t**)(seg + 0);
        uint32_t size = *(uint32_t*)(seg + 4);
        uint8_t* next = *(uint8_t**)(seg + 8);
        // Sanity-guard a torn read of th155's live list.
        if (base && size && ((uintptr_t)base & (PAGE - 1)) == 0 &&
            size >= PAGE && size <= 512u * 1024 * 1024)
            add(base, size);
        seg = next;
    }
}

// Register a freshly-seen segment: mirror it, allocate its coverage
// bitmap, probe write-watch. Returns false if out of room / alloc fails.
static bool add_segment(uint8_t* base, uint32_t size) {
    if (g_nseg >= MAX_SEG) return false;
    Seg& S = g_seg[g_nseg];
    S.base   = base;
    S.size   = size;
    S.npages = size / PAGE;
    S.mirror = (uint8_t*)VirtualAlloc(nullptr, size,
                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    S.cover  = (uint8_t*)VirtualAlloc(nullptr, (size / 8) + 1,
                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!S.mirror || !S.cover) {
        log_printf("[tf4_snap] !! add_segment: alloc failed (%uKB)\n",
                   size / 1024);
        return false;
    }
    memcpy(S.mirror, base, size);

    // Probe write-watch: a successful GetWriteWatch means the segment was
    // VirtualAlloc'd with MEM_WRITE_WATCH (our call-site patch landed in
    // time); otherwise capture() falls back to scanning against the mirror.
    ULONG_PTR cnt = S.npages;
    ULONG     gran = 0;
    UINT rc = GetWriteWatch(WRITE_WATCH_FLAG_RESET, base, size,
                            g_pgbuf, &cnt, &gran);
    S.has_ww = (rc == 0);

    if (S.npages > g_maxpg) g_maxpg = S.npages;
    ++g_nseg;
    log_printf("[tf4_snap] segment %d: base=%p %uKB ww=%s\n",
               g_nseg - 1, (void*)base, size / 1024,
               S.has_ww ? "yes" : "NO(scan)");
    return true;
}

// Pick up segments the mspace grew. New segments only — existing ones are
// matched by base and left intact. (Segment release mid-battle is rare;
// a released segment simply stops being written and is harmless to keep.)
static void scan_segments() {
    uint8_t* ms1 = *(uint8_t**)(uintptr_t)(MSPACE_PTR_ADDR);
    uint8_t* ms2 = (uint8_t*)(uintptr_t)(MSPACE_EMBEDDED);
    auto consider = [&](uint8_t* base, uint32_t size) {
        for (int i = 0; i < g_nseg; ++i)
            if (g_seg[i].base == base) return;          // already tracked
        add_segment(base, size);
    };
    walk_segments(ms1, consider);
    if (ms2 != ms1) walk_segments(ms2, consider);
}

// Diff one dirty page against its mirror and append the differing
// byte-runs to *pp (bounded by `end`). Pre-image bytes come from the
// mirror; the mirror is then synced to the page. Sets *over on overflow.
static void diff_page(uint8_t* pg, uint8_t* mir, uintptr_t page_addr,
                      uint8_t** pp, uint8_t* end, bool* over) {
    uint32_t i = 0;
    while (i < PAGE) {
        if (pg[i] == mir[i]) { ++i; continue; }
        uint32_t s = i, last = i;
        ++i;
        while (i < PAGE) {
            if (pg[i] != mir[i]) { last = i; ++i; }
            else if (i - last <= RUN_GAP) { ++i; }
            else break;
        }
        uint32_t rlen = last + 1 - s;
        uint8_t* p = *pp;
        if (p + 6 + rlen > end) { *over = true; break; }
        *(uint32_t*)p = (uint32_t)(page_addr + s); p += 4;
        *(uint16_t*)p = (uint16_t)rlen;            p += 2;
        memcpy(p, mir + s, rlen);                  p += rlen;
        *pp = p;
    }
    memcpy(mir, pg, PAGE);   // mirror now holds this frame's value
}

} // namespace

void install() {
    // Add MEM_WRITE_WATCH (0x200000) to the mspace's four VirtualAlloc
    // call sites by patching the `push flAllocationType` immediate that
    // precedes each `call ds:VirtualAlloc`. Segments allocated after this
    // are write-watch-enabled, so capture() finds dirty pages with a cheap
    // GetWriteWatch instead of scanning. (push imm32 = opcode 0x68 + imm;
    // the immediate sits one byte past the push address.)
    struct Site { uintptr_t imm; uint32_t oldv, newv; };
    const Site sites[4] = {
        { (uintptr_t)(0x3321E_R), 0x00003000u, 0x00203000u },  // sub_331C0 #1
        { (uintptr_t)(0x332A3_R), 0x00003000u, 0x00203000u },  // sub_331C0 #2
        { (uintptr_t)(0x336BA_R), 0x00103000u, 0x00303000u },  // sub_33680
        { (uintptr_t)(0x33E63_R), 0x00003000u, 0x00203000u },  // sub_33DE0
    };
    int ok = 0;
    for (const Site& s : sites) {
        uint32_t cur = *(volatile uint32_t*)s.imm;
        if (cur != s.oldv) {
            log_printf("[tf4_snap] !! install: site %08X has %08X, "
                       "expected %08X — skipped\n",
                       (uint32_t)s.imm, cur, s.oldv);
            continue;
        }
        mem_write(s.imm, s.newv);
        ++ok;
    }
    log_printf("[tf4_snap] install: MEM_WRITE_WATCH patched %d/4 sites\n", ok);
}

void arm() {
    if (g_armed) return;

    uint8_t* ms1 = *(uint8_t**)(uintptr_t)(MSPACE_PTR_ADDR);
    uint8_t* ms2 = (uint8_t*)(uintptr_t)(MSPACE_EMBEDDED);
    if (!ms1 && !ms2) {
        log_printf("[tf4_snap] !! arm: TF4 mspace not resolved\n");
        return;
    }

    // GetWriteWatch scratch — sized once the segments are known. Walk
    // first to learn the largest segment, then allocate.
    uint32_t maxpg = 0;
    auto measure = [&](uint8_t*, uint32_t size) {
        uint32_t np = size / PAGE;
        if (np > maxpg) maxpg = np;
    };
    walk_segments(ms1, measure);
    if (ms2 != ms1) walk_segments(ms2, measure);
    g_pgbuf = (void**)VirtualAlloc(nullptr, (maxpg + 64) * sizeof(void*),
                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_pgbuf) {
        log_printf("[tf4_snap] !! arm: pgbuf alloc failed\n");
        return;
    }

    for (int s = 0; s < RING; ++s) {
        g_ring[s].frame = -1;
        g_ring[s].len   = 0;
        g_ring[s].buf   = (uint8_t*)VirtualAlloc(nullptr, SLOT_CAP,
                              MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_ring[s].buf) {
            log_printf("[tf4_snap] !! arm: ring buf %d alloc failed\n", s);
            return;
        }
    }

    g_nseg = 0;
    g_maxpg = 0;
    scan_segments();           // mirror + write-watch-probe every segment
    if (g_nseg == 0) {
        log_printf("[tf4_snap] !! arm: no mspace segments found\n");
        return;
    }

    uint32_t total = 0;
    int ww = 0;
    for (int i = 0; i < g_nseg; ++i) { total += g_seg[i].size; ww += g_seg[i].has_ww; }
    g_cur   = -1;
    g_armed = true;
    log_printf("[tf4_snap] armed: segs=%d (%d write-watched) total=%uKB ring=%d\n",
               g_nseg, ww, total / 1024, RING);
}

bool armed() { return g_armed; }

void capture(uint32_t frame) {
    if (!g_armed) return;

    scan_segments();           // pick up any segment grown since last frame

    Slot& S = g_ring[frame % RING];
    uint8_t* p   = S.buf;
    uint8_t* end = S.buf + SLOT_CAP;
    bool over = false;

    for (int i = 0; i < g_nseg && !over; ++i) {
        Seg& sg = g_seg[i];
        if (sg.has_ww) {
            // Cheap path: GetWriteWatch lists exactly the pages written
            // since the last capture.
            ULONG_PTR cnt = sg.npages;
            ULONG     gran = 0;
            UINT rc = GetWriteWatch(WRITE_WATCH_FLAG_RESET, sg.base, sg.size,
                                    g_pgbuf, &cnt, &gran);
            if (rc != 0) {
                // Lost write-watch — degrade this segment to scanning.
                sg.has_ww = false;
            } else {
                for (ULONG_PTR k = 0; k < cnt && !over; ++k) {
                    uint32_t off = (uint32_t)((uint8_t*)g_pgbuf[k] - sg.base);
                    if (off + PAGE > sg.size) continue;
                    diff_page(sg.base + off, sg.mirror + off,
                              (uintptr_t)sg.base + off, &p, end, &over);
                }
            }
        }
        if (!sg.has_ww) {
            // Fallback: scan every page against the mirror.
            for (uint32_t off = 0; off + PAGE <= sg.size && !over; off += PAGE) {
                if (memcmp(sg.base + off, sg.mirror + off, PAGE) != 0)
                    diff_page(sg.base + off, sg.mirror + off,
                              (uintptr_t)sg.base + off, &p, end, &over);
            }
        }
    }

    if (over) {
        log_printf("[tf4_snap] !! delta overflow f=%u — raise SLOT_CAP\n",
                   frame);
    }
    S.len   = (uint32_t)(p - S.buf);
    S.frame = (int32_t)frame;
    g_cur   = (int64_t)frame;
}

void restore(uint32_t frame) {
    if (!g_armed) return;
    int64_t target = (int64_t)frame;
    if (target > g_cur) {
        log_printf("[tf4_snap] !! restore future f=%u cur=%d\n",
                   frame, (int)g_cur);
        return;
    }

    // Revert any uncaptured in-progress writes (write-watched segments
    // only): copy the mirror — frame g_cur's state — back over pages
    // dirtied since the last capture, returning each segment to g_cur.
    for (int i = 0; i < g_nseg; ++i) {
        Seg& sg = g_seg[i];
        if (!sg.has_ww) continue;
        ULONG_PTR cnt = sg.npages;
        ULONG     gran = 0;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, sg.base, sg.size,
                          g_pgbuf, &cnt, &gran) == 0) {
            for (ULONG_PTR k = 0; k < cnt; ++k) {
                uint32_t off = (uint32_t)((uint8_t*)g_pgbuf[k] - sg.base);
                if (off + PAGE <= sg.size)
                    memcpy(sg.base + off, sg.mirror + off, PAGE);
            }
        }
        memset(sg.cover, 0, (sg.size / 8) + 1);
    }

    // Reverse the byte-run chain oldest -> newest (frames target+1 .. cur).
    // The first writer of a byte wins, so the oldest pre-image — the byte's
    // value at frame `target` — is the one applied.
    for (int64_t f = target + 1; f <= g_cur; ++f) {
        Slot& S = g_ring[(uint32_t)(f % RING)];
        if (S.frame != (int32_t)f) {
            log_printf("[tf4_snap] !! restore gap: want f=%d slot frame=%d\n",
                       (int)f, S.frame);
            continue;
        }
        const uint8_t* p   = S.buf;
        const uint8_t* end = S.buf + S.len;
        while (p + 6 <= end) {
            uintptr_t addr = (uintptr_t)(*(const uint32_t*)p); p += 4;
            uint32_t  rlen = *(const uint16_t*)p;              p += 2;
            if (p + rlen > end) break;
            const uint8_t* src = p;
            p += rlen;
            int si = seg_of(addr);
            if (si < 0) continue;                       // segment released
            Seg& sg = g_seg[si];
            uint32_t idx = (uint32_t)(addr - (uintptr_t)sg.base);
            for (uint32_t b = 0; b < rlen; ++b) {
                uint32_t bi = idx + b;
                if (bi >= sg.size) break;
                uint8_t  m = 1u << (bi & 7);
                if (sg.cover[bi >> 3] & m) continue;    // an older frame won
                sg.cover[bi >> 3] |= m;
                sg.base[bi]   = src[b];
                sg.mirror[bi] = src[b];
            }
        }
    }

    // Discard the write-watch entries our own restore writes produced, so
    // the re-sim's first capture sees only the re-sim's dirty pages.
    for (int i = 0; i < g_nseg; ++i)
        if (g_seg[i].has_ww)
            ResetWriteWatch(g_seg[i].base, g_seg[i].size);

    g_cur = target;
}

} // namespace tf4_snap
