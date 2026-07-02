#pragma once

#ifndef BETTER_GAME_LOOP_H
#define BETTER_GAME_LOOP_H 1

void init_better_game_loop();

// The measured fps GetFPS() reads (th155 sim derives dt = 1/fps from it).
// advance_one_frame freezes this to a fixed 60 so a rollback re-sim computes
// the same dt as the original forward pass. See better_game_loop.cpp.
uint32_t sim_get_fps();
void     sim_set_fps(uint32_t v);

#endif