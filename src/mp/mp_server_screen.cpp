#include "mp/mp_server_screen.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mp/mp_protocol.h"
#include "mp/mp_screen_codec.h"
#include "mp/mp_transport.h"

// Defined in mp_server_input.cpp; the screen reset drops the input state too.
void mp_server_input_reset_for_test();

namespace
{

// A client that stops draining its socket leaves screen updates piling up in
// the transport's outbound buffer. Past this many undelivered bytes the
// history is not worth replaying -- screen state is idempotent, so the server
// drops the queued updates and sends one full redraw of the current screen
// instead, letting the client converge instead of lagging behind forever.
// Kept here rather than in the header because only this translation unit uses
// it.
constexpr size_t MP_SCREEN_BACKLOG_BYTES = 512u * 1024u;

// Process-wide state: the game is single-threaded around these calls, and the
// curses backend these modules serve is itself a set of free functions. The
// default size is arbitrary -- the game resizes the grid when a client
// connects and the reset returns it to a fresh all-dirty screen.
mp_screen_grid g_grid( 80, 24 );
std::shared_ptr<mp_transport> g_transport;

int g_cursor_x = 0;
int g_cursor_y = 0;
bool g_cursor_visible = false;
bool g_cursor_changed = false;

} // namespace

mp_screen_grid &mp_server_grid()
{
    return g_grid;
}

void mp_server_set_terminal_size( const int width, const int height )
{
    // resize() blanks the screen and marks every row dirty, so the next flush
    // always carries a full redraw.
    g_grid.resize( width, height );
}

void mp_server_set_transport( std::shared_ptr<mp_transport> transport )
{
    g_transport = std::move( transport );
}

bool mp_server_peer_connected()
{
    return g_transport && g_transport->connected();
}

void mp_server_blit_row( const int x0, const int y, const std::vector<mp_cell> &cells )
{
    for( size_t i = 0; i < cells.size(); ++i ) {
        // set() ignores out-of-range writes, which is exactly the clipping
        // curses applies when a window is drawn past the edge of the screen.
        g_grid.set( x0 + static_cast<int>( i ), y, cells[i] );
    }
}

void mp_server_set_cursor( const int x, const int y, const bool visible )
{
    if( g_cursor_x == x && g_cursor_y == y && g_cursor_visible == visible ) {
        return;
    }
    g_cursor_x = x;
    g_cursor_y = y;
    g_cursor_visible = visible;
    g_cursor_changed = true;
}

void mp_server_flush_screen()
{
    if( !g_transport ) {
        return;
    }
    if( g_transport->pending_bytes() > MP_SCREEN_BACKLOG_BYTES ) {
        // The peer is not keeping up. Throw away the queued screen history
        // and mark the whole grid dirty so the send below becomes a single
        // full redraw; control messages survive the drop untouched.
        g_transport->drop_pending_screen_messages();
        g_grid.mark_all_dirty();
    }
    if( g_grid.any_dirty() ) {
        g_transport->send( mp_wrap( mp_msg_kind::screen, mp_screen_encode( g_grid ) ) );
        g_grid.clear_dirty();
    }
    if( g_cursor_changed ) {
        mp_control msg;
        msg.kind = mp_control_kind::cursor;
        msg.cursor.x = g_cursor_x;
        msg.cursor.y = g_cursor_y;
        msg.cursor.visible = g_cursor_visible;
        const std::string json = mp_control_to_json( msg );
        g_transport->send( mp_wrap( mp_msg_kind::control,
                                    std::vector<uint8_t>( json.begin(), json.end() ) ) );
        g_cursor_changed = false;
    }
}

void mp_server_reset_for_test()
{
    g_grid.resize( 80, 24 );
    g_transport.reset();
    g_cursor_x = 0;
    g_cursor_y = 0;
    g_cursor_visible = false;
    g_cursor_changed = false;
    mp_server_input_reset_for_test();
}

std::shared_ptr<mp_transport> &mp_server_shared_transport()
{
    return g_transport;
}
