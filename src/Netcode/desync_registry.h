#pragma once
// desync_registry — the single declarative source of truth for RENDER-ONLY
// fields inside otherwise-checksummed simulation state (GDC MvC3 rollback
// tooling model: a standardized desync log + a differ that names every
// divergence, driven by one registry instead of exclusions scattered per-file).
//
// Two consumers:
//  1. battle_pools::save() — emits a byte-exact nochecksum span for each
//     registered field of each live slot (matched by OBJECT VTABLE, masked by
//     OFFSET). Offset-based masking is value-independent: identical span sets
//     on the forward and re-sim timelines by construction, immune to the
//     render heap moving between sessions (which broke the value-range scan:
//     0x1a... one session, 0x518a... the next).
//  2. the desync report (battle_pools::diff_report) — annotates every
//     diverging dword as KNOWN-RENDER(name) vs UNKNOWN (= new field to
//     triage), so one run yields a complete, named analysis.
//
// A field earns an entry ONLY with evidence: a desync whose diff shows the
// field holding pointers into a non-captured heap (or a count of visual-only
// objects) while the rest of the sim state matches byte-for-byte. Every entry
// cites that evidence. Sim state NEVER goes here — a wrongly-registered sim
// field would hide real desyncs; when in doubt, leave it checksummed and let
// the desync-abort + report flag it again.
//
// Layout note: battle-pool slots holding these objects are
// std::_Ref_count_obj_alloc (make_shared): [ctrl vtable][uses][weaks][object]
// — the control header is 0x0C bytes, PLUS alignment padding for the object:
// Actor2D (align 4) starts at slot+0x0C; AnimationController2D (holds
// D3DMATRIX/__m128, align 16) starts at slot+0x10. Each SlotType records its
// header size; matching probes the OBJECT vtable at slot+hdr.

#include <stdint.h>

namespace desync_registry {

struct Field {
    uint32_t    obj_off;   // offset within the OBJECT (not the slot)
    uint32_t    len;       // bytes
    const char* name;      // short field name for the report
};

struct SlotType {
    uint32_t     vtable_rva;  // object vtable that identifies the type
    uint32_t     hdr;         // make_shared header size: object at slot+hdr
    const char*  type_name;
    const Field* fields;
    int          nfields;
};

// --- AnimationController2D (object vtable 0x445E10) -------------------------
// Evidence: 0xEAC9 distance=10 BP@2231 — the ONLY bp bytes diverging while the
// whole gameplay sim matched:
//  - obj+0x10/+0x14: BitmapFontResource handle + shared_ptr ctrl block. Dtor
//    releases via Manbow__bitmapfont_resource_release_handle; points into the
//    OS-placed render heap (session-dependent base: 0x1ab3... / 0x518a...).
//  - obj+0x224..: sprites std::vector<shared_ptr<Sprite>> begin/end/cap —
//    render backing on the same heap. (Re-homing this LIVE vector broke the
//    HUD — never mutate; checksum-exclusion only.)
static const Field kAnimCtrl2D_fields[] = {
    { 0x010, 8,  "font-resource handle (AnimCtrlBase+0x10)" },
    { 0x224, 12, "sprites vector begin/end/cap (render backing)" },
};

// --- Manbow::Actor2D (object vtable 0x447A38, object at slot+0x0C) ----------
// Evidence: 0xEAC9 distance=10 f2231 [bpreport] — with the AnimCtrl2D fields
// registered, the ONLY remaining diverging dwords were Actor2D obj+0xD0
// (__unkD0 in the decomp header) and obj+0xD8 (__ptrD8), both holding
// render-heap pointers (fwd 0x1AB881B0 / resim 0x1ACF73E0; same heap page as
// the AnimCtrl sprites backing) with all other Actor2D/sim state identical.
// They sit right after the two SqratFunction slots (update_func @0xA8,
// __sqrat_funcBC @0xBC) — unreversed render resource handles.
static const Field kActor2D_fields[] = {
    { 0x0D0, 4, "__unkD0 render-heap handle" },
    { 0x0D8, 4, "__ptrD8 render-heap handle" },
};

// Registry of slot types. Unknown diverging vtables are printed by the report
// (raw + RVA) — extending this table is a one-line, evidence-cited addition.
static const SlotType kSlotTypes[] = {
    { 0x445E10, 0x10, "Manbow::AnimationController2D",
      kAnimCtrl2D_fields, (int)(sizeof(kAnimCtrl2D_fields) / sizeof(Field)) },
    { 0x447A38, 0x0C, "Manbow::Actor2D",
      kActor2D_fields, (int)(sizeof(kActor2D_fields) / sizeof(Field)) },
};
static const int kNSlotTypes = (int)(sizeof(kSlotTypes) / sizeof(SlotType));

// --- known render-only .data globals (annotation for the eng report) --------
// These are already carved out of the eng checksum (exr[] + rng_collect in
// engine_snap.cpp); listed here so the report names any future hit near them
// and so the "why" lives in one place.
struct DataGlobal { uint32_t rva; uint32_t len; const char* name; };
static const DataGlobal kDataGlobals[] = {
    { 0x4DB06C, 4,    "g_eft_param_last_owner_ptr (visual-effect param cache)" },
    { 0x4DB0B4, 4,    "g_eft_param_pair_count (visual-effect param count)" },
    { 0x4DBBE0, 8,    "g_eft_param_pairbuf begin/end (heap ptrs)" },
    { 0x4DC790, 0x40, "FontPool + BitmapFont boost-pool structs" },
    { 0x4DD0A0, 0x20, "BitmapFont glyph boost-pool struct" },
    { 0x4DC320, 4,    "g_gfx_active_transfer_byte_offset (GPU vertex-transfer cursor)" },
};
static const int kNDataGlobals = (int)(sizeof(kDataGlobals) / sizeof(DataGlobal));

// Lookup: match a slot's content against the registry by probing the object
// vtable at each type's header offset. content = the slot's bytes, ss = slot
// size; caller supplies the module base.
inline const SlotType* match_slot(const uint8_t* content, uint32_t ss,
                                  uint32_t module_base) {
    for (int i = 0; i < kNSlotTypes; ++i) {
        const SlotType& t = kSlotTypes[i];
        if (t.hdr + 4 > ss) continue;
        uint32_t vt;
        __builtin_memcpy(&vt, content + t.hdr, 4);
        if (vt - module_base == t.vtable_rva) return &t;
    }
    return nullptr;
}

// Lookup: is [obj_off, obj_off+4) inside a registered render-only field?
inline const Field* find_field(const SlotType* t, uint32_t obj_off) {
    if (!t) return nullptr;
    for (int i = 0; i < t->nfields; ++i) {
        const Field& f = t->fields[i];
        if (obj_off >= f.obj_off && obj_off < f.obj_off + f.len) return &f;
    }
    return nullptr;
}

// Lookup: does an RVA fall inside a known render-only .data global?
inline const DataGlobal* find_data_global(uint32_t rva) {
    for (int i = 0; i < kNDataGlobals; ++i) {
        const DataGlobal& g = kDataGlobals[i];
        if (rva >= g.rva && rva < g.rva + g.len) return &g;
    }
    return nullptr;
}

} // namespace desync_registry
