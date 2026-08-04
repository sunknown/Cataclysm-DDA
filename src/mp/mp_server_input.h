#pragma once
#ifndef CATA_SRC_MP_MP_SERVER_INPUT_H
#define CATA_SRC_MP_MP_SERVER_INPUT_H

#include <string>

#include "input_enums.h"

class mp_transport;

// The largest terminal a peer may declare, in either dimension. The server's
// screen grid multiplies width by height (mp_screen_grid::resize), so an
// unbounded peer value would make it try to allocate the product of two 31-bit
// ints -- roughly 2^62 cells -- and abort on std::bad_alloc before the
// exception can be caught. Real terminals live far below this cap: 1000x1000
// cells already exceeds every device the game runs on. The cap exists purely
// to bound what a hostile or buggy peer can make the server allocate; a client
// that genuinely needs more room is a non-existent client.
constexpr int MP_MAX_TERMINAL_WIDTH = 1000;
constexpr int MP_MAX_TERMINAL_HEIGHT = 1000;

// Performs the connection handshake on a freshly-accepted transport: reads the
// client's first message (a `hello` control), checks its protocol version and
// the terminal size it wants, and sends a `hello_ack` verdict either way.
//
// Returns true and sizes the screen grid when the client is accepted, in which
// case `reject_reason` is left empty. Returns false on any refusal -- a silent
// client, a message that is not a parseable `hello`, a protocol mismatch, or a
// terminal size outside the accepted range (too small to run the game, or too
// large for the server to allocate) -- with a human-readable reason in
// `reject_reason` that is also sent to the client in the ack.
bool mp_server_handshake( mp_transport &transport, std::string &reject_reason );

// Blocks until the connected player presses something, a resize arrives, the
// timeout expires, or the connection drops. Flushes the pending screen first:
// this is the single point at which the player sees what the game drew.
//
// `timeout_ms` of a negative value blocks until one of those happens. Returns
// an event of type `timeout` when nothing arrived and when the wait ended on a
// resize -- the caller tells the two apart with mp_server_resize_pending().
input_event mp_server_wait_for_input( int timeout_ms );

// Consumes whatever has already arrived without blocking. Keystrokes seen here
// are buffered for the next mp_server_wait_for_input().
void mp_server_pump();

// Reports a resize the client asked for, once. The curses backend acts on it:
// it resizes the grid and rebuilds the game's windows.
bool mp_server_take_resize_request( int &width, int &height );

// Whether a resize is recorded and still waiting to be taken. Lets the curses
// backend tell "the wait ended because the client resized" from an ordinary
// timeout without consuming the request.
bool mp_server_resize_pending();

#endif // CATA_SRC_MP_MP_SERVER_INPUT_H
