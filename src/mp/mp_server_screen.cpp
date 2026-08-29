#include "mp/mp_server_screen.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mp/mp_protocol.h"
#include "mp/mp_screen_codec.h"
#include "mp/mp_session.h"
#include "mp/mp_server_log.h"
#include "mp/mp_transport.h"

// Defined in mp_server_input.cpp; the screen reset drops the input state too.
void mp_server_input_reset_for_test();

namespace
{

// Before any client is attached (startup, tests without a session) the game
// still asks for a grid to size stdscr from. The fallback keeps that path
// alive without pretending a session exists.
mp_screen_grid fallback_grid( 80, 24 );

} // namespace

mp_screen_grid &mp_server_grid()
{
    mp_session *const s = mp_active_session();
    return s ? s->grid : fallback_grid;
}

void mp_server_set_terminal_size( const int width, const int height )
{
    // resize() blanks the screen and marks every row dirty, so the next flush
    // always carries a full redraw.
    mp_server_grid().resize( width, height );
}

bool mp_server_peer_connected()
{
    if( mp_sessions().empty() ) {
        return false;
    }
    for( const std::unique_ptr<mp_session> &s : mp_sessions() ) {
        if( !s || !s->transport || !s->transport->connected() ) {
            // The roster is fixed: one player leaving ends the session for
            // everybody. Keeping the others playing would mean parking the
            // absent context and handing their character to the AI, which is
            // Phase 5's job, not a half-version of it here.
            return false;
        }
    }
    return true;
}

void mp_server_blit_row( const int x0, const int y, const std::vector<mp_cell> &cells )
{
    // Through mp_server_grid(), never through the session's grid directly: a
    // write has to land in the grid the matching read comes from. Reads already
    // fall back to fallback_grid when no session is active, so an early return
    // here made writes and reads disagree -- the row arrived, was dropped, and
    // the reader got a blank cell back out of a grid nobody had written to.
    mp_screen_grid &grid = mp_server_grid();
    for( size_t i = 0; i < cells.size(); ++i ) {
        // set() ignores out-of-range writes, which is exactly the clipping
        // curses applies when a window is drawn past the edge of the screen.
        grid.set( x0 + static_cast<int>( i ), y, cells[i] );
    }
}

void mp_server_set_cursor( const int x, const int y, const bool visible )
{
    mp_session *const s = mp_active_session();
    if( s == nullptr ) {
        // Deliberately session-only, unlike the blit above. The asymmetry that
        // bug was is a write with no matching read; the cursor has no read at
        // all without a session. Its only reader is mp_server_flush_screen(),
        // which needs a transport and therefore a session, and nothing else in
        // the tree reads cursor_x/cursor_y/cursor_visible. A fallback cursor
        // would be state that can never be observed -- dead by construction --
        // so there is nothing here to keep symmetric.
        return;
    }
    if( s->cursor_x == x && s->cursor_y == y && s->cursor_visible == visible ) {
        return;
    }
    s->cursor_x = x;
    s->cursor_y = y;
    s->cursor_visible = visible;
    s->cursor_changed = true;
}

void mp_server_flush_screen()
{
    mp_session *const s = mp_active_session();
    if( s == nullptr ) {
        return;
    }
    std::shared_ptr<mp_transport> t = s->transport;
    if( !t ) {
        return;
    }
    if( t->pending_bytes() > MP_SCREEN_BACKLOG_BYTES ) {
        // The peer is not keeping up. Throw away the queued screen history
        // and mark the whole grid dirty so the send below becomes a single
        // full redraw; control messages survive the drop untouched.
        t->drop_pending_screen_messages();
        s->grid.mark_all_dirty();
    }
    if( s->grid.any_dirty() ) {
        t->send( mp_wrap( mp_msg_kind::screen, mp_screen_encode( s->grid ) ) );
        s->grid.clear_dirty();
    }
    if( s->cursor_changed ) {
        mp_control msg;
        msg.kind = mp_control_kind::cursor;
        msg.cursor.x = s->cursor_x;
        msg.cursor.y = s->cursor_y;
        msg.cursor.visible = s->cursor_visible;
        const std::string json = mp_control_to_json( msg );
        t->send( mp_wrap( mp_msg_kind::control,
                          std::vector<uint8_t>( json.begin(), json.end() ) ) );
        s->cursor_changed = false;
    }
}

void mp_server_reset_for_test()
{
    fallback_grid.resize( 80, 24 );
    mp_server_input_reset_for_test();
    mp_server_log_reset_for_test();
}

std::shared_ptr<mp_transport> mp_server_shared_transport()
{
    mp_session *const s = mp_active_session();
    if( s == nullptr ) {
        return nullptr;
    }
    return s->transport;
}
