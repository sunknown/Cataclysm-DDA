#if defined(MP_SERVER)

#include "mp/mp_server_main.h"

#include <memory>
#include <optional>
#include <string>

#include "debug.h"
#include "mp/mp_protocol.h"
#include "mp/mp_server_input.h"
#include "mp/mp_server_log.h"
#include "mp/mp_server_screen.h"
#include "mp/mp_socket.h"
#include "mp/mp_tcp_transport.h"
#include "mp/mp_transport.h"

int mp_server_main( const int port, const std::string &world_name )
{
    mp_socket_startup();

    // Everything from here on is also the operator's console log: the server
    // has no screen of its own, and until this point it started up silently.
    mp_server_log_enable();

    std::string err;
    std::optional<mp_listener> listener = mp_listener::bind( port, err );
    if( !listener ) {
        mp_server_log( "cannot listen on port " + std::to_string( port ) + ": " + err );
        DebugLog( D_ERROR, D_MAIN ) << "mp server: cannot listen on port " << port << ": " << err;
        return 1;
    }

    // The world is loaded later by main() from the same cli_opts it handed us;
    // name it here so an operator can see what the client is about to get. An
    // empty name means main() will offer the world-selection menu instead.
    mp_server_log( "listening on port " + std::to_string( port ) + " (world: " +
                   ( world_name.empty() ? "chosen by the client" : "\"" + world_name + "\"" ) +
                   ", protocol " + std::to_string( MP_PROTOCOL_VERSION ) + ")" );
    mp_server_log( "waiting for a client" );

    // A server waits for its one client as long as it takes.
    std::shared_ptr<mp_transport> transport = listener->accept( -1 );
    if( !transport ) {
        mp_server_log( "accepting the client failed" );
        DebugLog( D_ERROR, D_MAIN ) << "mp server: accepting the client failed";
        return 1;
    }

    // The accepted line comes before the handshake so a peer that then fails
    // the handshake is still attributable to an address.
    mp_server_log( "connection from " + transport->peer_description() + "; shaking hands" );

    std::string reject_reason;
    if( !mp_server_handshake( *transport, reject_reason ) ) {
        mp_server_log( "handshake rejected: " + reject_reason );
        DebugLog( D_ERROR, D_MAIN ) << "mp server: handshake rejected: " << reject_reason;
        return 1;
    }

    // From here on the input and screen modules own the wire. The curses
    // interface is initialized by main() after this returns: it falls through
    // to the same init block as single-player, which sets the fixed minimum
    // FULL_SCREEN sizes and calls catacurses::init_interface() -- and the mp
    // curses backend sizes stdscr from the grid the handshake set up.
    mp_server_set_transport( transport );

    return 0;
}

#endif // MP_SERVER
