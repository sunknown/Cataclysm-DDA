#pragma once
#ifndef CATA_SRC_MP_MP_SERVER_MAIN_H
#define CATA_SRC_MP_MP_SERVER_MAIN_H

#include <string>

// Binds a TCP listener on `port`, waits for one client, performs the
// connection handshake, attaches the client's transport to the server modules,
// and initializes the curses interface sized to the client's terminal.
//
// Returns 0 when the server is ready for the game to start; a nonzero status
// means the server could not start and the game must not run. The world the
// client will play is `world_name`; the caller launches the game loop itself
// after this returns successfully.
int mp_server_main( int port, const std::string &world_name );

#endif // CATA_SRC_MP_MP_SERVER_MAIN_H
