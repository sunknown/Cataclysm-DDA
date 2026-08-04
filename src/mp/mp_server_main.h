#pragma once
#ifndef CATA_SRC_MP_MP_SERVER_MAIN_H
#define CATA_SRC_MP_MP_SERVER_MAIN_H

#include <string>

// Binds a TCP listener on `port`, waits for one client, performs the
// connection handshake, and attaches the client's transport to the server
// modules. The handshake sizes the screen grid to the client's terminal; the
// caller (main()) then initializes the curses interface, which the mp curses
// backend builds from that grid.
//
// Returns 0 when the server is ready for the game to start; a nonzero status
// means the server could not start and the game must not run. The world the
// client will play is `world_name`; the caller launches the game loop itself
// after this returns successfully.
int mp_server_main( int port, const std::string &world_name );

// Ends the server session cleanly. Called by the mp curses backend when the
// connected client is gone, so the game does not spin on timeout events from a
// dead transport. Defined in main.cpp, which owns the game's exit path; this
// declaration lets the mp layer (which must not know the game's internals)
// ask the entry to tear the whole process down.
void mp_server_request_exit();

#endif // CATA_SRC_MP_MP_SERVER_MAIN_H
