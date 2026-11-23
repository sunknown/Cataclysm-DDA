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

// Blanks every connected session's screen and marks it fully dirty. The join
// menu paints into the session grid without a window that survives the join
// phase, so those cells would otherwise ghost over the running game until some
// later window happened to repaint them. Every seat also leaves the UI here,
// which clears its in-game flag and any hover request, so the join menu's state
// does not survive into play. Called once when the party starts.
void mp_server_blank_screens();

// True while the connected client is still present. False when no transport
// was ever attached or the peer closed it; the curses backend uses this to end
// the session instead of feeding the game an endless stream of timeout events.
bool mp_server_peer_connected();

// Writes `cells` starting at (x0, y). Cells past the right edge are dropped,
// matching curses clipping. Writes go to the same grid mp_server_grid() reads
// from, so with no session active they land in the fallback grid rather than
// being discarded.
void mp_server_blit_row( int x0, int y, const std::vector<mp_cell> &cells );

// Records where the client should draw its cursor; sent with the next flush.
// Does nothing when no session is active -- unlike the grid, cursor state has
// no reader without one, so there is no fallback to keep it in.
void mp_server_set_cursor( int x, int y, bool visible );

// Encodes and sends the dirty rows and the cursor, then clears the dirty
// flags. A client that negotiated the log stream also gets its snapshot, and
// one that negotiated the scene stream gets a frame from
// mp_server_flush_scene() after the grid, so the screen frame precedes the
// scene frame that belongs with it. A due hover answer is sent last, after
// that scene frame, so it can name the frame the client is about to draw.
// Does nothing when no session or transport is active.
void mp_server_flush_screen();

// Sends one scene frame if this seat negotiated the capability, holds the baton
// and the scene changed since the last send. Silent on every other path,
// including the world phase and the legacy no-scheduler path.
void mp_server_flush_scene();

// Drops all state. Tests only -- these modules hold process-wide state because
// the curses backend they serve is itself a set of free functions.
void mp_server_reset_for_test();

// Internal: the active session's transport, or nullptr when no session is
// active. Used by the input module to read from the wire.
std::shared_ptr<mp_transport> mp_server_shared_transport();

#endif // CATA_SRC_MP_MP_SERVER_SCREEN_H
