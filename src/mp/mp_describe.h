#pragma once
#ifndef CATA_SRC_MP_MP_DESCRIBE_H
#define CATA_SRC_MP_MP_DESCRIBE_H

#include <cstddef>
#include <vector>

#include "coordinates.h"
#include "mp/mp_cell.h"
#include "mp/mp_protocol.h"

// Phase 4b-2: the pure text rules of the hover-describe feed. A rendered
// tile-info cell grid becomes the lines the client draws; the server only
// decides which cells to read and where the answer goes.
//
// The feed answers a hover, so the caps bound what one request may cost the
// server and the connection.

constexpr int MP_DESCRIBE_MAX_LINES = 24;
// A per-line draw budget; the text byte cap is global, so one tall answer
// cannot crowd out the rest of the connection.
constexpr int MP_DESCRIBE_MAX_RUNS = 256;
constexpr int MP_DESCRIBE_MIN_WIDTH = 20;
constexpr int MP_DESCRIBE_MAX_WIDTH = 80;
constexpr int MP_DESCRIBE_MIN_INTERVAL_MS = 100;
constexpr size_t MP_DESCRIBE_MAX_TEXT_BYTES = 16 * 1024;

// Trims a row-major grid of `width * height` cells to the text a client
// draws: trailing blanks removed, the shared left indent removed, no blank
// rows at the edges, and runs split only where `fg` changes. `bg` never
// matters. An empty `ch` is the second column of a wide glyph and adds no
// text. Every cap truncates: no empty run and no padded line is emitted.
std::vector<mp_log_line> mp_describe_extract( const std::vector<mp_cell> &cells,
        int width, int height );

// Renders the game's own tile-info text for `tile` into a scratch window `width`
// columns wide and returns it as describe lines. The caller clamps `width`;
// this function does not. A scratch window that cannot be created yields no
// lines, so a failed render can never crash the request path.
//
// Server-only: the in-memory curses window and its cell accessor exist only in
// the MP_SERVER build.
#if defined( MP_SERVER )
std::vector<mp_log_line> mp_describe_lines( const tripoint_bub_ms &tile, int width );

// Builds the answer envelope for one hover request. `cell_x` and `cell_y` are
// absolute terminal cells; the active context's map window decides which of
// them exist, and `width` is clamped to
// [MP_DESCRIBE_MIN_WIDTH, MP_DESCRIBE_MAX_WIDTH] before it reaches `w` and the
// renderer. Every failure -- no game, no context, no map window, a cell
// outside it, or a tile outside the map -- returns the same envelope with
// empty `lines`, so the client always gets an answer for its latest seq.
mp_describe_msg mp_describe_reply( uint32_t seq, uint32_t frame,
                                   int cell_x, int cell_y, int width );
#endif

#endif // CATA_SRC_MP_MP_DESCRIBE_H
