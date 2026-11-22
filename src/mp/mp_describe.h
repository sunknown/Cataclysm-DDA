#pragma once
#ifndef CATA_SRC_MP_MP_DESCRIBE_H
#define CATA_SRC_MP_MP_DESCRIBE_H

#include <cstddef>
#include <vector>

#include "mp/mp_cell.h"
#include "mp/mp_protocol.h"

// Phase 4b-2: the pure text rules of the hover-describe feed. A rendered
// tile-info cell grid becomes the lines the client draws; the server only
// decides which cells to read and where the answer goes.
//
// The feed answers a hover, so the caps bound what one request may cost the
// server and the connection.

constexpr int MP_DESCRIBE_MAX_LINES = 24;
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

#endif // CATA_SRC_MP_MP_DESCRIBE_H
