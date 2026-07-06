#pragma once

#ifndef OVERLAY_H
#define OVERLAY_H 1

#include "TF4.h"

void overlay_init();
void overlay_destroy();

void overlay_set_hitboxes(ManbowActor2DGroup* group, int p1_flags, int p2_flags);
void overlay_clear();
void overlay_draw();
int debug(ManbowActor2D* player);

// Flush the D3D immediate context and BLOCK until the GPU has finished all
// prior work (event-query spin). Called before a rollback restore rewrites
// tf4-mspace so the NVIDIA driver's async command threads aren't still
// dereferencing that memory (task #28). Cheap when the GPU is already idle.
// Same thread as render (better_game_loop drives tick + present).
void gpu_sync_idle();

#endif
