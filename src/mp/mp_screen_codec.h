#pragma once
#ifndef CATA_SRC_MP_MP_SCREEN_CODEC_H
#define CATA_SRC_MP_MP_SCREEN_CODEC_H

#include <cstdint>
#include <vector>

class mp_screen_grid;

// Wire format of one screen update. All integers are big-endian.
//
//   u16 width, u16 height        -- of the sender's screen, for validation
//   u16 row_count
//   per row:
//     u16 row_index
//     u16 run_count
//     per run:
//       u16 repeat
//       u8  fg, u8 bg
//       u8  glyph_len, glyph_len bytes of UTF-8 (0 means a continuation cell)
//
// Only rows the grid reports as dirty are included. The caller is responsible
// for calling clear_dirty() afterwards -- encoding does not mutate the grid,
// so a failed send can simply be retried.
std::vector<uint8_t> mp_screen_encode( const mp_screen_grid &grid );

// Applies an encoded update to `target`. Returns false and leaves the target
// untouched when the payload is malformed or describes a different screen
// size; the caller must then drop the connection or request a full redraw.
bool mp_screen_apply( mp_screen_grid &target, const std::vector<uint8_t> &payload );

#endif // CATA_SRC_MP_MP_SCREEN_CODEC_H
