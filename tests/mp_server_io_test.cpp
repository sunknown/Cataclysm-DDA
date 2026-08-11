#include "cata_catch.h"

#if defined(MULTIPLAYER)
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "avatar.h"
#include "game.h"
#include "game_constants.h"
#include "game_test_helper.h"
#include "input_enums.h"
#include "mp/mp_cell.h"
#include "mp/mp_context.h"
#include "mp/mp_loopback_transport.h"
#include "mp/mp_protocol.h"
#include "mp/mp_screen_codec.h"
#include "mp/mp_server_input.h"
#include "mp/mp_server_screen.h"
#include "mp/mp_session.h"

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
    CHECK_FALSE( mp_server_handshake( *pair.a, session, reason ) );
    CHECK_FALSE( reason.empty() );
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
#endif // MULTIPLAYER
