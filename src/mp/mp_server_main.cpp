#if defined(MP_SERVER)

#include "mp/mp_server_main.h"

#include <memory>
#include <optional>
#include <string>

#include "debug.h"
#include "mp/mp_server_input.h"
#include "mp/mp_server_screen.h"
#include "mp/mp_socket.h"
#include "mp/mp_tcp_transport.h"
#include "mp/mp_transport.h"

int mp_server_main( const int port, const std::string &world_name )
{
    mp_socket_startup();

    // The world is loaded later by main() from the same cli_opts it handed us;
    // log it here so an operator can see what world the client is about to get.
    DebugLog( D_INFO, D_MAIN ) << "mp server: waiting for a client on port " << port
                               << " (world: " << world_name << ")";

    std::string err;
    std::optional<mp_listener> listener = mp_listener::bind( port, err );
    if( !listener ) {
        DebugLog( D_ERROR, D_MAIN ) << "mp server: cannot listen on port " << port << ": " << err;
        return 1;
    }

    // A server waits for its one client as long as it takes.
    std::shared_ptr<mp_transport> transport = listener->accept( -1 );
    if( !transport ) {
        DebugLog( D_ERROR, D_MAIN ) << "mp server: accepting the client failed";
        return 1;
    }

    std::string reject_reason;
    if( !mp_server_handshake( *transport, reject_reason ) ) {
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
