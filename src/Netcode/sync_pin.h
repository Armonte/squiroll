#pragma once
#include <stdint.h>

// SYNC-PRIMITIVE PIN — locks are wall-clock (time-axis) state, like the CRT
// stack: they must NEVER roll back. th155 embeds CRITICAL_SECTIONs and
// std::mutex (_Mtx_internal_imp_t) INSIDE objects that live in the snapshot
// arenas (ScriptAPI signal locks, the sound-stream ring buffer's mutex, every
// boost::signals2 connection mutex). Restore step-0 made rollback complete
// enough to revert them -> a reverted lock (stale owner/count/SRWLOCK word)
// under a live thread => _Mtx_lock error -> C++ throw -> abort (the f~248
// streaming-slot FASTFAIL), or a silent deadlock.
//
// Fix: register every sync primitive at init time (IAT hooks on
// InitializeCriticalSection/AndSpinCount/Delete + safetyhook on the CRT's
// _Mtx_init_in_situ/_Mtx_destroy_in_situ), and around every rollback restore
// snapshot_ring re-applies each registered primitive's LIVE bytes so the
// restore never moves a lock through time.
namespace sync_pin {

struct Ent { uint32_t addr, len; };

void install();                       // hooks; call from common_init (early)
void forget_range(uint32_t lo, uint32_t hi);  // arena block freed -> drop entries

// Copy the live bytes of every registered primitive into buf; returns the
// number of entries written to out[] (each with its byte offset into buf
// implied by accumulation order). Thread-safe snapshot of the registry.
int snapshot_live(uint8_t* buf, uint32_t cap, Ent* out, int maxn);

} // namespace sync_pin
