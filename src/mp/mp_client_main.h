#pragma once
#ifndef CATA_SRC_MP_MP_CLIENT_MAIN_H
#define CATA_SRC_MP_MP_CLIENT_MAIN_H

#include <string>

// The terminal-and-socket half of the thin client. Parses `host_and_port`
// ("host:port"), connects, initializes the curses interface, performs the
// handshake, then loops: draining keystrokes to the server and painting the
// server's screen messages as they arrive.
//
// Returns 0 when the session ends cleanly; a nonzero status means the client
// could not start (unparseable address, connect failure, or a handshake
// rejection). The caller (main()) forks into this function before the normal
// single-player init block, which this function drives itself via
// catacurses::init_interface().
int mp_client_main( const std::string &host_and_port );

#endif // CATA_SRC_MP_MP_CLIENT_MAIN_H
