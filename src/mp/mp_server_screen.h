#pragma once
#ifndef CATA_SRC_MP_MP_SERVER_SCREEN_H
#define CATA_SRC_MP_MP_SERVER_SCREEN_H

#include <cstddef>
#include <memory>
#include <vector>

#include "mp/mp_cell.h"

class mp_transport;

// A client that stops draining its socket leaves screen updates piling up in
// the transport's outbound buffer. Past this many undelivered bytes the
// history is not worth replaying -- screen state is idempotent, so the server
// drops the queued updates and sends one full redraw of the current screen
// instead, letting the client converge instead of lagging behind forever.
// Exposed so tests can fill the backlog against the real limit rather than
// guessing how many flushes it takes to cross it.
constexpr size_t MP_SCREEN_BACKLOG_BYTES = 512u * 1024u;

// The active session's screen grid. Returns a fallback grid when no session is
// active (startup, tests without a session) so the game still has a surface to
// size stdscr from.
mp_screen_grid &mp_server_grid();

void mp_server_set_terminal_size( int width, int height );

// True while the connected client is still present. False when no transport
// was ever attached or the peer closed it; the curses backend uses this to end
// the session instead of feeding the game an endless stream of timeout events.
bool mp_server_peer_connected();

// Writes `cells` starting at (x0, y). Cells past the right edge are dropped,
// matching curses clipping. Does nothing when no session is active.
void mp_server_blit_row( int x0, int y, const std::vector<mp_cell> &cells );

// Records where the client should draw its cursor; sent with the next flush.
// Does nothing when no session is active.
void mp_server_set_cursor( int x, int y, bool visible );

// Encodes and sends the dirty rows and the cursor, then clears the dirty
// flags. Does nothing when nothing changed or no session/transport is active.
void mp_server_flush_screen();

// Drops all state. Tests only -- these modules hold process-wide state because
// the curses backend they serve is itself a set of free functions.
void mp_server_reset_for_test();

// Internal: the active session's transport, or nullptr when no session is
// active. Used by the input module to read from the wire.
std::shared_ptr<mp_transport> mp_server_shared_transport();

#endif // CATA_SRC_MP_MP_SERVER_SCREEN_H
