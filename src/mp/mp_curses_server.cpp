#if defined(MP_SERVER)

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cata_utility.h"
#include "coordinates.h"
#include "cuboid_rectangle.h"
#include "cursesdef.h"
#include "cursesport.h"
#include "cata_imgui.h"
#include "game_constants.h"
#include "game_ui.h"
#include "input.h"
#include "input_context.h"
#include "map.h"
#include "mp/mp_cell.h"
#include "mp/mp_context.h"
#include "mp/mp_sched.h"
#include "mp/mp_server_input.h"
#include "mp/mp_server_log.h"
#include "mp/mp_server_main.h"
#include "mp/mp_server_screen.h"
#include "mp/mp_turn.h"
#include "output.h"
#include "point.h"
#include "ui_manager.h"

// The fourth curses backend. Instead of a terminal it drives the composite
// screen of mp_server_screen; instead of a keyboard it reads the player's
// transport through mp_server_input.

// The imgui client the game drives (declared in ui_manager.h). The ncurses
// backend defines it too; that file is excluded from the server build, so this
// is the one the link resolves.
std::unique_ptr<cataimgui::client> imclient;

namespace
{
// The in-memory curses stores the line-drawing glyphs as single bytes from a
// private code page (output.h's LINE_*_C constants, 0xa0-0xaa). They are not
// valid UTF-8, so a window border would make the screen codec reject the whole
// frame on the client. Translate them to the box-drawing characters they stand
// for before they reach the grid. Everything else (empty continuation cells,
// ordinary text) passes through unchanged.
std::string mp_translate_line_glyph( const std::string &ch )
{
    if( ch.size() != 1 ) {
        return ch;
    }
    static constexpr std::array<std::string_view, 11> glyphs = { {
            "\xE2\x94\x82", // U+2502 │ LINE_XOXO_C 0xa0
            "\xE2\x94\x80", // U+2500 ─ LINE_OXOX_C 0xa1
            "\xE2\x94\x94", // U+2514 └ LINE_XXOO_C 0xa2
            "\xE2\x94\x8C", // U+250C ┌ LINE_OXXO_C 0xa3
            "\xE2\x94\x90", // U+2510 ┐ LINE_OOXX_C 0xa4
            "\xE2\x94\x98", // U+2518 ┘ LINE_XOOX_C 0xa5
            "\xE2\x94\x9C", // U+251C ├ LINE_XXXO_C 0xa6
            "\xE2\x94\xB4", // U+2534 ┴ LINE_XXOX_C 0xa7
            "\xE2\x94\xA4", // U+2524 ┤ LINE_XOXX_C 0xa8
            "\xE2\x94\xAC", // U+252C ┬ LINE_OXXX_C 0xa9
            "\xE2\x94\xBC", // U+253C ┼ LINE_XXXX_C 0xaa
        }
    };
    const unsigned char b = static_cast<unsigned char>( ch[0] );
    if( b >= 0xa0 && b <= 0xaa ) {
        return std::string( glyphs[b - 0xa0] );
    }
    return ch;
}
} // namespace

void cata_cursesport::curses_drawwindow( const catacurses::window &win_ )
{
    cata_cursesport::WINDOW *const win = win_.get<cata_cursesport::WINDOW>();
    if( win == nullptr ) {
        return;
    }
    std::vector<mp_cell> row;
    for( int j = 0; j < win->height; ++j ) {
        if( !win->line[j].touched ) {
            continue;
        }
        win->line[j].touched = false;
        row.clear();
        row.reserve( win->width );
        for( int i = 0; i < win->width; ++i ) {
            const cata_cursesport::cursecell &cell = win->line[j].chars[i];
            mp_cell out;
            // An empty glyph is the second column of a double-width character;
            // it is carried through as-is so the client renders it the same way.
            out.ch = mp_translate_line_glyph( cell.ch );
            out.fg = static_cast<uint8_t>( cell.FG );
            out.bg = static_cast<uint8_t>( cell.BG );
            row.push_back( out );
        }
        mp_server_blit_row( win->pos.x, win->pos.y + j, row );
    }
}

void catacurses::init_interface()
{
    // The client told us its terminal size during the handshake, so the grid
    // is already sized; the game's root window must match it exactly.
    catacurses::stdscr = catacurses::newwin( mp_server_grid().height(), mp_server_grid().width(),
                          point::zero );

    // The game's shared startup (run_game) drives ImGui from the first frame,
    // so the client owning the ImGui context must exist before that.  Mirror the
    // TUI backend (ncurses_def.cpp) minus its terminal-only calls: create the
    // imgui client, then initialize the color table, which needs that client.
    imclient = std::make_unique<cataimgui::client>();
    init_colors();
}

void catacurses::endwin()
{
    // Nothing to tear down: the screen is a buffer, and the transport is owned
    // by mp_server_main.
}

bool catacurses::supports_256_colors()
{
    // Colours travel as indices and the client maps them; the server never
    // renders, so it never needs to know what a terminal can show.
    return true;
}

input_event input_manager::get_input_event( const keyboard_mode /*mode*/ )
{
    while( true ) {
        int w = 0;
        int h = 0;
        if( mp_server_take_resize_request( w, h ) && handle_resize( w, h ) ) {
            // The same pair catacurses::resizeterm() runs in the ncurses
            // backend: handle_resize() rebuilt the windows, and this redraws
            // every UI into them. Without it the game repaints only whatever
            // it happens to invalidate next, and the flush below would carry a
            // blank grid to a client that has already cleared its terminal.
            ui_manager::screen_resized();
        }
        // When the only peer is gone there is nobody to wait for, and returning
        // a timeout forever just makes the game spin in its input loops. End the
        // session through the entry point instead; the game itself never sees
        // the transport. This is the one place the backend learns about the
        // wire, and it is deliberately the mp layer, not game logic.
        if( !mp_server_peer_connected() ) {
            // The stop itself is announced by exit_handler(), which also covers
            // the sessions that end without a disconnect.
            const std::string length = mp_server_log_session_length();
            mp_server_log( "client disconnected" +
                           ( length.empty() ? std::string() : " after " + length ) );
            mp_server_request_exit();
        }
        const size_t slot = mp_active_context_ptr() ? mp_active_context().slot_index : 0;
        mp_assert_holds( slot );

        const int timeout = mp_active_context_ptr() ?
                            mp_active_context().input_timeout_ms : -1;

        input_event ev;
        if( mp_sched *const sched = mp_active_sched() ) {
            // This is the only place in the game where the baton changes hands.
            // The screen goes out first (inside wait_for_input), because the
            // player must see what the game drew before anyone waits on them;
            // then the baton is released, and this thread blocks on its own
            // socket holding nothing. A slow wire cannot stall the simulation,
            // because whoever holds the baton is somebody else.
            mp_set_waiting_for_input( slot, true );
            sched->release_to( slot, mp_sched::world );
            ev = mp_server_wait_for_input( timeout );
            sched->wait_for_turn( slot );
            mp_set_waiting_for_input( slot, false );
            if( sched->stopping() ) {
                ev = input_event();
                ev.type = input_event_t::timeout;
                return ev;
            }
        } else {
            ev = mp_server_wait_for_input( timeout );
        }
        if( ev.type == input_event_t::timeout && mp_server_resize_pending() ) {
            // The wait ended on the client resizing, not on the clock. Go round
            // and apply it rather than handing the game a timeout it would sit
            // on until the player pressed a key.
            continue;
        }
        return ev;
    }
}

void input_manager::pump_events()
{
    mp_server_pump();
}

void input_manager::set_timeout( const int delay )
{
    // Mirrors the ncurses backend: a negative delay means "block". load_static_data
    // resets the timeout (via inp_mngr.init) before any player context exists; the
    // write is then the -1 default anyway, so only touch the context when there is
    // one to touch.
    if( player_context *ctx = mp_active_context_ptr() ) {
        ctx->input_timeout_ms = delay;
    }
    input_timeout = delay;
}

int get_terminal_width()
{
    return mp_server_grid().width();
}

int get_terminal_height()
{
    return mp_server_grid().height();
}

int projected_window_width()
{
    return mp_server_grid().width();
}

int projected_window_height()
{
    return mp_server_grid().height();
}

bool handle_resize( const int w, const int h )
{
    // The lower bound keeps the grid big enough for the game's windows; the
    // upper bound keeps the grid small enough to allocate. dispatch() already
    // drops oversized peer resizes, but handle_resize is also reached directly
    // via resize_term(), so it re-checks the cap rather than trusting its
    // callers. Refusing leaves the current terminal in place, which cannot
    // crash anything.
    if( w < EVEN_MINIMUM_TERM_WIDTH || h < EVEN_MINIMUM_TERM_HEIGHT ||
        w > MP_MAX_TERMINAL_WIDTH || h > MP_MAX_TERMINAL_HEIGHT ) {
        return false;
    }
    mp_server_set_terminal_size( w, h );
    catacurses::stdscr = catacurses::newwin( h, w, point::zero );
    game_ui::init_ui();
    return true;
}

void resize_term( const int cell_w, const int cell_h )
{
    // ncurses resizes the real terminal; on the server the equivalent is to
    // resize the grid, which is what the client's resize request would trigger.
    handle_resize( cell_w, cell_h );
}

void refresh_display()
{
    // Deliberately empty: the screen goes out when the game blocks for input,
    // not on every refresh. See mp_server_screen.h.
}

void drain_renderer_recovery()
{
    // There is no renderer to recover on a graphics-less server.
}

void refresh_mouse_config()
{
    // Mouse config lives on the client; the server never owns a mouse.
}

int get_scaling_factor()
{
    // The server has no font to scale; cells map one-to-one to the client.
    return 1;
}

bool is_draw_tiles_mode()
{
    // The server never draws tiles; the client renders the cell grid.
    return false;
}

bool gamepad_available()
{
    // Input devices belong to the client; the server never polls one.
    return false;
}

void set_title( const std::string & )
{
    // The window title is the client terminal's business, not the server's.
}

// Mirrors the ncurses/TUI implementation in input_context.cpp: the mouse
// position recorded by the input loop is turned into map coordinates the same
// way, so clicks work for the connected client.
std::optional<tripoint_bub_ms> input_context::get_coordinates( const catacurses::window
        &capture_win, const point &offset, const bool center_cursor ) const
{
    if( !coordinate_input_received ) {
        return std::nullopt;
    }
    const point view_size( getmaxx( capture_win ), getmaxy( capture_win ) );
    const point win_min( getbegx( capture_win ), getbegy( capture_win ) );
    const half_open_rectangle<point> win_bounds( win_min, win_min + view_size );
    if( !win_bounds.contains( coordinate ) ) {
        return std::nullopt;
    }

    point p = coordinate + offset;
    // If no offset is specified, account for the window location
    if( offset == point::zero ) {
        p -= win_min;
    }
    // Some windows (notably the overmap) want 0,0 to be the center of the screen
    if( center_cursor ) {
        p -= view_size / 2;
    }
    return tripoint_bub_ms( p.x, p.y, get_map().get_abs_sub().z() );
}

#endif // MP_SERVER
