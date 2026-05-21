#pragma once

#ifndef ALLOCMAN_H
#define ALLOCMAN_H 1

#include <stdint.h>
#include <stdlib.h>
#include <bit>

#include "util.h"

#define PATCH_NO_ALLOCS 0
#define PATCH_SQUIRREL_ALLOCS 1
#define PATCH_ALL_ALLOCS 2

#define ALLOCATION_PATCH_TYPE PATCH_SQUIRREL_ALLOCS

#define MEMORY_DEBUG_NONE 0
#define MEMORY_DEBUG_LIGHT 1
#define MEMORY_DEBUG_HEAVY 2

#define MEMORY_DEBUG_LEVEL MEMORY_DEBUG_LIGHT


#if ALLOCATION_PATCH_TYPE != PATCH_NO_ALLOCS

static inline constexpr size_t SAVED_FRAMES = 8;
static_assert(std::has_single_bit(SAVED_FRAMES));

void tick_allocs();
size_t fastcall rollback_allocs(size_t frames);
void reset_rollback_buffers();

void update_allocs();

void* cdecl my_malloc(size_t size);
void* cdecl my_calloc(size_t num, size_t size);
void cdecl my_free(void* ptr);
size_t cdecl my_msize(void* ptr);
void* cdecl my_expand(void* ptr, size_t new_size);
void* cdecl my_realloc(void* ptr, size_t new_size);
void* cdecl my_recalloc(void* ptr, size_t num, size_t size);

void patch_allocman();

// --- giuroll-style Squirrel VM heap snapshot (GekkoNet rollback) -----------
//
// Every Squirrel allocation is tracked on a global live list (see my_malloc).
// sq_heap snapshots that whole list as raw memory — a lossless capture of the
// VM object graph (closures, weakrefs, deep tables: all of it, since they are
// all allocations). This replaces the lossy text walker for rollback
// save/load. While armed, frees are DEFERRED so every object keeps a stable
// address across the rollback window — raw pointers in the snapshot then
// stay valid on restore.
namespace sq_heap {
    // Arm/disarm. While armed my_free defers instead of freeing.
    void set_armed(bool on);
    // Hard-free every deferred block. Call at disarm.
    void flush();
    // One committed real frame elapsed: advance the cleanup clock and
    // hard-free blocks that have been dead longer than the rollback window.
    void on_advance();
    // Serialize every live Squirrel allocation into out[0..cap). Returns
    // bytes written, 0 on overflow.
    uint32_t save(uint8_t* out, uint32_t cap);
    // Restore from a save() blob: memcpy each alloc back, resurrect deferred
    // blocks the blob names, hard-free blocks born after the saved frame.
    void load(const uint8_t* blob, uint32_t len);
}
#endif

#endif
