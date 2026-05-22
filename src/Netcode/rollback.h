#pragma once

#ifndef ROLLBACK_H
#define ROLLBACK_H 1

#define ROLLBACK_MAX_FRAMES 8
#define ROLLBACK_FRAME_MASK (ROLLBACK_MAX_FRAMES - 1)

void rollback_start();
void rollback_stop();
void rollback_preframe();
void rollback_postframe();
void rollback_rewind(size_t frames);

// Layer-4 diagnostic: identify the diverging Squirrel object given the
// forward / re-sim sq_arena blobs and the first diverging byte offset.
void rollback_identify_sqdiff(const void* fwd_blob, const void* resim_blob,
                              uint32_t blob_len, uint32_t diff_off);

#endif
