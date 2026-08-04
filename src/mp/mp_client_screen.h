#pragma once
#ifndef CATA_SRC_MP_MP_CLIENT_SCREEN_H
#define CATA_SRC_MP_MP_CLIENT_SCREEN_H

#include <cstdint>
#include <string>
#include <vector>

#include "input_enums.h"
#include "mp/mp_cell.h"

// The pure-logic half of the thin client: parsing the server's messages and
// turning them into a client screen grid, plus the colour-pair cache and the
// input encoding. Nothing here touches a terminal or a socket, so it is fully
// testable in the normal build.
//
// The other half -- the loop that owns the transport, drives input and paints
// the grid -- lives in mp_client_main.

// Splits a "host:port" string. Returns false for a missing colon, an empty
// host, a non-numeric port, or a port outside [1, 65535]; on success fills
// `host` and `port`.
bool mp_parse_host_port( const std::string &text, std::string &host, int &port );

// What mp_client_apply_message concluded about one inbound message.
enum class mp_client_event {
    none,     // nothing usable: malformed frame, non-control/non-screen kind, or unparseable JSON
    screen,   // the grid was updated and should be repainted
    cursor,   // the cursor position was updated
    rejected, // the server refused the connection; see state.reject_reason
    closed,   // the server announced it is leaving (a bye control)
};

// The client's mirror of server state: where the cursor should be drawn, and
// the outcome of the handshake.
struct mp_client_state {
    int cursor_x = 0;
    int cursor_y = 0;
    bool cursor_visible = false;
    std::string reject_reason;
    bool accepted = false;
};

// Applies one framed message from the wire to `screen`/`state`. Never throws
// on any input; a malformed frame, a non-control/non-screen kind, or an
// unparseable control body all read as mp_client_event::none. Returns the
// event class so the caller knows what to repaint.
mp_client_event mp_client_apply_message( const std::vector<uint8_t> &payload,
        mp_screen_grid &screen, mp_client_state &state );

// Wraps one keystroke as a framed control message ready to send to the server.
std::vector<uint8_t> mp_client_encode_input( const input_event &ev );

// Returns the curses colour-pair index to render (fg, bg) with, registering
// the pair on first sight. The cache is capped (see mp_client_screen.cpp);
// once it is full, every unknown combination degrades to a safe white-on-black
// pair rather than corrupting cursesport's pair table.
int mp_client_pair_for( uint8_t fg, uint8_t bg );

// Drops the colour-pair cache. Tests only: the cache is process-wide because
// it mirrors cursesport's process-wide pair table.
void mp_client_reset_pairs_for_test();

#endif // CATA_SRC_MP_MP_CLIENT_SCREEN_H
