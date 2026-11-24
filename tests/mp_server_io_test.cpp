#include "cata_catch.h"

#if defined(MP_SERVER)
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "avatar.h"
#include "cursesdef.h"
#include "game.h"
#include "game_constants.h"
#include "game_test_helper.h"
#include "input_enums.h"
#include "mp/mp_cell.h"
#include "mp/mp_context.h"
#include "mp/mp_describe.h"
#include "mp/mp_loopback_transport.h"
#include "mp/mp_protocol.h"
#include "mp/mp_screen_codec.h"
#include "mp/mp_server_input.h"
#include "mp/mp_server_mask.h"
#include "mp/mp_server_screen.h"
#include "mp/mp_sched.h"
#include "mp/mp_session_ui.h"
#include "mp/mp_turn.h"
#include "mp/mp_session.h"
#include "cata_scope_helpers.h"
#include "input.h"
#include "panels.h"
#include "player_helpers.h"

// Server input lives in the active player context (its input_queue), so every
// test must set one before any input call or reset. Static, not a stack local,
// so the active pointer does not dangle between tests.
static player_context test_input_ctx;

static std::vector<mp_cell> text_row( const std::string &s )
{
    std::vector<mp_cell> row;
    for( const char c : s ) {
        mp_cell cell;
        cell.ch = std::string( 1, c );
        row.push_back( cell );
    }
    return row;
}

static void send_control( mp_transport &t, const mp_control &msg )
{
    const std::string json = mp_control_to_json( msg );
    t.send( mp_wrap( mp_msg_kind::control, std::vector<uint8_t>( json.begin(), json.end() ) ) );
}

// One row of 80 distinct printable ASCII glyphs. Every cell differs from its
// neighbours, so the dirty-row codec cannot run-length-encode the row into a
// few bytes -- each cell becomes its own run, which makes a full 80x24 redraw
// land around 11 KB instead of a few hundred bytes. The backpressure tests
// need that bulk so the undelivered backlog can cross MP_SCREEN_BACKLOG_BYTES
// (512 KB) within a few dozen flushes.
static std::vector<mp_cell> pattern_row( const int row, const int seed )
{
    std::vector<mp_cell> cells;
    cells.reserve( 80 );
    for( int x = 0; x < 80; ++x ) {
        mp_cell cell;
        cell.ch = std::string( 1, static_cast<char>( '!' + ( x + 3 * row + seed ) % 94 ) );
        cells.push_back( cell );
    }
    return cells;
}

// Marks every row of the server's grid dirty with incompressible content, so
// the next flush encodes and sends a full redraw.
static void blit_full_screen( const int seed )
{
    for( int y = 0; y < 24; ++y ) {
        mp_server_blit_row( 0, y, pattern_row( y, seed ) );
    }
}

// Creates a default session (context_index == 0) with the given transport so
// mp_active_session() returns it. Replaces the old mp_server_set_transport().
static void set_test_transport( std::shared_ptr<mp_transport> transport )
{
    mp_sessions_reset_for_test();
    auto s = std::make_unique<mp_session>();
    s->transport = std::move( transport );
    s->context_index = 0;
    mp_sessions().push_back( std::move( s ) );
}

// The client-visible frames of one flush, in wire order.
struct io_frame {
    mp_msg_kind kind = mp_msg_kind::control;
    std::vector<uint8_t> body;
};

static std::vector<io_frame> drain_frames( mp_transport &client )
{
    std::vector<io_frame> frames;
    while( const std::optional<std::vector<uint8_t>> payload = client.recv( 0 ) ) {
        io_frame frame;
        REQUIRE( mp_unwrap( *payload, frame.kind, frame.body ) );
        frames.push_back( std::move( frame ) );
    }
    return frames;
}

// The describe replies among `frames`, in wire order. Every control frame must
// parse; the ones of other kinds are skipped.
static std::vector<mp_describe_msg> describe_replies( const std::vector<io_frame> &frames )
{
    std::vector<mp_describe_msg> replies;
    for( const io_frame &frame : frames ) {
        if( frame.kind != mp_msg_kind::control ) {
            continue;
        }
        mp_control msg;
        REQUIRE( mp_control_from_json( std::string( frame.body.begin(), frame.body.end() ), msg ) );
        if( msg.kind == mp_control_kind::describe ) {
            replies.push_back( msg.describe );
        }
    }
    return replies;
}

static mp_control describe_request( const uint32_t seq, const int x, const int y, const int w )
{
    mp_control msg;
    msg.kind = mp_control_kind::describe;
    msg.describe.seq = seq;
    msg.describe.x = static_cast<uint16_t>( x );
    msg.describe.y = static_cast<uint16_t>( y );
    msg.describe.w = static_cast<uint16_t>( w );
    return msg;
}

// A loopback client whose seat negotiated `caps` and entered the in-game UI.
// The seat is the active one (context 0), so mp_server_pump() and the flush
// both address it.
struct describe_seat {
    std::shared_ptr<mp_transport> client;
    mp_session *session = nullptr;
};

static describe_seat prepare_describe_seat( const uint32_t caps = MP_CAP_DESCRIBE )
{
    mp_loopback_pair pair = mp_make_loopback_pair();
    set_test_transport( pair.a );
    describe_seat seat;
    seat.session = mp_active_session();
    REQUIRE( seat.session != nullptr );
    seat.session->caps = caps;
    mp_session_ui_enter( *seat.session );
    seat.client = pair.b;
    return seat;
}

// One row of `width` cells no two neighbours of which are equal: the glyph
// repeats every 94 columns while the foreground byte advances with x. The
// screen codec therefore spends a whole run on every cell, and a 1000x100 full
// redraw stays over MP_SCREEN_BACKLOG_BYTES even after the flush has dropped
// the queued history and sent one replacement.
static void blit_wide_backlog_row( const int y, const int width )
{
    std::vector<mp_cell> cells;
    cells.reserve( static_cast<size_t>( width ) );
    for( int x = 0; x < width; ++x ) {
        mp_cell cell;
        cell.ch = std::string( 1, static_cast<char>( '!' + x % 94 ) );
        cell.fg = static_cast<uint8_t>( x % 251 );
        cells.push_back( cell );
    }
    mp_server_blit_row( 0, y, cells );
}

TEST_CASE( "server_blits_a_row_into_the_screen_grid", "[mp]" )
{
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    set_test_transport( nullptr );
    mp_server_set_terminal_size( 40, 10 );

    mp_server_blit_row( 2, 1, text_row( "hello" ) );

    CHECK( mp_server_grid().at( 2, 1 ).ch == "h" );
    CHECK( mp_server_grid().at( 6, 1 ).ch == "o" );
    CHECK( mp_server_grid().at( 7, 1 ).ch == " " );
}

TEST_CASE( "server_sends_nothing_when_nothing_changed", "[mp]" )
{
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    set_test_transport( pair.a );
    mp_server_set_terminal_size( 40, 10 );

    mp_server_flush_screen();
    REQUIRE( pair.b->recv( 0 ).has_value() );

    mp_server_flush_screen();
    CHECK_FALSE( pair.b->recv( 0 ).has_value() );
}

TEST_CASE( "server_sends_a_delta_far_smaller_than_a_full_screen", "[mp]" )
{
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    set_test_transport( pair.a );
    mp_server_set_terminal_size( 80, 24 );

    mp_server_flush_screen();
    const std::optional<std::vector<uint8_t>> full = pair.b->recv( 0 );
    REQUIRE( full.has_value() );

    mp_server_blit_row( 0, 0, text_row( "abcde" ) );
    mp_server_flush_screen();
    const std::optional<std::vector<uint8_t>> delta = pair.b->recv( 0 );
    REQUIRE( delta.has_value() );
    CHECK( delta->size() < full->size() / 4 );
}

TEST_CASE( "a_client_can_reconstruct_the_server_screen_from_the_wire", "[mp]" )
{
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    set_test_transport( pair.a );
    mp_server_set_terminal_size( 40, 10 );

    mp_server_blit_row( 5, 3, text_row( "reconstruct me" ) );
    mp_server_flush_screen();

    mp_screen_grid client( 40, 10 );
    while( const std::optional<std::vector<uint8_t>> msg = pair.b->recv( 0 ) ) {
        mp_msg_kind kind = mp_msg_kind::control;
        std::vector<uint8_t> body;
        REQUIRE( mp_unwrap( *msg, kind, body ) );
        if( kind == mp_msg_kind::screen ) {
            REQUIRE( mp_screen_apply( client, body ) );
        }
    }

    for( int y = 0; y < 10; ++y ) {
        for( int x = 0; x < 40; ++x ) {
            REQUIRE( client.at( x, y ) == mp_server_grid().at( x, y ) );
        }
    }
}

TEST_CASE( "the_join_menu_is_blanked_from_every_screen_at_game_start", "[mp]" )
{
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    set_test_transport( pair.a );
    mp_server_set_terminal_size( 40, 10 );

    // The join menu writes into the session grid; once the menu window is
    // gone, no later window owns those cells. The game-start reset must make
    // the next flush carry a blank full screen instead of leaving the text
    // ghosted over the map.
    mp_server_blit_row( 0, 3, text_row( "Continue as Stephan Kim" ) );
    mp_server_flush_screen();

    mp_screen_grid client( 40, 10 );
    auto apply_pending = [&]() {
        while( const std::optional<std::vector<uint8_t>> msg = pair.b->recv( 0 ) ) {
            mp_msg_kind kind = mp_msg_kind::control;
            std::vector<uint8_t> body;
            REQUIRE( mp_unwrap( *msg, kind, body ) );
            if( kind == mp_msg_kind::screen ) {
                REQUIRE( mp_screen_apply( client, body ) );
            }
        }
    };
    apply_pending();
    REQUIRE( client.at( 0, 3 ).ch == "C" );

    mp_server_blank_screens();
    mp_server_flush_screen();
    apply_pending();

    CHECK( client.at( 0, 3 ).ch == " " );
    CHECK( client.at( 9, 3 ).ch == " " );
    for( int y = 0; y < 10; ++y ) {
        for( int x = 0; x < 40; ++x ) {
            CHECK( client.at( x, y ) == mp_server_grid().at( x, y ) );
        }
    }
}

TEST_CASE( "server_returns_a_queued_keystroke", "[mp]" )
{
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    set_test_transport( pair.a );
    mp_server_set_terminal_size( 40, 10 );

    mp_control key;
    key.kind = mp_control_kind::input;
    key.event.type = input_event_t::keyboard_char;
    key.event.sequence = { 'y' };
    send_control( *pair.b, key );

    const input_event got = mp_server_wait_for_input( 1000 );
    CHECK( got.type == input_event_t::keyboard_char );
    CHECK( got.sequence == std::vector<int> { 'y' } );
}

TEST_CASE( "server_flushes_the_screen_before_blocking_for_input", "[mp]" )
{
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    set_test_transport( pair.a );
    mp_server_set_terminal_size( 40, 10 );
    mp_server_flush_screen();
    while( pair.b->recv( 0 ) ) {
    }

    mp_server_blit_row( 0, 0, text_row( "ready" ) );

    mp_control key;
    key.kind = mp_control_kind::input;
    key.event.type = input_event_t::keyboard_char;
    key.event.sequence = { 'y' };
    send_control( *pair.b, key );

    mp_server_wait_for_input( 1000 );

    bool saw_screen = false;
    while( const std::optional<std::vector<uint8_t>> msg = pair.b->recv( 0 ) ) {
        mp_msg_kind kind = mp_msg_kind::control;
        std::vector<uint8_t> body;
        REQUIRE( mp_unwrap( *msg, kind, body ) );
        saw_screen = saw_screen || kind == mp_msg_kind::screen;
    }
    CHECK( saw_screen );
}

TEST_CASE( "server_returns_a_timeout_event_when_no_input_arrives", "[mp]" )
{
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    set_test_transport( pair.a );
    mp_server_set_terminal_size( 40, 10 );

    const input_event got = mp_server_wait_for_input( 10 );
    CHECK( got.type == input_event_t::timeout );
}

TEST_CASE( "server_records_a_resize_request_without_touching_the_grid_itself", "[mp]" )
{
    // The grid is rebuilt by the curses backend, which must also rebuild the
    // game's windows; the module only records what the client asked for.
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    set_test_transport( pair.a );
    mp_server_set_terminal_size( 40, 10 );

    mp_control resize;
    resize.kind = mp_control_kind::resize;
    resize.resize.width = 60;
    resize.resize.height = 20;
    send_control( *pair.b, resize );

    mp_server_pump();

    int w = 0;
    int h = 0;
    REQUIRE( mp_server_take_resize_request( w, h ) );
    CHECK( w == 60 );
    CHECK( h == 20 );
    CHECK( mp_server_grid().width() == 40 );
    CHECK_FALSE( mp_server_take_resize_request( w, h ) );
}

TEST_CASE( "a_resize_wakes_the_input_wait", "[mp]" )
{
    // A resize is not a keystroke, so it used to leave the wait blocked: the
    // curses backend only applies a pending resize on its way into the *next*
    // get_input_event(), which never came until the player pressed something.
    // The client had already blanked its terminal by then, so the session sat
    // on a black screen until the next keypress. Waking the wait lets the
    // backend rebuild the windows and flush a full redraw straight away.
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    set_test_transport( pair.a );
    mp_server_set_terminal_size( 40, 10 );

    mp_control resize;
    resize.kind = mp_control_kind::resize;
    resize.resize.width = 60;
    resize.resize.height = 20;
    send_control( *pair.b, resize );

    const auto start = std::chrono::steady_clock::now();
    const input_event got = mp_server_wait_for_input( 5000 );
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start );

    CHECK( got.type == input_event_t::timeout );
    CHECK( elapsed < std::chrono::milliseconds( 1000 ) );
    CHECK( mp_server_resize_pending() );
}

TEST_CASE( "a_keystroke_outranks_a_resize_that_arrived_with_it", "[mp]" )
{
    // Waking on a resize must not swallow input that arrived first: the
    // keystroke is returned now and the resize survives for the next pass
    // (still on the wire here, since the wait stopped at the frame that
    // carried the keystroke).
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    set_test_transport( pair.a );
    mp_server_set_terminal_size( 40, 10 );

    mp_control key;
    key.kind = mp_control_kind::input;
    key.event.type = input_event_t::keyboard_char;
    key.event.sequence.push_back( 'q' );
    send_control( *pair.b, key );

    mp_control resize;
    resize.kind = mp_control_kind::resize;
    resize.resize.width = 60;
    resize.resize.height = 20;
    send_control( *pair.b, resize );

    const input_event got = mp_server_wait_for_input( 5000 );
    CHECK( got.type == input_event_t::keyboard_char );
    REQUIRE( got.sequence.size() == 1 );
    CHECK( got.sequence[0] == 'q' );

    mp_server_pump();
    int w = 0;
    int h = 0;
    REQUIRE( mp_server_take_resize_request( w, h ) );
    CHECK( w == 60 );
    CHECK( h == 20 );
}

TEST_CASE( "server_rejects_an_oversized_resize", "[mp]" )
{
    // A resize above the cap is dropped at the peer-data entry point, so it is
    // never recorded and the grid stays at its current size; honouring it would
    // make the grid allocation abort on std::bad_alloc.
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    set_test_transport( pair.a );
    mp_server_set_terminal_size( 40, 10 );

    mp_control resize;
    resize.kind = mp_control_kind::resize;
    resize.resize.width = MP_MAX_TERMINAL_WIDTH + 1;
    resize.resize.height = MP_MAX_TERMINAL_HEIGHT;
    send_control( *pair.b, resize );

    mp_server_pump();

    int w = 0;
    int h = 0;
    CHECK_FALSE( mp_server_take_resize_request( w, h ) );
    CHECK( mp_server_grid().width() == 40 );
    CHECK( mp_server_grid().height() == 10 );
}

TEST_CASE( "server_sends_the_cursor_position_with_the_screen", "[mp]" )
{
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    set_test_transport( pair.a );
    mp_server_set_terminal_size( 40, 10 );

    mp_server_set_cursor( 4, 5, true );
    mp_server_flush_screen();

    bool saw_cursor = false;
    while( const std::optional<std::vector<uint8_t>> msg = pair.b->recv( 0 ) ) {
        mp_msg_kind kind = mp_msg_kind::control;
        std::vector<uint8_t> body;
        REQUIRE( mp_unwrap( *msg, kind, body ) );
        if( kind != mp_msg_kind::control ) {
            continue;
        }
        mp_control got;
        REQUIRE( mp_control_from_json( std::string( body.begin(), body.end() ), got ) );
        if( got.kind == mp_control_kind::cursor ) {
            saw_cursor = true;
            CHECK( got.cursor.x == 4 );
            CHECK( got.cursor.y == 5 );
            CHECK( got.cursor.visible );
        }
    }
    CHECK( saw_cursor );
}

TEST_CASE( "server_survives_a_dropped_transport", "[mp]" )
{
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    set_test_transport( pair.a );
    mp_server_set_terminal_size( 40, 10 );
    pair.b->close();

    mp_server_blit_row( 0, 0, text_row( "gone" ) );
    mp_server_flush_screen();
    const input_event got = mp_server_wait_for_input( 10 );
    CHECK( got.type == input_event_t::timeout );
}

TEST_CASE( "a_describe_request_gets_an_answer_on_the_wire", "[mp]" )
{
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.slot_index = 0;
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    const describe_seat seat = prepare_describe_seat();
    mp_server_set_terminal_size( 40, 10 );

    // The epoch default already makes the request due, so the flush answers
    // it without any further delay.
    send_control( *seat.client, describe_request( 12, 30, 5, 40 ) );
    mp_server_pump();
    REQUIRE( seat.session->describe_pending );
    CHECK( seat.session->describe_pending->seq == 12 );

    mp_server_flush_screen();
    CHECK_FALSE( seat.session->describe_pending );

    const std::vector<io_frame> frames = drain_frames( *seat.client );
    const std::vector<mp_describe_msg> replies = describe_replies( frames );
    REQUIRE( replies.size() == 1 );
    CHECK( replies[0].seq == 12 );
    CHECK( replies[0].frame == seat.session->scene_seq );
    CHECK( replies[0].x == 30 );
    CHECK( replies[0].y == 5 );
    CHECK( replies[0].w == 40 );
    // The answer is all-or-nothing, so the lines member is on every reply even
    // when the cell has no text.
    bool saw_lines_member = false;
    for( const io_frame &frame : frames ) {
        if( frame.kind == mp_msg_kind::control ) {
            const std::string body( frame.body.begin(), frame.body.end() );
            if( body.find( "\"seq\":12" ) != std::string::npos ) {
                saw_lines_member = body.find( "\"lines\"" ) != std::string::npos;
            }
        }
    }
    CHECK( saw_lines_member );
}

TEST_CASE( "a_cell_outside_the_terminal_is_still_answered", "[mp]" )
{
    // The dispatch stores whatever rectangle the client pointed at; only the
    // reply decides whether it can describe anything. Dropping the request at
    // the door would leave the client waiting for a seq that never comes.
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.slot_index = 0;
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    const describe_seat seat = prepare_describe_seat();
    mp_server_set_terminal_size( 40, 10 );

    send_control( *seat.client, describe_request( 3, 900, 700, 40 ) );
    mp_server_pump();
    REQUIRE( seat.session->describe_pending );
    CHECK( seat.session->describe_pending->x == 900 );
    CHECK( seat.session->describe_pending->y == 700 );

    mp_server_flush_screen();
    const std::vector<mp_describe_msg> replies = describe_replies( drain_frames( *seat.client ) );
    REQUIRE( replies.size() == 1 );
    CHECK( replies[0].seq == 3 );
    CHECK( replies[0].x == 900 );
    CHECK( replies[0].y == 700 );
    CHECK( replies[0].lines.empty() );
}

TEST_CASE( "a_client_without_the_describe_cap_is_not_served", "[mp]" )
{
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.slot_index = 0;
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    const describe_seat seat = prepare_describe_seat( 0 );
    mp_server_set_terminal_size( 40, 10 );

    send_control( *seat.client, describe_request( 1, 30, 5, 40 ) );
    mp_server_pump();
    CHECK_FALSE( seat.session->describe_pending );

    mp_server_flush_screen();
    CHECK( describe_replies( drain_frames( *seat.client ) ).empty() );
}

TEST_CASE( "a_scene_and_log_client_without_describe_keeps_its_traffic", "[mp]" )
{
    // The spec's caps-3 client -- scene | log, no describe -- is the only old
    // client whose flush shares every block with the new one. The describe
    // block runs last and must leave the screen, log and scene frames intact,
    // never answer, and never keep the request it was not entitled to make.
    restore_active_state restore;
    mp_ensure_player_context();
    mp_sync_context_count();
    mp_activate_player( 0 );
    // The scene and the describe cell mapping read the map window, and the
    // avatar must be on the map for the scene to have levels.
    clear_avatar();
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.slot_index = 0;
    test_input_ctx.input_queue.clear();
    test_input_ctx.ter_view_p = get_avatar_legacy().pos_bub();
    test_input_ctx.w_terrain = catacurses::newwin( 10, 40, point( 10, 0 ) );
    mp_set_active_sched( nullptr );
    mp_server_reset_for_test();
    // The log snapshot walks the panel layout; without one the lookup would
    // recurse through its classic fallback (see mp_log_test.cpp).
    panel_manager::get_manager().init();

    const describe_seat seat = prepare_describe_seat( MP_CAP_SCENE | MP_CAP_LOG );
    mp_server_set_terminal_size( 80, 24 );
    // A dirty grid is an ordinary screen frame for this client, and the point
    // is that it still arrives.
    mp_server_blit_row( 0, 0, text_row( "dirty" ) );

    send_control( *seat.client, describe_request( 7, 30, 5, 40 ) );
    mp_server_pump();
    CHECK_FALSE( seat.session->describe_pending );

    mp_sched sched( 1 );
    mp_set_active_sched( &sched );
    on_out_of_scope clear_sched( []() {
        mp_set_active_sched( nullptr );
    } );
    std::thread player( [&sched]() {
        sched.wait_for_turn( 0 );
        mp_server_flush_screen();
        sched.release_to( 0, mp_sched::world );
    } );
    sched.yield_to( mp_sched::world, 0 );
    player.join();

    CHECK_FALSE( seat.session->describe_pending );
    const std::vector<io_frame> frames = drain_frames( *seat.client );
    size_t screen_index = frames.size();
    size_t log_index = frames.size();
    size_t scene_index = frames.size();
    for( size_t i = 0; i < frames.size(); ++i ) {
        if( frames[i].kind == mp_msg_kind::screen ) {
            screen_index = std::min( screen_index, i );
        } else if( frames[i].kind == mp_msg_kind::scene ) {
            scene_index = std::min( scene_index, i );
        } else {
            mp_control msg;
            REQUIRE( mp_control_from_json( std::string( frames[i].body.begin(),
                                          frames[i].body.end() ), msg ) );
            CHECK( msg.kind != mp_control_kind::describe );
            if( msg.kind == mp_control_kind::log ) {
                log_index = std::min( log_index, i );
            }
        }
    }
    // The flush's own order: the screen, then the log snapshot, then the scene
    // frame. A describe block that consumed or reordered any of them would
    // move one of these indices.
    REQUIRE( screen_index < frames.size() );
    REQUIRE( log_index < frames.size() );
    REQUIRE( scene_index < frames.size() );
    CHECK( screen_index < log_index );
    CHECK( log_index < scene_index );
    CHECK( describe_replies( frames ).empty() );

    test_input_ctx.w_terrain = catacurses::window();
}

TEST_CASE( "a_seat_that_left_the_ui_loses_its_describe_request", "[mp]" )
{
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.slot_index = 0;
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    const describe_seat seat = prepare_describe_seat();
    mp_server_set_terminal_size( 40, 10 );

    // Leaving the UI is the invariant, but the flush is what runs next: it
    // must not serve a request the seat is no longer entitled to, even if the
    // flag changed without going through mp_session_ui_leave().
    mp_session_ui_leave( *seat.session );
    send_control( *seat.client, describe_request( 2, 30, 5, 40 ) );
    mp_server_pump();
    CHECK_FALSE( seat.session->describe_pending );

    seat.session->describe_pending = mp_describe_request{ 2, 30, 5, 40 };
    mp_server_flush_screen();
    CHECK_FALSE( seat.session->describe_pending );
    CHECK( describe_replies( drain_frames( *seat.client ) ).empty() );
}

TEST_CASE( "the_describe_interval_gates_the_answer", "[mp]" )
{
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.slot_index = 0;
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    const describe_seat seat = prepare_describe_seat();
    mp_server_set_terminal_size( 40, 10 );

    send_control( *seat.client, describe_request( 4, 30, 5, 40 ) );
    mp_server_pump();
    REQUIRE( seat.session->describe_pending );

    // An answer built at this instant closes the window: the next request
    // waits out the interval even though it is already pending.
    seat.session->describe_last = std::chrono::steady_clock::now();
    mp_server_flush_screen();
    CHECK( seat.session->describe_pending );
    CHECK( describe_replies( drain_frames( *seat.client ) ).empty() );

    seat.session->describe_last = std::chrono::steady_clock::now() -
                                  std::chrono::milliseconds( MP_DESCRIBE_MIN_INTERVAL_MS + 1 );
    mp_server_flush_screen();
    CHECK_FALSE( seat.session->describe_pending );
    const std::vector<mp_describe_msg> replies = describe_replies( drain_frames( *seat.client ) );
    REQUIRE( replies.size() == 1 );
    CHECK( replies[0].seq == 4 );
}

TEST_CASE( "a_pending_describe_caps_the_input_wait", "[mp]" )
{
    // Without the cap the wait would sleep for the caller's whole budget, so
    // an answer that is already due would sit until the player pressed a key.
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.slot_index = 0;
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    const describe_seat seat = prepare_describe_seat();
    mp_server_set_terminal_size( 40, 10 );

    send_control( *seat.client, describe_request( 5, 30, 5, 40 ) );
    mp_server_pump();
    REQUIRE( seat.session->describe_pending );
    seat.session->describe_last = std::chrono::steady_clock::now();

    const auto start = std::chrono::steady_clock::now();
    const input_event got = mp_server_wait_for_input( 5000 );
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start );

    CHECK( got.type == input_event_t::timeout );
    // The wait woke on the interval, not on the caller's five seconds, and it
    // did not skip the interval either.
    CHECK( elapsed >= std::chrono::milliseconds( MP_DESCRIBE_MIN_INTERVAL_MS / 2 ) );
    CHECK( elapsed < std::chrono::milliseconds( 1000 ) );
    // Waking is not answering: the request survives for the caller's flush.
    CHECK( seat.session->describe_pending );
}

TEST_CASE( "a_burst_of_describe_requests_builds_one_answer", "[mp]" )
{
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.slot_index = 0;
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    const describe_seat seat = prepare_describe_seat();
    mp_server_set_terminal_size( 40, 10 );

    for( int i = 0; i < 1000; ++i ) {
        send_control( *seat.client, describe_request( static_cast<uint32_t>( i + 1 ), 30, 5, 40 ) );
    }

    const auto first_start = std::chrono::steady_clock::now();
    const input_event first = mp_server_wait_for_input( 5000 );
    const auto first_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - first_start );
    // The first frame makes the request due at once, so the wait wakes instead
    // of sleeping on the five-second budget.
    CHECK( first.type == input_event_t::timeout );
    CHECK( first_elapsed < std::chrono::milliseconds( 1000 ) );

    // The caller's next pass answers the request it saw; the latest already
    // won over the earlier ones by leaving one slot.
    const std::chrono::steady_clock::time_point before = seat.session->describe_last;
    mp_server_flush_screen();
    CHECK( seat.session->describe_last > before );
    CHECK_FALSE( seat.session->describe_pending );

    // The second wait reads what is left of the burst; the interval keeps it
    // from building another answer before it returns.
    CHECK( mp_server_wait_for_input( 5000 ).type == input_event_t::timeout );

    const std::vector<mp_describe_msg> replies = describe_replies( drain_frames( *seat.client ) );
    REQUIRE( replies.size() == 1 );
    CHECK( replies[0].seq == 1 );
}

TEST_CASE( "a_seat_without_the_baton_keeps_its_describe_request", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_activate_player( 0 );
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.slot_index = 0;
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    const describe_seat seat = prepare_describe_seat();
    mp_server_set_terminal_size( 40, 10 );

    mp_sched sched( 2 );
    mp_set_active_sched( &sched );
    on_out_of_scope clear_sched( []() {
        mp_set_active_sched( nullptr );
    } );

    send_control( *seat.client, describe_request( 6, 30, 5, 40 ) );
    mp_server_pump();
    REQUIRE( seat.session->describe_pending );
    seat.session->describe_last = std::chrono::steady_clock::now() -
                                  std::chrono::milliseconds( MP_DESCRIBE_MIN_INTERVAL_MS + 1 );

    // The world holds the baton: the join-loop redraw reaches the flush with
    // this seat's context active, but it must not answer for it.
    mp_server_flush_screen();
    CHECK( seat.session->describe_pending );
    CHECK( describe_replies( drain_frames( *seat.client ) ).empty() );

    // The seat's own turn answers it.
    std::thread player( [&sched]() {
        sched.wait_for_turn( 0 );
        mp_server_flush_screen();
        sched.release_to( 0, mp_sched::world );
    } );
    sched.yield_to( mp_sched::world, 0 );
    player.join();

    CHECK_FALSE( seat.session->describe_pending );
    const std::vector<mp_describe_msg> replies = describe_replies( drain_frames( *seat.client ) );
    REQUIRE( replies.size() == 1 );
    CHECK( replies[0].seq == 6 );
}

TEST_CASE( "a_backlogged_transport_drops_the_describe_request", "[mp]" )
{
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.slot_index = 0;
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    const describe_seat seat = prepare_describe_seat();
    // A full redraw this wide cannot be folded by the codec: at six bytes a
    // cell it stays over the backlog cap after the flush replaces the queued
    // history with one redraw, which is what the describe block then sees.
    mp_server_set_terminal_size( 1000, 100 );
    for( int y = 0; y < 100; ++y ) {
        blit_wide_backlog_row( y, 1000 );
    }
    mp_server_flush_screen();
    REQUIRE( seat.session->transport->pending_bytes() > MP_SCREEN_BACKLOG_BYTES );

    send_control( *seat.client, describe_request( 7, 30, 5, 40 ) );
    mp_server_pump();
    REQUIRE( seat.session->describe_pending );
    seat.session->describe_last = std::chrono::steady_clock::now() -
                                  std::chrono::milliseconds( MP_DESCRIBE_MIN_INTERVAL_MS + 1 );

    mp_server_flush_screen();
    // The request is dropped, not deferred: a hover answer that outlived its
    // view is worth less than the client's next question.
    CHECK_FALSE( seat.session->describe_pending );
    CHECK( describe_replies( drain_frames( *seat.client ) ).empty() );
}

TEST_CASE( "a_describe_answer_follows_the_scene_frame", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    mp_sync_context_count();
    mp_activate_player( 0 );
    // The base avatar can still be at the join-phase sentinel (z far below the
    // map), and a scene built there has no levels. A cleared avatar is on the
    // map at a fixed tile, which is all the scene and the cell mapping need.
    clear_avatar();
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.slot_index = 0;
    test_input_ctx.input_queue.clear();
    test_input_ctx.ter_view_p = get_avatar_legacy().pos_bub();
    // The scene takes its viewport from the map window, and the describe
    // answer maps the client's cell through that same rect.
    test_input_ctx.w_terrain = catacurses::newwin( 10, 40, point( 10, 0 ) );
    mp_set_active_sched( nullptr );
    mp_server_reset_for_test();

    // Both blocks below read these: a scene built at a z outside the map has
    // no levels, and a missing map window has no rect for either.
    REQUIRE( get_avatar_legacy().posz() == 0 );
    mp_scene_rect map_rect;
    REQUIRE( mp_scene_mask_map_rect( map_rect ) );
    CHECK( map_rect.w == 40 );
    CHECK( map_rect.h == 10 );

    const describe_seat seat = prepare_describe_seat( MP_CAP_SCENE | MP_CAP_DESCRIBE );
    mp_server_set_terminal_size( 80, 24 );

    send_control( *seat.client, describe_request( 21, 30, 5, 40 ) );
    mp_server_pump();
    REQUIRE( seat.session->describe_pending );
    seat.session->describe_last = std::chrono::steady_clock::now() -
                                  std::chrono::milliseconds( MP_DESCRIBE_MIN_INTERVAL_MS + 1 );

    // Both the scene frame and the answer need the baton: the world yields to
    // the seat for one flush and takes it back. The flush is the whole batch,
    // so the wire order below is the order the server produced.
    mp_sched sched( 1 );
    mp_set_active_sched( &sched );
    on_out_of_scope clear_sched( []() {
        mp_set_active_sched( nullptr );
    } );
    std::thread player( [&sched]() {
        sched.wait_for_turn( 0 );
        mp_server_flush_screen();
        sched.release_to( 0, mp_sched::world );
    } );
    sched.yield_to( mp_sched::world, 0 );
    player.join();

    CHECK_FALSE( seat.session->describe_pending );
    REQUIRE( seat.session->scene_seq == 1 );
    const std::vector<io_frame> frames = drain_frames( *seat.client );
    size_t screen_index = frames.size();
    size_t scene_index = frames.size();
    size_t describe_index = frames.size();
    mp_describe_msg reply;
    for( size_t i = 0; i < frames.size(); ++i ) {
        if( frames[i].kind == mp_msg_kind::screen ) {
            screen_index = std::min( screen_index, i );
        }
        if( frames[i].kind == mp_msg_kind::scene ) {
            scene_index = std::min( scene_index, i );
        }
        if( frames[i].kind == mp_msg_kind::control ) {
            mp_control msg;
            REQUIRE( mp_control_from_json( std::string( frames[i].body.begin(),
                                          frames[i].body.end() ), msg ) );
            if( msg.kind == mp_control_kind::describe ) {
                describe_index = std::min( describe_index, i );
                reply = msg.describe;
            }
        }
    }
    REQUIRE( scene_index < frames.size() );
    REQUIRE( describe_index < frames.size() );
    CHECK( screen_index < scene_index );
    CHECK( scene_index < describe_index );
    // The answer names the frame the scene block just sent, which is what lets
    // the client discard text computed for a world it no longer draws.
    CHECK( reply.seq == 21 );
    CHECK( reply.frame == 1 );
    CHECK( reply.x == 30 );
    CHECK( reply.y == 5 );
    CHECK( reply.w == 40 );

    test_input_ctx.w_terrain = catacurses::window();
}

TEST_CASE( "handshake_accepts_a_matching_protocol_version", "[mp]" )
{
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();

    mp_control hello;
    hello.kind = mp_control_kind::hello;
    hello.hello.proto = MP_PROTOCOL_VERSION;
    hello.hello.name = "serg";
    hello.hello.width = 100;
    hello.hello.height = 30;
    send_control( *pair.b, hello );

    mp_session session;
    std::string reason;
    CHECK( mp_server_handshake( *pair.a, session, reason ) );
    CHECK( reason.empty() );
    CHECK( session.grid.width() == 100 );
    CHECK( session.grid.height() == 30 );
    CHECK( session.name == "serg" );
}

TEST_CASE( "handshake_refuses_a_protocol_mismatch_with_an_explanation", "[mp]" )
{
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();

    mp_control hello;
    hello.kind = mp_control_kind::hello;
    hello.hello.proto = MP_PROTOCOL_VERSION + 1;
    hello.hello.name = "serg";
    hello.hello.width = 100;
    hello.hello.height = 30;
    send_control( *pair.b, hello );

    mp_session session;
    std::string reason;
    CHECK_FALSE( mp_server_handshake( *pair.a, session, reason ) );
    CHECK_THAT( reason, Catch::Contains( "protocol" ) );
}

TEST_CASE( "handshake_refuses_a_terminal_below_the_game_s_minimum", "[mp]" )
{
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();

    mp_control hello;
    hello.kind = mp_control_kind::hello;
    hello.hello.proto = MP_PROTOCOL_VERSION;
    hello.hello.name = "serg";
    hello.hello.width = EVEN_MINIMUM_TERM_WIDTH - 1;
    hello.hello.height = EVEN_MINIMUM_TERM_HEIGHT;
    send_control( *pair.b, hello );

    mp_session session;
    std::string reason;
    CHECK_FALSE( mp_server_handshake( *pair.a, session, reason ) );
    CHECK_THAT( reason, Catch::Contains( "terminal" ) );
}

TEST_CASE( "handshake_refuses_an_oversized_terminal", "[mp]" )
{
    // A peer that claims a terminal above the cap would otherwise make the
    // server's grid allocation multiply the two dimensions and abort on
    // std::bad_alloc; the handshake must refuse it like any other bad hello.
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();

    mp_control hello;
    hello.kind = mp_control_kind::hello;
    hello.hello.proto = MP_PROTOCOL_VERSION;
    hello.hello.name = "serg";
    hello.hello.width = MP_MAX_TERMINAL_WIDTH + 1;
    hello.hello.height = MP_MAX_TERMINAL_HEIGHT;
    send_control( *pair.b, hello );

    mp_session session;
    std::string reason;
    CHECK_FALSE( mp_server_handshake( *pair.a, session, reason ) );
    CHECK_FALSE( reason.empty() );
    CHECK_THAT( reason, Catch::Contains( "terminal" ) );

    // The verdict still reaches the client, exactly like the other refusals.
    const std::optional<std::vector<uint8_t>> msg = pair.b->recv( 100 );
    REQUIRE( msg.has_value() );
    mp_msg_kind kind = mp_msg_kind::screen;
    std::vector<uint8_t> body;
    REQUIRE( mp_unwrap( *msg, kind, body ) );
    REQUIRE( kind == mp_msg_kind::control );
    mp_control ack;
    REQUIRE( mp_control_from_json( std::string( body.begin(), body.end() ), ack ) );
    CHECK( ack.kind == mp_control_kind::hello_ack );
    CHECK_FALSE( ack.hello_ack.accepted );
}

TEST_CASE( "handshake_sends_its_verdict_to_the_client_either_way", "[mp]" )
{
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();

    mp_control hello;
    hello.kind = mp_control_kind::hello;
    hello.hello.proto = MP_PROTOCOL_VERSION + 1;
    hello.hello.name = "serg";
    hello.hello.width = 100;
    hello.hello.height = 30;
    send_control( *pair.b, hello );

    mp_session session;
    std::string reason;
    mp_server_handshake( *pair.a, session, reason );

    const std::optional<std::vector<uint8_t>> msg = pair.b->recv( 100 );
    REQUIRE( msg.has_value() );
    mp_msg_kind kind = mp_msg_kind::screen;
    std::vector<uint8_t> body;
    REQUIRE( mp_unwrap( *msg, kind, body ) );
    REQUIRE( kind == mp_msg_kind::control );
    mp_control ack;
    REQUIRE( mp_control_from_json( std::string( body.begin(), body.end() ), ack ) );
    CHECK( ack.kind == mp_control_kind::hello_ack );
    CHECK_FALSE( ack.hello_ack.accepted );
    CHECK_FALSE( ack.hello_ack.reason.empty() );
}

TEST_CASE( "handshake_gives_up_when_the_client_says_nothing", "[mp]" )
{
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();

    mp_session session;
    std::string reason;
    // The short budget the post-start poll uses: a silent peer is refused
    // instead of freezing the turn path for the join phase's whole budget.
    CHECK_FALSE( mp_server_handshake( *pair.a, session, reason, 20 ) );
    CHECK_THAT( reason, Catch::Contains( "timeout" ) );
}

TEST_CASE( "a_backlogged_client_gets_one_full_redraw_instead_of_a_history", "[mp]" )
{
    // A loopback peer that never reads simulates a client that stopped
    // draining its socket. Each iteration blits a full screen of
    // incompressible glyphs and flushes once, so every flush queues an ~11 KB
    // screen message. After a few dozen flushes the undelivered backlog
    // crosses MP_SCREEN_BACKLOG_BYTES and the server must collapse the queued
    // history into a single full redraw instead of replaying every stale
    // frame, so only the frames queued since the last collapse survive.
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    set_test_transport( pair.a );
    mp_server_set_terminal_size( 80, 24 );

    for( int i = 0; i < 400; ++i ) {
        blit_full_screen( i );
        // A moving cursor queues a control message next to every screen
        // update, so the drop has control messages to spare.
        mp_server_set_cursor( i % 80, i % 24, true );
        mp_server_flush_screen();
    }

    int screen_messages = 0;
    int control_messages = 0;
    while( const std::optional<std::vector<uint8_t>> msg = pair.b->recv( 0 ) ) {
        mp_msg_kind kind = mp_msg_kind::control;
        std::vector<uint8_t> body;
        REQUIRE( mp_unwrap( *msg, kind, body ) );
        screen_messages += kind == mp_msg_kind::screen ? 1 : 0;
        control_messages += kind == mp_msg_kind::control ? 1 : 0;
    }
    // ~11 KB per full redraw against a 512 KB limit collapses the backlog
    // roughly every 45 flushes, so the survivors number in the dozens. A bound
    // of 400 would also pass if the server dropped a single message.
    CHECK( screen_messages < 100 );
    // Backpressure drops screen history, never control messages: every cursor
    // move is still queued, including the ones from before each collapse.
    CHECK( control_messages == 400 );
}

TEST_CASE( "the_coalesced_redraw_still_describes_the_whole_screen", "[mp]" )
{
    // Dropping queued screen history is only safe because the screen is
    // idempotent: the client can lose intermediate frames and still converge
    // to the current state, as long as every dropped run is followed by a
    // full redraw. Applying every screen message the client actually receives
    // must therefore reproduce the server's final grid cell for cell.
    //
    // The flush that crosses the limit has to be a *partial* update for this
    // to have teeth: if every flush already carried a whole screen, the client
    // would converge whether or not the server redrew after the drop. So the
    // backlog is filled with full frames first, and the updates that follow
    // touch one row each -- without the redraw the client would be left with
    // that one row over a screen of stale blanks.
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    set_test_transport( pair.a );
    mp_server_set_terminal_size( 80, 24 );

    // The peer never reads, so the queue only grows.
    int fills = 0;
    while( pair.a->pending_bytes() <= MP_SCREEN_BACKLOG_BYTES ) {
        // Guards against a transport that reports no backlog at all, which
        // would otherwise spin here forever.
        REQUIRE( fills < 1000 );
        blit_full_screen( fills );
        mp_server_flush_screen();
        ++fills;
    }

    // The first of these sees the over-limit backlog and must turn its
    // one dirty row into a redraw of the whole screen.
    for( int i = 0; i < 20; ++i ) {
        mp_server_blit_row( 0, i % 24, text_row( "partial update " + std::to_string( i ) ) );
        mp_server_flush_screen();
    }

    mp_screen_grid client( 80, 24 );
    while( const std::optional<std::vector<uint8_t>> msg = pair.b->recv( 0 ) ) {
        mp_msg_kind kind = mp_msg_kind::control;
        std::vector<uint8_t> body;
        REQUIRE( mp_unwrap( *msg, kind, body ) );
        if( kind == mp_msg_kind::screen ) {
            REQUIRE( mp_screen_apply( client, body ) );
        }
    }

    for( int y = 0; y < 24; ++y ) {
        for( int x = 0; x < 80; ++x ) {
            REQUIRE( client.at( x, y ) == mp_server_grid().at( x, y ) );
        }
    }
}

TEST_CASE( "each_session_keeps_its_own_screen", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();
    mp_sessions_reset_for_test();

    mp_loopback_pair wire_a = mp_make_loopback_pair();
    mp_loopback_pair wire_b = mp_make_loopback_pair();
    mp_sessions().push_back( std::make_unique<mp_session>() );
    mp_sessions().push_back( std::make_unique<mp_session>() );
    mp_sessions()[0]->transport = wire_a.a;
    mp_sessions()[0]->context_index = 0;
    mp_sessions()[0]->grid.resize( 80, 24 );
    mp_sessions()[1]->transport = wire_b.a;
    mp_sessions()[1]->context_index = 1;
    mp_sessions()[1]->grid.resize( 100, 30 );

    mp_activate_player( 0 );
    REQUIRE( mp_active_session() == mp_sessions()[0].get() );
    mp_server_blit_row( 0, 0, { mp_cell{ "A", 1, 0 } } );

    mp_activate_player( 1 );
    REQUIRE( mp_active_session() == mp_sessions()[1].get() );
    mp_server_blit_row( 0, 0, { mp_cell{ "B", 2, 0 } } );

    // The leak this guards is the one the design calls the main class of bug:
    // player 1 seeing player 2's screen.
    CHECK( mp_sessions()[0]->grid.at( 0, 0 ).ch == "A" );
    CHECK( mp_sessions()[1]->grid.at( 0, 0 ).ch == "B" );
    CHECK( mp_sessions()[0]->grid.width() == 80 );
    CHECK( mp_sessions()[1]->grid.width() == 100 );

    mp_activate_player( 0 );
    mp_sessions_reset_for_test();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

TEST_CASE( "the_world_phase_has_no_active_session", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    mp_sessions_reset_for_test();
    mp_sessions().push_back( std::make_unique<mp_session>() );
    mp_sessions()[0]->context_index = 0;

    mp_activate_player( 0 );
    REQUIRE( mp_active_session() != nullptr );

    mp_activate_world();
    // Nothing the world phase draws belongs to one client, so there is nothing
    // to address. Broadcast drawing goes through mp_for_each_context.
    CHECK( mp_active_session() == nullptr );

    mp_activate_player( 0 );
    mp_sessions_reset_for_test();
}

TEST_CASE( "waiting_for_input_yields_the_baton", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    mp_sessions_reset_for_test();
    mp_loopback_pair wire = mp_make_loopback_pair( 5 );
    mp_sessions().push_back( std::make_unique<mp_session>() );
    mp_sessions()[0]->transport = wire.a;
    mp_sessions()[0]->context_index = 0;
    mp_activate_player( 0 );

    mp_sched sched( 1 );
    mp_set_active_sched( &sched );
    on_out_of_scope clear_sched( []() {
        mp_set_active_sched( nullptr );
    } );

    std::atomic<bool> world_ran{ false };
    std::thread player( [&]() {
        sched.wait_for_turn( 0 );
        // get_input_event() must give the baton up while it blocks on the wire:
        // otherwise a player thinking in a menu stops everybody else, which is
        // the whole thing the scheduler exists to prevent.
        inp_mngr.get_input_event( keyboard_mode::keychar );
        if( !sched.stopping() ) {
            sched.release_to( 0, mp_sched::world );
        }
    } );

    sched.yield_to( mp_sched::world, 0 );
    // Reaching here means the player thread released the baton from inside its
    // input wait, before any key was sent.
    world_ran = true;
    CHECK( world_ran.load() );

    // Let the player finish.
    mp_control key;
    key.kind = mp_control_kind::input;
    key.event = input_event( static_cast<int>( ' ' ), input_event_t::keyboard_char );
    const std::string json = mp_control_to_json( key );
    wire.b->send( mp_wrap( mp_msg_kind::control,
                           std::vector<uint8_t>( json.begin(), json.end() ) ) );
    sched.shut_down();
    player.join();
    mp_sessions_reset_for_test();
}

// Starts the per-seat reader threads for a test-built session and joins them on
// the way out. A session whose reader is still joinable terminates the process
// when its mp_session dies, so every case here that raises readers has to drop
// them before the sessions go away.
struct reader_scope {
    reader_scope() {
        mp_server_start_readers();
    }
    ~reader_scope() {
        mp_server_stop_readers();
    }
    reader_scope( const reader_scope & ) = delete;
    reader_scope &operator=( const reader_scope & ) = delete;
};

TEST_CASE( "a_plan_frame_reaches_the_world_without_the_baton", "[mp]" )
{
    // No player thread, no baton and no flush: the plan has to travel from the
    // wire into the world's hands through the seat's reader alone.
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    set_test_transport( pair.a );
    mp_server_set_terminal_size( 40, 10 );
    const reader_scope readers;

    mp_control plan;
    plan.kind = mp_control_kind::plan;
    plan.plan.kind = mp_plan_kind::move;
    plan.plan.seq = 9;
    send_control( *pair.b, plan );

    REQUIRE( mp_server_wait_for_events( 100 ) );

    size_t slot = 99;
    mp_control got;
    REQUIRE( mp_server_take_control( slot, got ) );
    CHECK( slot == 0 );
    CHECK( got.kind == mp_control_kind::plan );
    CHECK( got.plan.seq == 9 );
    // Taken, not merely observed: the world's next pass has nothing left.
    CHECK_FALSE( mp_server_take_control( slot, got ) );
}

TEST_CASE( "a_raw_input_frame_lands_in_the_seat_queue", "[mp]" )
{
    // Keystrokes do not go to the world: they belong to the seat that typed
    // them, and the seat's own input wait is what consumes them.
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    set_test_transport( pair.a );
    mp_server_set_terminal_size( 40, 10 );
    const reader_scope readers;

    mp_control key;
    key.kind = mp_control_kind::input;
    key.event.type = input_event_t::keyboard_char;
    key.event.sequence.push_back( 'x' );
    send_control( *pair.b, key );

    REQUIRE( mp_server_wait_for_events( 100 ) );
    CHECK( mp_server_seat_has_input( 0 ) );
    CHECK_FALSE( mp_server_seat_has_input( 1 ) );

    const input_event got = mp_server_wait_for_input( 1000 );
    CHECK( got.type == input_event_t::keyboard_char );
    CHECK( got.sequence == std::vector<int> { 'x' } );
    CHECK_FALSE( mp_server_seat_has_input( 0 ) );
}

TEST_CASE( "closing_the_transport_wakes_the_event_wait", "[mp]" )
{
    // The world parks in mp_server_wait_for_events() while nobody is typing;
    // a client that disappears must not leave it there until its timeout. The
    // reader notices the dead wire and signals, without touching game state --
    // marking the seat disconnected is the world's job.
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    set_test_transport( pair.a );
    mp_server_set_terminal_size( 40, 10 );
    const reader_scope readers;

    pair.b->close();

    REQUIRE( mp_server_wait_for_events( 1000 ) );

    // Nothing was queued: waking is the whole event.
    size_t slot = 0;
    mp_control got;
    CHECK_FALSE( mp_server_take_control( slot, got ) );
}

#endif // MP_SERVER
