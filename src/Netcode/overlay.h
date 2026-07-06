#pragma once

#ifndef OVERLAY_H
#define OVERLAY_H 1

#include <cstdint>
#include "TF4.h"

void overlay_init();
void overlay_destroy();

void overlay_set_hitboxes(ManbowActor2DGroup* group, int p1_flags, int p2_flags);
void overlay_clear();
void overlay_draw();
int debug(ManbowActor2D* player);

// ---------------------------------------------------------------------------
// Native immediate-mode HUD API (rollback-safe by construction). A plugin
// queues text/rect draw commands each Update (via ::hud.* in Squirrel);
// overlay_draw() expands the queue into the overlay's OWN D3D quad pipeline on
// the render thread. The queue (a std::vector on the normal DLL heap) and the
// font (a static const array) are NATIVE — never in any snapshot arena
// (sq/cpp/render/tf4) — so a rollback has literally nothing here to rewind, and
// no th155 String/DrawCommandSlot/glyph/font machinery is ever touched.
//
// Immediate mode / forward-only: the plugin calls hud_clear() at the top of its
// Update and re-emits the whole HUD every frame; overlay_draw renders whatever
// is queued and does NOT clear it (so the last emitted frame persists across
// re-sim frames = no flicker). hud_clear() must also be called on battle
// teardown so a finished match's HUD does not linger.
//
//   rgba  : packed 0xAARRGGBB (matches the hitbox shader decode_color()).
//   x/y/w/h : 1280x720 game-HUD space, origin top-left (inverse of the
//             overlay's clip_to_screen()).
//   scale : font pixels -> screen pixels for text (scale=2 -> 16x16 glyphs).
//           x advances 8*scale per char (monospace).
void hud_clear();
void hud_text(float x, float y, const char* str, uint32_t rgba, float scale);
void hud_rect(float x, float y, float w, float h, uint32_t rgba);

// Flush the D3D immediate context and BLOCK until the GPU has finished all
// prior work (event-query spin). Called before a rollback restore rewrites
// tf4-mspace so the NVIDIA driver's async command threads aren't still
// dereferencing that memory (task #28). Cheap when the GPU is already idle.
// Same thread as render (better_game_loop drives tick + present).
void gpu_sync_idle();

#endif
