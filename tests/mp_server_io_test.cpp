#include "cata_catch.h"

#if defined(MP_SERVER)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
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
#include "map.h"
#include "map_helpers.h"
#include "map_helpers_tests.h"
#include "mp/mp_cell.h"
#include "mp/mp_context.h"
#include "mp/mp_describe.h"
#include "mp/mp_loopback_transport.h"
#include "mp/mp_plan.h"
#include "mp/mp_protocol.h"
#include "mp/mp_scene.h"
#include "mp/mp_screen_codec.h"
#include "mp/mp_server_input.h"
#include "mp/mp_server_main.h"
#include "mp/mp_server_mask.h"
#include "mp/mp_server_screen.h"
#include "mp/mp_sched.h"
#include "mp/mp_session_ui.h"
#include "mp/mp_turn.h"
#include "mp/mp_session.h"
#include "cata_scope_helpers.h"
#include "input.h"
#include "panels.h"
#include "player_activity.h"
#include "player_helpers.h"
#include "type_id.h"
#include "ui_manager.h"

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

// Whether the thread blocked in a wait for up to `limit` has returned yet.
static bool wait_for_flag( const std::atomic<bool> &flag,
                           const std::chrono::milliseconds limit )
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while( !flag.load() && std::chrono::steady_clock::now() < deadline ) {
        std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
    }
    return flag.load();
}

// Runs one blocking seat wait on its own thread and reports whether it got back.
struct parked_wait {
    std::atomic<bool> returned{ false };
    std::atomic<int> type{ -1 };
    std::thread thread;

    explicit parked_wait( mp_session *const seat ) {
        thread = std::thread( [this, seat]() {
            const input_event ev = mp_server_wait_for_input_of( seat, &test_input_ctx, -1 );
            type.store( static_cast<int>( ev.type ) );
            returned.store( true );
        } );
    }
    parked_wait( const parked_wait & ) = delete;
    parked_wait &operator=( const parked_wait & ) = delete;
    ~parked_wait() {
        if( thread.joinable() ) {
            thread.detach();
        }
    }
    // Joins after the wait came back; on a failure the caller must unblock it
    // first (see the tests) so a broken build cannot hang the binary.
    void finish() {
        if( thread.joinable() ) {
            thread.join();
        }
    }
};

TEST_CASE( "a_closed_wire_wakes_a_blocking_seat_wait", "[mp]" )
{
    // The production input wait uses -1 (block until something happens). A
    // client that quits while its player is parked there must wake the seat:
    // before the reader dropped reader_running on exit, and with no transport
    // check in the predicate, the notification was swallowed and the thread
    // slept forever.
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    set_test_transport( pair.a );
    mp_server_set_terminal_size( 40, 10 );
    const reader_scope readers;
    mp_session *const seat = mp_active_session();
    REQUIRE( seat != nullptr );

    parked_wait wait( seat );
    std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    REQUIRE_FALSE( wait.returned.load() );

    pair.b->close();

    if( !wait_for_flag( wait.returned, std::chrono::seconds( 5 ) ) ) {
        // Unblock the waiter so the failure is a CHECK, not a hung binary.
        mp_server_stop_reader( *seat );
    }
    wait.finish();
    CHECK( wait.returned.load() );
    CHECK( wait.type.load() == static_cast<int>( input_event_t::timeout ) );
}

TEST_CASE( "a_queued_plan_does_not_spin_the_seat_input_wait", "[mp]" )
{
    // A plan belongs to the world; the seat leaves it in the queue. If the seat
    // treated it as an arrival it would return from the wait immediately, loop,
    // and burn its thread until the world drained the plan -- and a -1 wait
    // would instead be one sleep.
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    set_test_transport( pair.a );
    mp_server_set_terminal_size( 40, 10 );
    const reader_scope readers;
    mp_session *const seat = mp_active_session();
    REQUIRE( seat != nullptr );

    mp_control plan;
    plan.kind = mp_control_kind::plan;
    plan.plan.kind = mp_plan_kind::move;
    plan.plan.seq = 3;
    send_control( *pair.b, plan );
    REQUIRE( mp_server_wait_for_events( 1000 ) );

    timespec before {};
    timespec after {};
    REQUIRE( clock_gettime( CLOCK_THREAD_CPUTIME_ID, &before ) == 0 );
    const input_event ev = mp_server_wait_for_input_of( seat, &test_input_ctx, 300 );
    REQUIRE( clock_gettime( CLOCK_THREAD_CPUTIME_ID, &after ) == 0 );
    const double cpu_ms = ( after.tv_sec - before.tv_sec ) * 1000.0 +
                          ( after.tv_nsec - before.tv_nsec ) / 1000000.0;

    CHECK( ev.type == input_event_t::timeout );
    // A spin spends the whole budget on this thread. A sleeping wait spends
    // almost nothing, so anything under a third of it cannot be the bug.
    CHECK( cpu_ms < 100.0 );

    // The seat did not eat the plan: the world still gets it.
    size_t slot = 99;
    mp_control got;
    REQUIRE( mp_server_take_control( slot, got ) );
    CHECK( slot == 0 );
    CHECK( got.kind == mp_control_kind::plan );
    CHECK( got.plan.seq == 3 );
}

TEST_CASE( "take_control_consumes_describe_and_resize_and_wakes_the_seat", "[mp]" )
{
    // Ruling 2 hands describe/resize/bye to mp_server_take_control, so the
    // world is what turns a hover or resize into seat state. The seat may be
    // parked in a -1 wait, so consuming the message must also wake it -- even
    // when the message was queued without the reader's own doorbell, which is
    // exactly the case here.
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    set_test_transport( pair.a );
    mp_server_set_terminal_size( 40, 10 );
    const reader_scope readers;
    mp_session *const seat = mp_active_session();
    REQUIRE( seat != nullptr );
    seat->caps = MP_CAP_DESCRIBE;
    mp_session_ui_enter( *seat );

    size_t slot = 99;
    mp_control got;

    // --- describe ---
    parked_wait hover( seat );
    std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    REQUIRE_FALSE( hover.returned.load() );
    {
        std::lock_guard<std::mutex> lock( seat->inbox_mutex );
        mp_control msg;
        msg.kind = mp_control_kind::describe;
        msg.describe.seq = 7;
        msg.describe.w = 4;
        seat->control_inbox.push_back( msg );
    }
    CHECK_FALSE( mp_server_take_control( slot, got ) );
    if( !wait_for_flag( hover.returned, std::chrono::seconds( 5 ) ) ) {
        mp_server_stop_reader( *seat );
    }
    hover.finish();
    CHECK( hover.returned.load() );
    CHECK( hover.type.load() == static_cast<int>( input_event_t::timeout ) );
    {
        std::lock_guard<std::mutex> lock( seat->inbox_mutex );
        REQUIRE( seat->describe_pending.has_value() );
        CHECK( seat->describe_pending->seq == 7 );
        // Due at once (the interval starts from the epoch in a fresh session),
        // which is what the seat's predicate saw.
        const auto due = seat->describe_last +
                         std::chrono::milliseconds( MP_DESCRIBE_MIN_INTERVAL_MS );
        CHECK( std::chrono::steady_clock::now() >= due );
        seat->describe_pending.reset();
    }

    // --- resize ---
    parked_wait resize( seat );
    std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    REQUIRE_FALSE( resize.returned.load() );
    {
        std::lock_guard<std::mutex> lock( seat->inbox_mutex );
        mp_control msg;
        msg.kind = mp_control_kind::resize;
        msg.resize.width = 60;
        msg.resize.height = 20;
        seat->control_inbox.push_back( msg );
    }
    CHECK_FALSE( mp_server_take_control( slot, got ) );
    if( !wait_for_flag( resize.returned, std::chrono::seconds( 5 ) ) ) {
        mp_server_stop_reader( *seat );
    }
    resize.finish();
    CHECK( resize.returned.load() );
    CHECK( resize.type.load() == static_cast<int>( input_event_t::timeout ) );

    int width = 0;
    int height = 0;
    REQUIRE( mp_server_take_resize_request( width, height ) );
    CHECK( width == 60 );
    CHECK( height == 20 );
}

TEST_CASE( "a_hover_under_a_blocking_wait_is_answered_at_its_due_instant", "[mp]" )
{
    // The production input wait blocks with -1. A hover that lands while the
    // seat is parked there and inside the answer interval must wake it when
    // that interval elapses -- nothing notifies the condition variable at the
    // due instant, so a bare predicate wait would sleep until the next key,
    // plan or resize. No other input follows here on purpose.
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.slot_index = 0;
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    const describe_seat seat = prepare_describe_seat();
    mp_server_set_terminal_size( 40, 10 );
    const reader_scope readers;

    parked_wait wait( seat.session );
    std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
    REQUIRE_FALSE( wait.returned.load() );

    // The interval starts now, so the hover below is recorded but not yet due.
    const std::chrono::steady_clock::time_point anchor = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock( seat.session->inbox_mutex );
        seat.session->describe_last = anchor;
    }
    send_control( *seat.client, describe_request( 11, 30, 5, 40 ) );
    REQUIRE( mp_server_wait_for_events( 1000 ) );
    // The seat has consumed the local control; the request is pending and the
    // wait must now be sleeping towards the due instant, not to forever.
    std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
    REQUIRE_FALSE( wait.returned.load() );

    const bool woke = wait_for_flag( wait.returned, std::chrono::seconds( 5 ) );
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - anchor );
    if( !woke ) {
        mp_server_stop_reader( *seat.session );
    }
    wait.finish();
    CHECK( woke );
    CHECK( wait.type.load() == static_cast<int>( input_event_t::timeout ) );
    // Woken by the interval, not early and not by the five-second give-up.
    CHECK( elapsed >= std::chrono::milliseconds( MP_DESCRIBE_MIN_INTERVAL_MS / 2 ) );
    CHECK( elapsed < std::chrono::milliseconds( 1500 ) );
    // Waking is not answering: the flush still builds the reply.
    {
        std::lock_guard<std::mutex> lock( seat.session->inbox_mutex );
        REQUIRE( seat.session->describe_pending.has_value() );
        CHECK( seat.session->describe_pending->seq == 11 );
    }
}

TEST_CASE( "leaving_the_ui_clears_a_pending_hover_under_the_seat_lock", "[mp]" )
{
    // The world dispatches a hover for a seat while that seat may be leaving
    // the UI, so the leave path clears the hover through the same locked helper
    // the flush and the input wait use. The assertion is the observable state:
    // once leave returns, no helper sees a request or an interval, and the
    // flush has nothing to take.
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.slot_index = 0;
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    const describe_seat seat = prepare_describe_seat();
    mp_server_set_terminal_size( 40, 10 );
    const reader_scope readers;

    send_control( *seat.client, describe_request( 13, 30, 5, 40 ) );
    REQUIRE( mp_server_wait_for_events( 1000 ) );
    // take_control consumes the non-round message through dispatch, which is
    // the world thread's write of describe_pending.
    size_t slot = 99;
    mp_control got;
    CHECK_FALSE( mp_server_take_control( slot, got ) );
    {
        std::lock_guard<std::mutex> lock( seat.session->inbox_mutex );
        REQUIRE( seat.session->describe_pending.has_value() );
        CHECK( seat.session->describe_pending->seq == 13 );
    }
    REQUIRE( mp_server_describe_pending_due( *seat.session ) );

    mp_session_ui_leave( *seat.session );

    {
        std::lock_guard<std::mutex> lock( seat.session->inbox_mutex );
        CHECK_FALSE( seat.session->describe_pending.has_value() );
        CHECK( seat.session->describe_last == std::chrono::steady_clock::time_point{} );
    }
    CHECK_FALSE( mp_server_describe_pending_due( *seat.session ) );
    mp_describe_request req;
    CHECK_FALSE( mp_server_describe_take_due( *seat.session, req ) );
}

TEST_CASE( "a_replacement_wire_is_claimed_by_its_reader_before_it_is_visible", "[mp]" )
{
    // Reconnect keeps the seat's player thread alive while the wire is swapped.
    // If the fresh transport were published while reader_running was still
    // false, the seat's input wait could read the wire itself and then race the
    // new reader for the same frames -- mp_transport::recv is not reentrant.
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    set_test_transport( pair.a );
    mp_server_set_terminal_size( 40, 10 );
    const reader_scope readers;
    mp_session *const seat = mp_active_session();
    REQUIRE( seat != nullptr );

    // Exactly the sequence install_client_on_seat uses.
    mp_server_stop_reader( *seat );
    mp_loopback_pair fresh = mp_make_loopback_pair();
    mp_server_start_reader( *seat, fresh.a );
    {
        std::lock_guard<std::mutex> lock( seat->inbox_mutex );
        CHECK( seat->reader_running );
        CHECK( seat->transport == fresh.a );
    }

    // One consumer only: the reader delivers the key, and the seat's wait sees
    // it as a queued event.
    mp_control key;
    key.kind = mp_control_kind::input;
    key.event.type = input_event_t::keyboard_char;
    key.event.sequence.push_back( 'z' );
    send_control( *fresh.b, key );
    REQUIRE( mp_server_wait_for_events( 1000 ) );

    const input_event got = mp_server_wait_for_input_of( seat, &test_input_ctx, 2000 );
    CHECK( got.type == input_event_t::keyboard_char );
    CHECK( got.sequence == std::vector<int> { 'z' } );
}

TEST_CASE( "party_snapshot_carries_each_receivers_own_frame", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    clear_creatures();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &second = g->add_player_slot();
    second.set_body();
    mp_sync_context_count();
    // Both avatars on the test map: a fresh slot starts at the blank sentinel,
    // which has no scene and no usable bubble position.
    g->player_at( 0 ).setpos( get_map(), tripoint_bub_ms( 65, 65, 0 ) );
    g->player_at( 1 ).setpos( get_map(), tripoint_bub_ms( 66, 65, 0 ) );
    mp_turn_reset_all();
    mp_plan_reset_all();
    for( size_t i = 0; i < g->player_slot_count(); ++i ) {
        g->player_at( i ).set_moves( 100 );
        g->player_at( i ).activity = player_activity();
    }
    // A step costs more than a turn's 100 AP hands out, and the party's cost
    // field must carry the real figure; give seat 0 room to commit it.
    g->player_at( 0 ).set_moves( 1000 );
    mp_activate_player( 0 );

    // Seat 0 has a map window, so its scene origin is known. Seat 1 has none --
    // a context can be between resizes -- and its body must say so instead of
    // carrying a meaningless (0,0).
    g->contexts[0].ter_view_p = g->player_at( 0 ).pos_bub();
    g->contexts[0].w_terrain = catacurses::newwin( 10, 40, point( 10, 0 ) );
    g->contexts[1].ter_view_p = g->player_at( 1 ).pos_bub();
    g->contexts[1].w_terrain = catacurses::window();

    mp_sessions_reset_for_test();
    mp_loopback_pair first_pair = mp_make_loopback_pair();
    mp_loopback_pair second_pair = mp_make_loopback_pair();
    mp_sessions().push_back( std::make_unique<mp_session>() );
    mp_sessions().push_back( std::make_unique<mp_session>() );
    mp_sessions()[0]->transport = first_pair.a;
    mp_sessions()[0]->context_index = 0;
    mp_sessions()[1]->transport = second_pair.a;
    mp_sessions()[1]->context_index = 1;

    // Seat 0 plans a step onto the tile beside it, so the party's plan, cost
    // and target are all real: a broadcast that hardcoded 0 would fail.
    mp_plan plan;
    plan.kind = mp_plan_kind::move;
    plan.target = g->player_at( 0 ).pos_bub() + tripoint_rel_ms( 1, 0, 0 );
    std::string err;
    REQUIRE( mp_plan_submit( 0, plan, err ) );
    REQUIRE( err.empty() );
    REQUIRE( mp_plan_ready( 0, err ) );
    REQUIRE( err.empty() );
    const int expected_cost = mp_plan_cost( 0, plan );
    REQUIRE( expected_cost > 0 );

    // Seat 1 passed, went away, lost its client, and carries the refusal text
    // that is the last thing the client shows it. Going away drops the seat's
    // unapplied plan (and with it the last refusal), so the text is set after
    // the transitions: the snapshot must carry whatever the seat holds now.
    mp_plan_pass( 1 );
    mp_set_away( 1, true );
    mp_set_disconnected( 1, true );
    mp_plan_set_error( 1, "needs 100 AP, you have 65" );
    mp_plan_set_phase( mp_round_phase::resolving );

    // The expected anchor for seat 0, from the same rect the broadcast reads.
    mp_scene_rect rect0;
    REQUIRE( mp_scene_mask_map_rect( rect0 ) );
    CHECK( rect0.w == 40 );
    CHECK( rect0.h == 10 );
    CHECK( rect0.x == 10 );
    CHECK( rect0.y == 0 );
    const tripoint_bub_ms origin0 = mp_scene_origin( rect0, g->contexts[0].ter_view_p );

    {
        // Seat 1 is inside a menu: its base main-UI adaptor plus the menu's.
        // The adaptors must be destroyed under their own stack, so the seat is
        // reactivated after the broadcast, which ends in the world phase.
        mp_activate_player( 1 );
        ui_adaptor base_ui;
        ui_adaptor menu_ui;
        REQUIRE( g->contexts[1].ui_stack.size() == 2 );
        mp_server_broadcast_party();
        mp_activate_player( 1 );
    }
    mp_activate_world();

    const std::vector<io_frame> first_frames = drain_frames( *first_pair.b );
    const std::vector<io_frame> second_frames = drain_frames( *second_pair.b );
    REQUIRE( first_frames.size() == 1 );
    REQUIRE( second_frames.size() == 1 );
    REQUIRE( first_frames[0].kind == mp_msg_kind::control );
    REQUIRE( second_frames[0].kind == mp_msg_kind::control );
    mp_control to_first;
    mp_control to_second;
    REQUIRE( mp_control_from_json(
                 std::string( first_frames[0].body.begin(), first_frames[0].body.end() ),
                 to_first ) );
    REQUIRE( mp_control_from_json(
                 std::string( second_frames[0].body.begin(), second_frames[0].body.end() ),
                 to_second ) );
    CHECK( to_first.kind == mp_control_kind::party );
    CHECK( to_second.kind == mp_control_kind::party );

    CHECK( to_first.party.phase == static_cast<uint8_t>( mp_round_phase::resolving ) );
    CHECK( to_second.party.phase == static_cast<uint8_t>( mp_round_phase::resolving ) );

    // Every recipient's header is its own: its slot and its own scene origin.
    CHECK( to_first.party.you == 0 );
    CHECK( to_first.party.view_known );
    CHECK( to_first.party.vx == origin0.x() );
    CHECK( to_first.party.vy == origin0.y() );
    CHECK( to_second.party.you == 1 );
    CHECK_FALSE( to_second.party.view_known );
    CHECK( to_second.party.vx == 0 );
    CHECK( to_second.party.vy == 0 );

    // The seats are the same body for everyone.
    REQUIRE( to_first.party.seats.size() == 2 );
    REQUIRE( to_second.party.seats.size() == 2 );
    for( size_t i = 0; i < 2; ++i ) {
        const mp_party_seat &a = to_first.party.seats[i];
        const mp_party_seat &b = to_second.party.seats[i];
        CHECK( a.slot == b.slot );
        CHECK( a.name == b.name );
        CHECK( a.moves == b.moves );
        CHECK( a.speed == b.speed );
        CHECK( a.ready == b.ready );
        CHECK( a.passed == b.passed );
        CHECK( a.connected == b.connected );
        CHECK( a.away == b.away );
        CHECK( a.plan == b.plan );
        CHECK( a.px == b.px );
        CHECK( a.py == b.py );
        CHECK( a.pcost == b.pcost );
        CHECK( a.menu == b.menu );
        CHECK( a.error == b.error );
    }

    const mp_party_seat &seat0 = to_first.party.seats[0];
    CHECK( seat0.slot == 0 );
    CHECK( seat0.name == g->player_at( 0 ).get_name() );
    CHECK( seat0.moves == 1000 );
    CHECK( seat0.speed == g->player_at( 0 ).get_speed() );
    CHECK( seat0.ready );
    CHECK_FALSE( seat0.passed );
    CHECK( seat0.connected );
    CHECK_FALSE( seat0.away );
    CHECK( seat0.plan == mp_plan_kind::move );
    // Bubble tiles, not scene cells: the target the seat submitted.
    CHECK( seat0.px == plan.target.x() );
    CHECK( seat0.py == plan.target.y() );
    CHECK( seat0.pcost == expected_cost );
    CHECK( seat0.menu.empty() );
    CHECK( seat0.error.empty() );

    const mp_party_seat &seat1 = to_first.party.seats[1];
    CHECK( seat1.slot == 1 );
    CHECK( seat1.name == g->player_at( 1 ).get_name() );
    CHECK( seat1.moves == 0 );
    CHECK( seat1.speed == g->player_at( 1 ).get_speed() );
    CHECK_FALSE( seat1.ready );
    CHECK( seat1.passed );
    CHECK_FALSE( seat1.connected );
    CHECK( seat1.away );
    CHECK( seat1.plan == mp_plan_kind::none );
    CHECK( seat1.px == 0 );
    CHECK( seat1.py == 0 );
    CHECK( seat1.pcost == 0 );
    CHECK( seat1.menu == "menu" );
    CHECK( seat1.error == "needs 100 AP, you have 65" );

    // The phase goes to every session too, and carries the same value.
    mp_server_broadcast_phase();
    const std::vector<io_frame> first_phase_frames = drain_frames( *first_pair.b );
    const std::vector<io_frame> second_phase_frames = drain_frames( *second_pair.b );
    REQUIRE( first_phase_frames.size() == 1 );
    REQUIRE( second_phase_frames.size() == 1 );
    mp_control phase_to_first;
    mp_control phase_to_second;
    REQUIRE( mp_control_from_json(
                 std::string( first_phase_frames[0].body.begin(),
                              first_phase_frames[0].body.end() ), phase_to_first ) );
    REQUIRE( mp_control_from_json(
                 std::string( second_phase_frames[0].body.begin(),
                              second_phase_frames[0].body.end() ), phase_to_second ) );
    CHECK( phase_to_first.kind == mp_control_kind::phase );
    CHECK( phase_to_second.kind == mp_control_kind::phase );
    CHECK( phase_to_first.phase == static_cast<uint8_t>( mp_round_phase::resolving ) );
    CHECK( phase_to_second.phase == static_cast<uint8_t>( mp_round_phase::resolving ) );

    // Nothing else went out: each broadcast sent exactly one frame per seat.
    CHECK_FALSE( first_pair.b->recv( 0 ).has_value() );
    CHECK_FALSE( second_pair.b->recv( 0 ).has_value() );

    mp_sessions_reset_for_test();
    mp_plan_reset_all();
    mp_turn_reset_all();
    mp_activate_player( 0 );
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

// Waits for the server end of a wire to report itself closed, so a test fails
// with an assertion instead of blocking on a reader that never closed it.
static bool wait_for_wire_close( mp_transport &t, const int timeout_ms )
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds( timeout_ms );
    while( t.connected() && std::chrono::steady_clock::now() < deadline ) {
        std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
    }
    return !t.connected();
}

TEST_CASE( "a_flooded_input_queue_is_bounded_and_closes_the_wire", "[mp]" )
{
    // A peer that streams keystrokes while nobody drains them must not make
    // the server allocate without bound: past the named cap the wire closes
    // and the session continues for everybody else.
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    set_test_transport( pair.a );
    mp_server_set_terminal_size( 40, 10 );
    mp_session *const seat = mp_active_session();
    REQUIRE( seat != nullptr );
    const reader_scope readers;

    mp_control key;
    key.kind = mp_control_kind::input;
    key.event.type = input_event_t::keyboard_char;
    key.event.sequence.push_back( 'x' );
    const std::string json = mp_control_to_json( key );
    const std::vector<uint8_t> frame = mp_wrap( mp_msg_kind::control,
                                     std::vector<uint8_t>( json.begin(), json.end() ) );
    for( size_t i = 0; i < MP_INPUT_BACKLOG_EVENTS + 32; ++i ) {
        pair.b->send( frame );
    }

    REQUIRE( wait_for_wire_close( *pair.a, 10000 ) );
    {
        std::lock_guard<std::mutex> lock( seat->inbox_mutex );
        CHECK( seat->input_inbox.size() <= MP_INPUT_BACKLOG_EVENTS );
    }
}

TEST_CASE( "a_flooded_control_queue_is_bounded_and_closes_the_wire", "[mp]" )
{
    // A plan's label is the one free-form string a control carries, so a few
    // large frames are enough to cross the byte cap; the queue must stop
    // growing there rather than keep what the sender writes.
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    set_test_transport( pair.a );
    mp_server_set_terminal_size( 40, 10 );
    mp_session *const seat = mp_active_session();
    REQUIRE( seat != nullptr );
    const reader_scope readers;

    mp_control plan;
    plan.kind = mp_control_kind::plan;
    plan.plan.kind = mp_plan_kind::move;
    plan.plan.label = std::string( 1024u * 1024u, 'x' );
    const std::string json = mp_control_to_json( plan );
    const std::vector<uint8_t> frame = mp_wrap( mp_msg_kind::control,
                                     std::vector<uint8_t>( json.begin(), json.end() ) );
    for( size_t i = 0; i < 16; ++i ) {
        pair.b->send( frame );
    }

    REQUIRE( wait_for_wire_close( *pair.a, 20000 ) );
    {
        std::lock_guard<std::mutex> lock( seat->inbox_mutex );
        CHECK( seat->control_inbox_bytes <= MP_CONTROL_BACKLOG_BYTES );
        CHECK( seat->control_inbox.size() <= 16 );
    }
}

#endif // MP_SERVER
