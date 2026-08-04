#pragma once
#ifndef CATA_SRC_MP_MP_SERVER_INPUT_H
#define CATA_SRC_MP_MP_SERVER_INPUT_H

#include "input_enums.h"

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
