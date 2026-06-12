#pragma once

#ifndef EFT_FREER_LOG_H
#define EFT_FREER_LOG_H 1

// Diagnostic for the "crash on hit" UAF chain.
//
// The pattern (per IDB analysis 2026-05-23):
//   Ew::sEffect (g_ewEftGroupMgr at 0x4DB0C8) holds the live-effect-groups
//   vector at sEffect+0xEC/0xF0 (begin/end). The vector is shrunk ONLY by
//   Manbow__EwCEftGroupMgr__PruneAndDispatchCallbacks (sub_EC130) when a
//   group's count==0 || dead-flag. The cEftGroup destructor (sub_ECB00)
//   removes itself from a PARENT cEftGroup's child-vector (+336/+340 via
//   the +332 backptr) — NOT from sEffect's +0xEC vector. So any code path
//   that destructs a group via parent-cascade or direct operator_delete
//   leaves a stale pointer in sEffect+0xEC, and the next prune walk reads
//   a zeroed-out cEftGroup, NULL-derefs its vtable, and crashes — which we
//   currently absorb via crash_handler's universal NULL-skip + EXEC-at-NULL
//   recovery.
//
// This module hooks both functions. On every cEftGroup destructor entry,
// if we are NOT inside the prune walk, we scan sEffect+0xEC/0xF0 looking
// for `this`. A hit means we are the buggy freer; the return address is
// logged so the offending call site can be identified.

namespace eft_freer_log {

void install();

} // namespace eft_freer_log

#endif // EFT_FREER_LOG_H
