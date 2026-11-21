#pragma once
#ifndef CATA_SRC_MP_MP_SERVER_MASK_H
#define CATA_SRC_MP_MP_SERVER_MASK_H

#include <cstdint>
#include <vector>

#include "cursesdef.h"
#include "mp/mp_scene.h"

// What the text UI covers inside the map window, one role per cell. The grid
// covers exactly the active context's terrain window; a terrain blit clears it
// and every other blit marks the cells it intersects. Rects are compressed to
// absolute terminal cells for the scene frame's mask list.

// Empties every cell. Called when the terrain window blits: the fresh map
// underneath has no text on it yet.
void mp_scene_mask_clear();

// Marks the cells of `x,y,w,h` that fall inside the map window with `role`.
// A rect intersecting nothing is ignored, as is every call with no active
// context or no terrain window.
void mp_scene_mask_note( uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t role );

// Remembers that this window's blits are a mask of `role` (1 = native log).
// The native pointer is held weakly, so a destroyed window can never inherit a
// stale role; an unknown or expired window is role 0.
void mp_scene_mask_register_role( const catacurses::window &win, uint8_t role );

// The role registered for `win`, or 0 when it was never registered.
uint8_t mp_scene_mask_role_for( const catacurses::window &win );

// The compressed mask list, in row-major order of the top-left corner. Cells
// inside the map window but outside every text mask are 3D only; role 0 is
// text over 3D and role 1 is the native log.
std::vector<mp_scene_rect> mp_scene_mask_rects();

// The active context's map window as one rect with `role` 0, in absolute
// terminal cells. Returns false when there is no map window or it has no
// area. This is the scene's viewport and map size; the same geometry the mask
// grid covers.
bool mp_scene_mask_map_rect( mp_scene_rect &out );

// FNV-1a 64-bit over `mp_scene_mask_rects()`, each rect's x, y, w, h and role
// as big-endian bytes. Equal lists hash equal.
uint64_t mp_scene_mask_hash();

#endif // CATA_SRC_MP_MP_SERVER_MASK_H
