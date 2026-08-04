#pragma once
#ifndef CATA_SRC_MP_MP_SERVER_SCREEN_H
#define CATA_SRC_MP_MP_SERVER_SCREEN_H

#include <memory>
#include <vector>

#include "mp/mp_cell.h"

class mp_transport;

// The server's composite screen for the connected player.
//
// Every curses window the game draws is blitted here instead of to a terminal.
// This module deliberately knows nothing about curses: it takes rows of cells,
// so the whole screen path is testable without the server build. Extracting
// rows from a cata_cursesport::WINDOW is the curses backend's job.

mp_screen_grid &mp_server_grid();

void mp_server_set_terminal_size( int width, int height );
void mp_server_set_transport( std::shared_ptr<mp_transport> transport );

// True while the connected client is still present. False when no transport
// was ever attached or the peer closed it; the curses backend uses this to end
// the session instead of feeding the game an endless stream of timeout events.
bool mp_server_peer_connected();

// Writes `cells` starting at (x0, y). Cells past the right edge are dropped,
// matching curses clipping.
void mp_server_blit_row( int x0, int y, const std::vector<mp_cell> &cells );

// Records where the client should draw its cursor; sent with the next flush.
void mp_server_set_cursor( int x, int y, bool visible );

// Encodes and sends the dirty rows and the cursor, then clears the dirty
// flags. Does nothing when nothing changed or no transport is attached.
void mp_server_flush_screen();

// Drops all state. Tests only -- these modules hold process-wide state because
// the curses backend they serve is itself a set of free functions.
void mp_server_reset_for_test();

// Internal: the transport both modules share. The input module waits on this
// same wire and closes it when the peer says bye, so it is declared here
// rather than in a private header the two modules would both have to include.
std::shared_ptr<mp_transport> &mp_server_shared_transport();

#endif // CATA_SRC_MP_MP_SERVER_SCREEN_H
