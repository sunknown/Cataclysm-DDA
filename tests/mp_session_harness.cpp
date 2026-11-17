#if defined(MP_SERVER)

#include "mp_session_harness.h"

#include "cata_catch.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "avatar.h"
#include "cached_options.h"
#include "cursesdef.h"
#include "game.h"
#include "game_constants.h"
#include "game_test_helper.h"
#include "game_ui.h"
#include "map.h"
#include "map_helpers.h"
#include "map_helpers_tests.h"
#include "output.h"
#include "player_helpers.h"
#include "type_id.h"
#include "ui_manager.h"
#include "mp/mp_context.h"
#include "mp/mp_loopback_transport.h"
#include "mp/mp_protocol.h"
#include "mp/mp_sched.h"
#include "mp/mp_server_main.h"
#include "mp/mp_server_screen.h"
#include "mp/mp_session.h"
#include "mp/mp_turn.h"

mp_server_startup_for_test::mp_server_startup_for_test( const int cols, const int rows )
    : prev_stdscr_( catacurses::stdscr ), prev_test_mode_( test_mode ), prev_termx_( TERMX ),
      prev_termy_( TERMY ), prev_full_screen_width_( FULL_SCREEN_WIDTH ),
      prev_full_screen_height_( FULL_SCREEN_HEIGHT ), prev_imclient_( std::move( imclient ) ),
      prev_panels_( panel_manager::get_manager() )
{
    // The same order main() and run_game() use. The handshake sizes the grid
    // before init_interface(), which reads it back to size the root window; with
    // no session attached that is the fallback grid, which is why the write side
    // of mp_server_blit_row() has to reach the same grid the read side does.
    mp_server_set_terminal_size( cols, rows );
    FULL_SCREEN_WIDTH = EVEN_MINIMUM_TERM_WIDTH;
    FULL_SCREEN_HEIGHT = EVEN_MINIMUM_TERM_HEIGHT;
    catacurses::init_interface();
    game_ui::init_ui();
    // game::setup() does this once the world's JSON is loaded; the test binary
    // never runs setup(), so without it the sidebar has no layout to draw and
    // panel_manager::get_current_layout() recurses until the stack runs out.
    panel_manager::get_manager().init();
    test_mode = false;
}

mp_server_startup_for_test::~mp_server_startup_for_test()
{
    test_mode = prev_test_mode_;
    panel_manager::get_manager() = prev_panels_;
    imclient = std::move( prev_imclient_ );
    FULL_SCREEN_HEIGHT = prev_full_screen_height_;
    FULL_SCREEN_WIDTH = prev_full_screen_width_;
    TERMY = prev_termy_;
    TERMX = prev_termx_;
    catacurses::stdscr = prev_stdscr_;
}

mp_session_harness::mp_session_harness( const size_t players, const int latency_ms,
                                        const mp_harness_mode mode )
{
    latency_ms_ = latency_ms;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    // build_test_map() repaints terrain and leaves the creatures the test world
    // generated. A hostile that can see a player arms the combat hurry
    // countdown on its own (mp_tick_hurry), and a session whose turns can close
    // without anyone hurrying cannot show what a hurry does.
    clear_creatures();
    g->ensure_gamemode_for_test();
    // The first do_turn() of a session is the one-time "new_game" pass
    // (weather.on_game_start), which does not advance the calendar. Pin it here
    // so turn counting in the tests is deterministic no matter which tests ran
    // before this one in the same process.
    g->new_game = true;
    game_test_helper::reset_player_slots();
    mp_sessions_reset_for_test();

    // Sessions are gone, so the terminal set up below lands in the fallback
    // grid rather than in a leftover session's. From here on the harness is a
    // running server: the screen has a size, a root window and an ImGui client,
    // and redraws are no longer gated off.
    startup_ = std::make_unique<mp_server_startup_for_test>( MP_HARNESS_COLS, MP_HARNESS_ROWS );
    // Again, now that TERMX/TERMY and stdscr are real: the first call above ran
    // before there was a terminal and left context 0 mirroring zeroes. This is
    // the order run_game() uses -- init_ui() first, then the context.
    mp_ensure_player_context();

    const bool join = mode == mp_harness_mode::join;
    // Where the test map put P0: in join mode, where the new characters go, as
    // a played world's state would place them on a real server.
    const tripoint_abs_ms world_anchor = g->player_at( 0 ).pos_abs();
    if( join ) {
        // Every slot starts as the empty avatar the join menu fills. The test
        // avatar slot 0 held is kept aside and put back by the destructor.
        stashed_avatar_ = std::make_unique<avatar>( std::move( g->player_at( 0 ) ) );
        g->player_at( 0 ) = avatar();
        for( size_t i = 1; i < players; ++i ) {
            g->add_player_slot();
        }
    }
    g->player_at( 0 ).name = "P0";
    for( size_t i = 1; i < players && !join; ++i ) {
        avatar &extra = g->add_player_slot();
        extra.set_body();
        extra.name = "P" + std::to_string( i );
        // A new slot is created nowhere in particular -- it was measured at
        // z = -500, off the map -- and the world half of a closed turn then
        // works on that position (scent, fields, caches). Stand every extra
        // player on the test map, a column south of P0 and two tiles apart, so
        // none of them is adjacent to anything a test puts next to P0.
        extra.setpos( get_map(), g->player_at( 0 ).pos_bub() +
                      tripoint_rel_ms( 0, 2 * static_cast<int>( i ), 0 ) );
    }
    // g->add_player_slot() leaves a new avatar at moves == 100, unlike a fresh
    // avatar's zero. Zero every slot here so the "fresh avatars start with zero
    // moves" turn-counting the tests rely on is actually true for every slot,
    // not just slot 0.
    for( size_t i = 0; i < players; ++i ) {
        g->player_at( i ).set_moves( 0 );
    }
    mp_sync_context_count();
    // Fresh session: no leftover barrier state from a previous harness. The
    // input-wait flags, the turn-open flag and the end-of-session request belong
    // to a session, not to a turn, so mp_turn_reset() deliberately leaves them --
    // reset_all is the session/test boundary.
    mp_turn_reset_all();

    for( size_t i = 0; i < players; ++i ) {
        // The latency is not decoration. Without it, code that only works at
        // zero round-trip time passes here and fails on the first real wire.
        mp_loopback_pair wire = mp_make_loopback_pair( latency_ms );
        auto session = std::make_unique<mp_session>();
        session->transport = wire.a;
        session->context_index = i;
        session->name = "P" + std::to_string( i );
        session->grid.resize( MP_HARNESS_COLS, MP_HARNESS_ROWS );
        mp_sessions().push_back( std::move( session ) );
        client_wires_.push_back( wire.b );
        client_grids_.emplace_back( MP_HARNESS_COLS, MP_HARNESS_ROWS );
        client_states_.emplace_back();
    }

    // Each thread runs its player's join menu first in join mode, and goes
    // straight to its turns otherwise -- which also clears what a previous
    // harness's join phase left behind.
    mp_join_begin( join ? players : 0, world_anchor );
    seated_ = !join;

    sched_ = std::make_unique<mp_sched>( players );
    mp_set_active_sched( sched_.get() );
    // The same thread body, per-seat lifecycle and watchdog the server raises
    // (mp_server_start_player_threads), not copies of them that can drift: the
    // harness has to be able to drop a seat and take a replacement in it exactly
    // as the server does.
    mp_sched_start_watchdog( *sched_, MP_WATCHDOG_MS );
    for( size_t i = 0; i < players; ++i ) {
        mp_server_start_player_thread( i );
    }
}

mp_session_harness::~mp_session_harness()
{
    // The server's own teardown: wires first, then the shutdown, then the
    // joins, ending in the world phase. Most harness tests end with a player
    // in a menu, which is the case that ordering exists for.
    mp_stop_player_threads( sched_.get() );
    mp_set_active_sched( nullptr );
    mp_sessions_reset_for_test();
    mp_join_begin( 0, std::nullopt );
    game_test_helper::reset_player_slots();
    if( stashed_avatar_ ) {
        g->player_at( 0 ) = std::move( *stashed_avatar_ );
        stashed_avatar_.reset();
    }
    mp_sync_context_count();
    mp_turn_reset_all();
    // A session that ended (a death, a quit) leaves the quit status behind,
    // and the next test's first do_turn() would read it as its game being over.
    g->uquit = QUIT_NO;
    // Put the globals back here rather than letting the member die on the way
    // out of the object, because the context has to be re-mirrored from the
    // restored terminal and not from the harness's.
    startup_.reset();
    // Restoring the globals is not enough on its own. mp_ensure_player_context()
    // copied TERMX, TERMY and stdscr into g->contexts[0] when the harness
    // started (src/mp/mp_context.cpp:86-89), and mp_activate_player() publishes
    // that copy straight back out into the same globals (:126, :136). A later
    // test that reaches mp_activate_player() -- directly, or through
    // mp_sync_context_count(), which several [mp] tests do -- without calling
    // ensure itself would therefore restore the harness's terminal over the one
    // just put back. Re-mirroring here leaves the context agreeing with the
    // globals, so that path publishes the pre-harness values instead.
    mp_ensure_player_context();
}

void mp_session_harness::press( const size_t slot, const int key )
{
    mp_control msg;
    msg.kind = mp_control_kind::input;
    msg.event = input_event( key, input_event_t::keyboard_char );
    const std::string json = mp_control_to_json( msg );
    client_wires_[slot]->send( mp_wrap( mp_msg_kind::control,
                                        std::vector<uint8_t>( json.begin(), json.end() ) ) );
}

void mp_session_harness::pump_clients()
{
    for( size_t i = 0; i < client_wires_.size(); ++i ) {
        while( const std::optional<std::vector<uint8_t>> msg = client_wires_[i]->recv( 0 ) ) {
            mp_client_apply_message( *msg, client_grids_[i], client_states_[i] );
        }
    }
}

void mp_session_harness::choose_random_character( const size_t slot )
{
    // "New character: random", then the creation screen opens on its summary:
    // the next-tab key there finishes, and the confirmation takes a capital Y.
    press( slot, 'r' );
    press( slot, '\t' );
    press( slot, 'Y' );
}

void mp_session_harness::quit_join_menu( const size_t slot )
{
    // The Quit entry's hotkey, which is the key a person presses.
    press( slot, 'q' );
}

void mp_session_harness::drop_client( const size_t slot )
{
    // Closing this end of the loopback pair closes the other end too, so the
    // server's session sees exactly what a client process dying leaves behind:
    // a transport that reports itself disconnected.
    client_wires_[slot]->close();
}

bool mp_session_harness::client_connected( const size_t slot ) const
{
    return slot < client_wires_.size() && client_wires_[slot] &&
           client_wires_[slot]->connected();
}

bool mp_session_harness::seat_occupied( const size_t slot ) const
{
    return mp_session_of_slot( slot ) != nullptr;
}

void mp_session_harness::connect_replacement_client( const size_t slot )
{
    // The seat is free and its thread is already joined: the world did both
    // before it stopped polling. What is left is what a new connection does.
    cata_assert( !seat_occupied( slot ) );
    cata_assert( !client_connected( slot ) );
    mp_loopback_pair wire = mp_make_loopback_pair( latency_ms_ );
    auto session = std::make_unique<mp_session>();
    session->transport = wire.a;
    // Name and grid as the constructor sets them, so this client is
    // indistinguishable from a first connection wherever the session reads it.
    session->name = "P" + std::to_string( slot );
    session->grid.resize( MP_HARNESS_COLS, MP_HARNESS_ROWS );
    client_wires_[slot] = wire.b;
    client_grids_[slot] = mp_screen_grid( MP_HARNESS_COLS, MP_HARNESS_ROWS );
    client_states_[slot] = mp_client_state();
    // The server's own seat path: roster entry, cleared quit mark, new thread
    // running this client's join menu.
    mp_server_seat_client( slot, std::move( session ) );
}

bool mp_session_harness::seated() const
{
    return seated_;
}

bool mp_session_harness::run_until( const std::function<bool()> &predicate, const int max_turns )
{
    for( int turn = 0; turn < max_turns; ++turn ) {
        if( !seated_ ) {
            // The join phase: no turn runs until everyone has chosen.
            std::string err;
            const mp_join_progress progress = mp_join_poll( *sched_, err );
            if( progress == mp_join_progress::failed ) {
                // Reported where a test can see it, then treated as running out
                // of turns.
                UNSCOPED_INFO( "join phase failed: " << err );
                return false;
            }
            seated_ = progress == mp_join_progress::seated;
        } else {
            g->do_turn();
        }
        pump_clients();
        if( predicate() ) {
            return true;
        }
    }
    // Returning false rather than looping forever is the point: a barrier bug
    // shows up as a failed assertion with a message, not as a hung CI job.
    return false;
}

const mp_screen_grid &mp_session_harness::screen( const size_t slot ) const
{
    return client_grids_[slot];
}

int mp_grid_nonblank_cells( const mp_screen_grid &grid )
{
    int cells = 0;
    for( int y = 0; y < grid.height(); ++y ) {
        for( int x = 0; x < grid.width(); ++x ) {
            cells += grid.at( x, y ).ch != " " ? 1 : 0;
        }
    }
    return cells;
}

bool mp_grid_contains( const mp_screen_grid &grid, const std::string &text )
{
    for( int y = 0; y < grid.height(); ++y ) {
        std::string row;
        for( int x = 0; x < grid.width(); ++x ) {
            row += grid.at( x, y ).ch;
        }
        if( row.find( text ) != std::string::npos ) {
            return true;
        }
    }
    return false;
}

bool mp_grids_equal( const mp_screen_grid &a, const mp_screen_grid &b )
{
    if( a.width() != b.width() || a.height() != b.height() ) {
        return false;
    }
    for( int y = 0; y < a.height(); ++y ) {
        for( int x = 0; x < a.width(); ++x ) {
            if( a.at( x, y ) != b.at( x, y ) ) {
                return false;
            }
        }
    }
    return true;
}

#endif // MP_SERVER
