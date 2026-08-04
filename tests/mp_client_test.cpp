#include "cata_catch.h"

#if defined(MULTIPLAYER)
#include <cstdint>
#include <string>
#include <vector>

#include "input_enums.h"
#include "mp/mp_cell.h"
#include "mp/mp_client_screen.h"
#include "mp/mp_protocol.h"
#include "mp/mp_screen_codec.h"

TEST_CASE( "client_parses_host_and_port", "[mp]" )
{
    std::string host;
    int port = 0;
    REQUIRE( mp_parse_host_port( "127.0.0.1:4321", host, port ) );
    CHECK( host == "127.0.0.1" );
    CHECK( port == 4321 );

    CHECK_FALSE( mp_parse_host_port( "127.0.0.1", host, port ) );
    CHECK_FALSE( mp_parse_host_port( "127.0.0.1:", host, port ) );
    CHECK_FALSE( mp_parse_host_port( "127.0.0.1:notaport", host, port ) );
    CHECK_FALSE( mp_parse_host_port( "", host, port ) );
}

TEST_CASE( "client_applies_a_screen_message", "[mp]" )
{
    mp_screen_grid source( 20, 5 );
    mp_cell cell;
    cell.ch = "@";
    cell.fg = 3;
    cell.bg = 1;
    source.set( 4, 2, cell );

    mp_screen_grid client_screen( 20, 5 );
    mp_client_state state;
    const std::vector<uint8_t> payload = mp_wrap( mp_msg_kind::screen, mp_screen_encode( source ) );

    CHECK( mp_client_apply_message( payload, client_screen, state ) == mp_client_event::screen );
    CHECK( client_screen.at( 4, 2 ) == cell );
}

TEST_CASE( "client_records_the_cursor_position", "[mp]" )
{
    mp_screen_grid client_screen( 20, 5 );
    mp_client_state state;

    mp_control cursor;
    cursor.kind = mp_control_kind::cursor;
    cursor.cursor.x = 7;
    cursor.cursor.y = 3;
    cursor.cursor.visible = true;
    const std::string json = mp_control_to_json( cursor );
    const std::vector<uint8_t> payload =
        mp_wrap( mp_msg_kind::control, std::vector<uint8_t>( json.begin(), json.end() ) );

    CHECK( mp_client_apply_message( payload, client_screen, state ) == mp_client_event::cursor );
    CHECK( state.cursor_x == 7 );
    CHECK( state.cursor_y == 3 );
    CHECK( state.cursor_visible );
}

TEST_CASE( "client_reports_a_rejection_with_its_reason", "[mp]" )
{
    mp_screen_grid client_screen( 20, 5 );
    mp_client_state state;

    mp_control ack;
    ack.kind = mp_control_kind::hello_ack;
    ack.hello_ack.accepted = false;
    ack.hello_ack.reason = "protocol version mismatch";
    const std::string json = mp_control_to_json( ack );
    const std::vector<uint8_t> payload =
        mp_wrap( mp_msg_kind::control, std::vector<uint8_t>( json.begin(), json.end() ) );

    CHECK( mp_client_apply_message( payload, client_screen, state ) == mp_client_event::rejected );
    CHECK( state.reject_reason == "protocol version mismatch" );
}

TEST_CASE( "client_ignores_a_malformed_message_without_crashing", "[mp]" )
{
    mp_screen_grid client_screen( 20, 5 );
    mp_client_state state;

    CHECK( mp_client_apply_message( { 99, 1, 2 }, client_screen, state ) == mp_client_event::none );
    CHECK( mp_client_apply_message( {}, client_screen, state ) == mp_client_event::none );

    const std::vector<uint8_t> bad_json =
        mp_wrap( mp_msg_kind::control, std::vector<uint8_t> { 'n', 'o', 'p', 'e' } );
    CHECK( mp_client_apply_message( bad_json, client_screen, state ) == mp_client_event::none );
}

TEST_CASE( "client_encodes_a_keystroke_as_a_control_message", "[mp]" )
{
    input_event ev;
    ev.type = input_event_t::keyboard_char;
    ev.sequence = { 'k' };

    const std::vector<uint8_t> payload = mp_client_encode_input( ev );
    mp_msg_kind kind = mp_msg_kind::screen;
    std::vector<uint8_t> body;
    REQUIRE( mp_unwrap( payload, kind, body ) );
    REQUIRE( kind == mp_msg_kind::control );

    mp_control got;
    REQUIRE( mp_control_from_json( std::string( body.begin(), body.end() ), got ) );
    CHECK( got.kind == mp_control_kind::input );
    CHECK( got.event.sequence == std::vector<int> { 'k' } );
}

TEST_CASE( "client_reuses_a_colour_pair_for_a_repeated_combination", "[mp]" )
{
    mp_client_reset_pairs_for_test();
    const int first = mp_client_pair_for( 3, 1 );
    CHECK( mp_client_pair_for( 3, 1 ) == first );
    CHECK( mp_client_pair_for( 4, 1 ) != first );
}

TEST_CASE( "client_falls_back_to_a_safe_pair_when_the_cache_is_exhausted", "[mp]" )
{
    // cursesport only has a hundred pairs; running past the cache must degrade
    // to readable text rather than corrupt the pair table.
    mp_client_reset_pairs_for_test();
    for( int i = 0; i < 200; ++i ) {
        const int pair = mp_client_pair_for( static_cast<uint8_t>( i % 256 ),
                                             static_cast<uint8_t>( i / 16 ) );
        REQUIRE( pair >= 1 );
        REQUIRE( pair < 100 );
    }
}
#endif // MULTIPLAYER
