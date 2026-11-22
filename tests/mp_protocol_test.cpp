#include "cata_catch.h"

#if defined(MULTIPLAYER)
#include <string>
#include <vector>

#include "debug.h"
#include "input_enums.h"
#include "mp/mp_protocol.h"
#include "mp/mp_scene.h"

TEST_CASE( "mp_wrap_tags_a_body_with_its_kind", "[mp]" )
{
    const std::vector<uint8_t> body = { 1, 2, 3 };
    const std::vector<uint8_t> wrapped = mp_wrap( mp_msg_kind::screen, body );

    mp_msg_kind kind = mp_msg_kind::control;
    std::vector<uint8_t> got;
    REQUIRE( mp_unwrap( wrapped, kind, got ) );
    CHECK( kind == mp_msg_kind::screen );
    CHECK( got == body );
}

TEST_CASE( "mp_unwrap_rejects_an_empty_payload", "[mp]" )
{
    mp_msg_kind kind = mp_msg_kind::control;
    std::vector<uint8_t> got;
    CHECK_FALSE( mp_unwrap( {}, kind, got ) );
}

TEST_CASE( "mp_unwrap_rejects_an_unknown_kind_byte", "[mp]" )
{
    mp_msg_kind kind = mp_msg_kind::control;
    std::vector<uint8_t> got;
    CHECK_FALSE( mp_unwrap( { 99, 1, 2 }, kind, got ) );
}

TEST_CASE( "mp_unwrap_accepts_scene_and_rejects_other_kinds", "[mp]" )
{
    const std::vector<uint8_t> body = { MP_SCENE_BODY_VERSION, 0, 0, 0, 1 };
    const std::vector<uint8_t> wrapped = mp_wrap( mp_msg_kind::scene, body );

    mp_msg_kind kind = mp_msg_kind::control;
    std::vector<uint8_t> got;
    REQUIRE( mp_unwrap( wrapped, kind, got ) );
    CHECK( kind == mp_msg_kind::scene );
    CHECK( got == body );

    CHECK_FALSE( mp_unwrap( { 99, 1, 2 }, kind, got ) );
}

TEST_CASE( "mp_control_round_trips_a_hello", "[mp]" )
{
    mp_control sent;
    sent.kind = mp_control_kind::hello;
    sent.hello.proto = MP_PROTOCOL_VERSION;
    sent.hello.name = "serg";
    sent.hello.width = 120;
    sent.hello.height = 40;
    sent.hello.colors_256 = true;

    mp_control got;
    REQUIRE( mp_control_from_json( mp_control_to_json( sent ), got ) );
    CHECK( got.kind == mp_control_kind::hello );
    CHECK( got.hello.proto == MP_PROTOCOL_VERSION );
    CHECK( got.hello.name == "serg" );
    CHECK( got.hello.width == 120 );
    CHECK( got.hello.height == 40 );
    CHECK( got.hello.colors_256 );
}

TEST_CASE( "mp_control_round_trips_a_rejection_with_its_reason", "[mp]" )
{
    mp_control sent;
    sent.kind = mp_control_kind::hello_ack;
    sent.hello_ack.accepted = false;
    sent.hello_ack.reason = "protocol version mismatch: server 1, client 2";

    mp_control got;
    REQUIRE( mp_control_from_json( mp_control_to_json( sent ), got ) );
    CHECK( got.kind == mp_control_kind::hello_ack );
    CHECK_FALSE( got.hello_ack.accepted );
    CHECK( got.hello_ack.reason == sent.hello_ack.reason );
}

TEST_CASE( "mp_control_round_trips_a_keyboard_input_event", "[mp]" )
{
    mp_control sent;
    sent.kind = mp_control_kind::input;
    sent.event.type = input_event_t::keyboard_code;
    sent.event.sequence = { 106 };
    sent.event.modifiers = { keymod_t::ctrl, keymod_t::shift };
    sent.event.text = "j";
    sent.event.edit = "";
    sent.event.edit_refresh = false;

    mp_control got;
    REQUIRE( mp_control_from_json( mp_control_to_json( sent ), got ) );
    CHECK( got.kind == mp_control_kind::input );
    CHECK( got.event.type == input_event_t::keyboard_code );
    CHECK( got.event.sequence == std::vector<int> { 106 } );
    CHECK( got.event.modifiers == std::set<keymod_t> { keymod_t::ctrl, keymod_t::shift } );
    CHECK( got.event.text == "j" );
    CHECK_FALSE( got.event.edit_refresh );
}

TEST_CASE( "mp_control_round_trips_a_mouse_input_event", "[mp]" )
{
    mp_control sent;
    sent.kind = mp_control_kind::input;
    sent.event.type = input_event_t::mouse;
    sent.event.sequence = { 0 };
    sent.event.mouse_pos = point( 37, 11 );

    mp_control got;
    REQUIRE( mp_control_from_json( mp_control_to_json( sent ), got ) );
    CHECK( got.event.type == input_event_t::mouse );
    CHECK( got.event.mouse_pos == point( 37, 11 ) );
}

TEST_CASE( "mp_control_round_trips_a_resize_and_a_cursor", "[mp]" )
{
    mp_control resize;
    resize.kind = mp_control_kind::resize;
    resize.resize.width = 90;
    resize.resize.height = 30;

    mp_control got_resize;
    REQUIRE( mp_control_from_json( mp_control_to_json( resize ), got_resize ) );
    CHECK( got_resize.resize.width == 90 );
    CHECK( got_resize.resize.height == 30 );

    mp_control cursor;
    cursor.kind = mp_control_kind::cursor;
    cursor.cursor.x = 4;
    cursor.cursor.y = 5;
    cursor.cursor.visible = true;

    mp_control got_cursor;
    REQUIRE( mp_control_from_json( mp_control_to_json( cursor ), got_cursor ) );
    CHECK( got_cursor.cursor.x == 4 );
    CHECK( got_cursor.cursor.y == 5 );
    CHECK( got_cursor.cursor.visible );
}

TEST_CASE( "mp_control_from_json_rejects_malformed_input", "[mp]" )
{
    mp_control got;
    CHECK_FALSE( mp_control_from_json( "not json at all", got ) );
    CHECK_FALSE( mp_control_from_json( "{\"t\":\"nonsense\"}", got ) );
}

TEST_CASE( "mp_control_round_trips_a_bye_with_its_reason", "[mp]" )
{
    mp_control sent;
    sent.kind = mp_control_kind::bye;
    sent.bye_reason = "client quit";

    mp_control got;
    REQUIRE( mp_control_from_json( mp_control_to_json( sent ), got ) );
    CHECK( got.kind == mp_control_kind::bye );
    CHECK( got.bye_reason == "client quit" );
}

TEST_CASE( "mp_control_from_json_ignores_a_field_it_does_not_know", "[mp]" )
{
    // A peer one version ahead may add fields this build has never heard of.
    // Those must be skipped silently: JsonObject reports every member that
    // went unread as a debugmsg when it is destroyed, so without
    // allow_omitted_members() a peer could raise a debug error prompt on the
    // server just by adding a field. Round-tripping our own writer can never
    // catch this -- the writer and reader visit the same members by
    // construction -- so the payload here is hand-built with an extra one.
    mp_control got;
    bool parsed = false;
    const std::string dmsg = capture_debugmsg_during( [&got, &parsed]() {
        parsed = mp_control_from_json(
                     R"({"t":"bye","reason":"client quit","field_from_the_future":7})", got );
    } );

    CHECK( dmsg.empty() );
    REQUIRE( parsed );
    CHECK( got.kind == mp_control_kind::bye );
    CHECK( got.bye_reason == "client quit" );
}
TEST_CASE( "mp_control_round_trips_a_log_snapshot", "[mp]" )
{
    mp_control sent;
    sent.kind = mp_control_kind::log;
    sent.log.seq = 17;
    sent.log.ui = true;
    sent.log.x = 134;
    sent.log.y = 2;
    sent.log.w = 26;
    sent.log.h = 33;
    sent.log.top = false;

    mp_log_line first;
    first.runs.push_back( { "You hit the zombie for ", 8 } );
    first.runs.push_back( { "5", 1 } );
    first.runs.push_back( { " damage.", 8 } );
    sent.log.lines.push_back( first );

    mp_log_line second;
    second.runs.push_back( { "You hear a whack!", 8 } );
    sent.log.lines.push_back( second );

    mp_control got;
    REQUIRE( mp_control_from_json( mp_control_to_json( sent ), got ) );
    CHECK( got.kind == mp_control_kind::log );
    CHECK( got.log.seq == 17 );
    CHECK( got.log.ui );
    CHECK( got.log.x == 134 );
    CHECK( got.log.y == 2 );
    CHECK( got.log.w == 26 );
    CHECK( got.log.h == 33 );
    CHECK_FALSE( got.log.top );
    REQUIRE( got.log.lines.size() == 2 );
    REQUIRE( got.log.lines[0].runs.size() == 3 );
    CHECK( got.log.lines[0].runs[0].text == "You hit the zombie for " );
    CHECK( got.log.lines[0].runs[1].text == "5" );
    CHECK( got.log.lines[0].runs[1].color == 1 );
    CHECK( got.log.lines[1].runs[0].text == "You hear a whack!" );
}

TEST_CASE( "mp_control_rejects_a_malformed_log", "[mp]" )
{
    mp_control got;

    // A body missing the snapshot members is not a log message.
    CHECK_FALSE( mp_control_from_json( R"({"t":"log","seq":1})", got ) );

    // More than 200 lines.
    std::string many = R"({"t":"log","seq":1,"ui":true,"x":0,"y":0,"w":0,"h":0,"top":false,"lines":[)";
    for( int i = 0; i < 201; i++ ) {
        if( i > 0 ) {
            many += ",";
        }
        many += "[]";
    }
    many += "]}";
    CHECK_FALSE( mp_control_from_json( many, got ) );

    // More than 256 runs in one line.
    std::string runs = R"({"t":"log","seq":1,"ui":true,"x":0,"y":0,"w":0,"h":0,"top":false,"lines":[[)";
    for( int i = 0; i < 257; i++ ) {
        if( i > 0 ) {
            runs += ",";
        }
        runs += R"({"t":"x","c":1})";
    }
    runs += "]]}";
    CHECK_FALSE( mp_control_from_json( runs, got ) );

    // A palette index outside 0..255.
    CHECK_FALSE( mp_control_from_json(
                     R"({"t":"log","seq":1,"ui":true,"x":0,"y":0,"w":0,"h":0,"top":false,"lines":[[{"t":"x","c":256}]]})",
                     got ) );

    // More than 64 KiB of text.
    std::string big =
        R"({"t":"log","seq":1,"ui":true,"x":0,"y":0,"w":0,"h":0,"top":false,"lines":[[{"t":")";
    big.append( 65537, 'x' );
    big += R"(","c":1}]]})";
    CHECK_FALSE( mp_control_from_json( big, got ) );
}
#endif // MULTIPLAYER
