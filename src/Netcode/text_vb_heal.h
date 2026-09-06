#pragma once
#ifndef TEXT_VB_HEAL_H
#define TEXT_VB_HEAL_H 1

// ============================================================================
// text_vb_heal — make Manbow::String (UI.Core.Text) vertex streams rollback-safe.
//
// THE BUG (plugins, 2026-09-05): a String lives in the snapshotted cpp_arena, so
// a rollback rewinds its fields, INCLUDING the shared_ptr<TF4::D3D11VertexBuffer>
// at String+288/+292 and the stream capacity at +296. The vertex-buffer object
// (pool node) it names lives in th155's TPoolAllocator free-list (mspace B,
// NEVER snapshotted). Whenever the text GREW between the snapshot and the
// rollback, the String had move-assigned a NEW node and RELEASED the old one:
// the rewound pointer now names a node that is on the free-list (garbage
// ID3D11Buffer -> d3d11 Map faults, 0xC0000094) or has been re-popped by
// another String (Map/memcpy on somebody else's buffer -> later corruption).
// th155's own HUD strings never grow mid-battle, which is why only a plugin's
// freshly created Text ever hit this.
//
// THE FIX: a per-node registry {node -> owner String, capacity} maintained from
// StoreStreamWithStride (create) and the pool release (destroy). On every
// forward render of a String, validate its stream pointer against the
// registry. If it is stale (rewound), RE-ADOPT the newest live node this String
// allocated (no leak, no realloc) or, failing that, drop the pointer and force
// a reallocation. Registry lives on the DLL heap; never snapshotted.
// ============================================================================
namespace text_vb_heal {
void install();   // after the th155 image is loaded; idempotent
}
#endif
