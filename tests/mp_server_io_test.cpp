#include "cata_catch.h"

#if defined(MULTIPLAYER)
#include <cstdint>
#include <string>
#include <vector>

#include "input_enums.h"
#include "mp/mp_cell.h"
#include "mp/mp_loopback_transport.h"
#include "mp/mp_protocol.h"
#include "mp/mp_screen_codec.h"
#include "mp/mp_server_input.h"
#include "mp/mp_server_screen.h"

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

TEST_CASE( "server_blits_a_row_into_the_screen_grid", "[mp]" )
{
    mp_server_reset_for_test();
    mp_server_set_terminal_size( 40, 10 );

    mp_server_blit_row( 2, 1, text_row( "hello" ) );

    CHECK( mp_server_grid().at( 2, 1 ).ch == "h" );
    CHECK( mp_server_grid().at( 6, 1 ).ch == "o" );
    CHECK( mp_server_grid().at( 7, 1 ).ch == " " );
}

TEST_CASE( "server_sends_nothing_when_nothing_changed", "[mp]" )
{
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    mp_server_set_terminal_size( 40, 10 );
    mp_server_set_transport( pair.a );

    mp_server_flush_screen();
    REQUIRE( pair.b->recv( 0 ).has_value() );

    mp_server_flush_screen();
    CHECK_FALSE( pair.b->recv( 0 ).has_value() );
}

TEST_CASE( "server_sends_a_delta_far_smaller_than_a_full_screen", "[mp]" )
{
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    mp_server_set_terminal_size( 80, 24 );
    mp_server_set_transport( pair.a );

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
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    mp_server_set_terminal_size( 40, 10 );
    mp_server_set_transport( pair.a );

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
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    mp_server_set_terminal_size( 40, 10 );
    mp_server_set_transport( pair.a );

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
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    mp_server_set_terminal_size( 40, 10 );
    mp_server_set_transport( pair.a );
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
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    mp_server_set_terminal_size( 40, 10 );
    mp_server_set_transport( pair.a );

    const input_event got = mp_server_wait_for_input( 10 );
    CHECK( got.type == input_event_t::timeout );
}

TEST_CASE( "server_records_a_resize_request_without_touching_the_grid_itself", "[mp]" )
{
    // The grid is rebuilt by the curses backend, which must also rebuild the
    // game's windows; the module only records what the client asked for.
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    mp_server_set_terminal_size( 40, 10 );
    mp_server_set_transport( pair.a );

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

TEST_CASE( "server_sends_the_cursor_position_with_the_screen", "[mp]" )
{
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    mp_server_set_terminal_size( 40, 10 );
    mp_server_set_transport( pair.a );

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
    mp_server_reset_for_test();
    mp_loopback_pair pair = mp_make_loopback_pair();
    mp_server_set_terminal_size( 40, 10 );
    mp_server_set_transport( pair.a );
    pair.b->close();

    mp_server_blit_row( 0, 0, text_row( "gone" ) );
    mp_server_flush_screen();
    const input_event got = mp_server_wait_for_input( 10 );
    CHECK( got.type == input_event_t::timeout );
}
#endif // MULTIPLAYER
