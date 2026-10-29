#include "mp/mp_client_main.h"

#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "color.h"
#include "cursesdef.h"
#include "debug.h"
#include "game_constants.h"
#include "input.h"
#include "mapsharing.h"
#include "mp/mp_cell.h"
#include "mp/mp_character_card.h"
#include "mp/mp_client_screen.h"
#include "mp/mp_protocol.h"
#include "mp/mp_tcp_transport.h"
#include "mp/mp_transport.h"
#include "point.h"

namespace
{

// Writes the grid's dirty rows into the real terminal, one run of
// same-coloured glyphs at a time (a full redraw is a handful of calls, not one
// per cell), then places the cursor and flushes. The empty glyph of a
// double-width character's second column terminates a run so the accumulated
// text is always written at its own start column.
void mp_client_paint( mp_screen_grid &screen, const mp_client_state &state )
{
    for( const int y : screen.dirty_rows() ) {
        int x = 0;
        while( x < screen.width() ) {
            const mp_cell &cell = screen.at( x, y );
            if( cell.ch.empty() ) {
                // The second column of a double-width glyph; the first column
                // already drew it two cells wide.
                ++x;
                continue;
            }
            const int pair = mp_client_pair_for( cell.fg, cell.bg );
            const int run_start = x;
            std::string text = cell.ch;
            ++x;
            while( x < screen.width() ) {
                const mp_cell &next = screen.at( x, y );
                if( next.ch.empty() || mp_client_pair_for( next.fg, next.bg ) != pair ) {
                    break;
                }
                text += next.ch;
                ++x;
            }
            catacurses::wattron( catacurses::stdscr,
                                 nc_color::from_color_pair_index( pair ) );
            catacurses::mvwprintw( catacurses::stdscr, point( run_start, y ), text );
        }
    }
    screen.clear_dirty();

    catacurses::curs_set( state.cursor_visible ? 1 : 0 );
    if( state.cursor_visible ) {
        catacurses::wmove( catacurses::stdscr, point( state.cursor_x, state.cursor_y ) );
    }
    catacurses::wrefresh( catacurses::stdscr );
}

void send_control( mp_transport &transport, const mp_control &msg )
{
    const std::string json = mp_control_to_json( msg );
    transport.send( mp_wrap( mp_msg_kind::control,
                             std::vector<uint8_t>( json.begin(), json.end() ) ) );
}

// The server just told us who we are. Written immediately rather than at session
// end, so a crash does not cost the player their identity on this server.
void remember_character( const std::string &server, const mp_character_card_msg &msg )
{
    mp_character_card card;
    card.server = server;
    card.world_name = msg.world_name;
    card.save_id = msg.save_id;
    card.character_name = msg.character_name;
    if( !mp_save_character_card( card ) ) {
        DebugLog( D_WARNING, D_MAIN ) << "mp client: could not write the character card for "
                                      << server;
    }
}

} // namespace

int mp_client_main( const std::string &host_and_port )
{
    std::string host;
    int port = 0;
    if( !mp_parse_host_port( host_and_port, host, port ) ) {
        std::cerr << "mp client: cannot parse --connect address \"" << host_and_port << "\"" << std::endl;
        return 1;
    }

    std::string err;
    std::shared_ptr<mp_transport> transport = mp_tcp_connect( host, port, err );
    if( !transport ) {
        std::cerr << "mp client: cannot connect to " << host_and_port << ": " << err << std::endl;
        DebugLog( D_ERROR, D_MAIN ) << "mp client: cannot connect to " << host_and_port
                                    << ": " << err;
        return 1;
    }

    // The terminal must be up before we can report its size in the hello, and
    // before any screen message can be painted.
    try {
        catacurses::init_interface();
    } catch( const std::exception &e ) {
        std::cerr << "mp client: cannot initialize the interface: " << e.what() << std::endl;
        return 1;
    }

    const int term_w = catacurses::getmaxx( catacurses::stdscr );
    const int term_h = catacurses::getmaxy( catacurses::stdscr );
    mp_client_state state;
    mp_screen_grid screen( term_w, term_h );

    // Introduce ourselves: the protocol version, a name, the terminal size we
    // want the server to stream, and whether we can show 256 colours.
    mp_control hello;
    hello.kind = mp_control_kind::hello;
    hello.hello.proto = MP_PROTOCOL_VERSION;
    hello.hello.name = MAP_SHARING::getUsername().empty() ? "client" : MAP_SHARING::getUsername();
    // Who we played here last time, if anybody. The server decides what that is
    // worth; an empty or stale card just means being offered a new character.
    hello.hello.save_id = mp_load_character_card( host_and_port ).save_id;
    hello.hello.width = term_w;
    hello.hello.height = term_h;
    hello.hello.colors_256 = catacurses::supports_256_colors();
    send_control( *transport, hello );

    // Wait for the verdict. Anything that arrives before the ack is ignored; a
    // refusal prints the server's reason and stops the client.
    while( !state.accepted ) {
        const std::optional<std::vector<uint8_t>> msg = transport->recv( 5000 );
        if( !msg ) {
            std::cerr << "mp client: no reply from the server" << std::endl;
            return 1;
        }
        const mp_client_event ev = mp_client_apply_message( *msg, screen, state );
        if( ev == mp_client_event::rejected ) {
            std::cerr << "mp client: the server refused the connection: "
                      << state.reject_reason << std::endl;
            return 1;
        }
        if( ev == mp_client_event::closed ) {
            return 1;
        }
    }

    while( transport->connected() ) {
        // Outgoing first: a keystroke must never wait for a redraw.
        while( true ) {
            inp_mngr.set_timeout( 0 );
            const input_event ev = inp_mngr.get_input_event();
            if( ev.type == input_event_t::timeout || ev.type == input_event_t::error ) {
                break;
            }
            transport->send( mp_client_encode_input( ev ) );
        }

        // A local terminal resize asks the server to stream at the new size and
        // rebuilds the client grid to match. The curses backend already resized
        // stdscr (the existing handle_resize path feeds KEY_RESIZE through
        // catacurses::resizeterm()); this propagates the new dimensions.
        const int cur_w = catacurses::getmaxx( catacurses::stdscr );
        const int cur_h = catacurses::getmaxy( catacurses::stdscr );
        if( cur_w != screen.width() || cur_h != screen.height() ) {
            if( cur_w >= EVEN_MINIMUM_TERM_WIDTH && cur_h >= EVEN_MINIMUM_TERM_HEIGHT ) {
                mp_control resize;
                resize.kind = mp_control_kind::resize;
                resize.resize.width = cur_w;
                resize.resize.height = cur_h;
                send_control( *transport, resize );
                screen.resize( cur_w, cur_h );
                screen.mark_all_dirty();
            }
        }

        const std::optional<std::vector<uint8_t>> msg = transport->recv( 16 );
        if( msg ) {
            const mp_client_event ev = mp_client_apply_message( *msg, screen, state );
            if( ev == mp_client_event::screen ) {
                mp_client_paint( screen, state );
            } else if( ev == mp_client_event::character_card ) {
                remember_character( host_and_port, state.card );
            } else if( ev == mp_client_event::closed ) {
                break;
            }
        }
    }

    return 0;
}
