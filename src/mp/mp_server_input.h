#pragma once
#ifndef CATA_SRC_MP_MP_SERVER_INPUT_H
#define CATA_SRC_MP_MP_SERVER_INPUT_H

#include <string>

#include "input_enums.h"

class mp_transport;

// Performs the connection handshake on a freshly-accepted transport: reads the
// client's first message (a `hello` control), checks its protocol version and
// the terminal size it wants, and sends a `hello_ack` verdict either way.
//
// Returns true and sizes the screen grid when the client is accepted, in which
// case `reject_reason` is left empty. Returns false on any refusal -- a silent
// client, a message that is not a parseable `hello`, a protocol mismatch, or a
// terminal smaller than the game minimum -- with a human-readable reason in
// `reject_reason` that is also sent to the client in the ack.
bool mp_server_handshake( mp_transport &transport, std::string &reject_reason );

// Blocks until the connected player presses something, the timeout expires, or
// the connection drops. Flushes the pending screen first: this is the single
// point at which the player sees what the game drew.
//
// `timeout_ms` of a negative value blocks until input arrives or the transport
// goes away. Returns an event of type `timeout` when nothing arrived.
input_event mp_server_wait_for_input( int timeout_ms );

// Consumes whatever has already arrived without blocking. Keystrokes seen here
// are buffered for the next mp_server_wait_for_input().
void mp_server_pump();

// Reports a resize the client asked for, once. The curses backend acts on it:
// it resizes the grid and rebuilds the game's windows.
bool mp_server_take_resize_request( int &width, int &height );

#endif // CATA_SRC_MP_MP_SERVER_INPUT_H
