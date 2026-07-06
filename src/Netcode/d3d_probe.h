#pragma once
#ifndef D3D_PROBE_H
#define D3D_PROBE_H 1

// [#28] Runtime instrumentation to find which D3D11 resources are backed by
// tf4-mspace region-A memory. The NVIDIA UMD holds a persistent reference into
// region A that rollback's reverse-apply rewrites inconsistently -> nvwgf2um
// exec-at-heap. This hooks the device/context COM vtables (CreateBuffer /
// CreateTexture2D / Map / UpdateSubresource) and logs any call whose
// app-memory pointer falls inside region A, so we can identify the resource to
// pin (or exclude) across restore.
//
// Idempotent: call every frame; it patches the vtables exactly once, when the
// device is available. No-op unless SQUIROLL_D3D_PROBE=1.
void d3d_probe_arm();

#endif
