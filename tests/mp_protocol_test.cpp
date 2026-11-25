#include "cata_catch.h"

#if defined(MULTIPLAYER)
#include <string>
#include <vector>

#include "debug.h"
#include "input_enums.h"
#include "mp/mp_protocol.h"
#include "mp/mp_scene.h"
#include "mp/mp_turn.h"

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

TEST_CASE( "mp_control_round_trips_a_hello_ack_with_capabilities", "[mp]" )
{
    mp_control sent;
    sent.kind = mp_control_kind::hello_ack;
    sent.hello_ack.accepted = true;
    sent.hello_ack.caps = MP_SERVER_CAPS;

    mp_control got;
    REQUIRE( mp_control_from_json( mp_control_to_json( sent ), got ) );
    CHECK( got.kind == mp_control_kind::hello_ack );
    CHECK( got.hello_ack.accepted );
    CHECK( got.hello_ack.caps == 7u );
    CHECK( MP_CAP_SCENE == 1u );
    CHECK( MP_CAP_LOG == 2u );
    CHECK( MP_CAP_DESCRIBE == 4u );
    CHECK( MP_SERVER_CAPS == 7u );

    // A server that predates capability advertisement accepts without caps.
    REQUIRE( mp_control_from_json( R"({"t":"hello_ack","ok":true,"reason":""})", got ) );
    CHECK( got.hello_ack.caps == 0u );
}

TEST_CASE( "mp_control_round_trips_a_describe_request", "[mp]" )
{
    mp_control got;
    REQUIRE( mp_control_from_json( R"({"t":"describe","seq":12,"x":147,"y":39,"w":40})",
                                   got ) );
    CHECK( got.kind == mp_control_kind::describe );
    CHECK( got.describe.seq == 12 );
    CHECK( got.describe.frame == 0 );
    CHECK( got.describe.x == 147 );
    CHECK( got.describe.y == 39 );
    CHECK( got.describe.w == 40 );
    CHECK( got.describe.lines.empty() );

    // The writer names the response members too, so our own request survives
    // the trip with an explicit empty answer.
    REQUIRE( mp_control_from_json( mp_control_to_json( got ), got ) );
    CHECK( got.kind == mp_control_kind::describe );
    CHECK( got.describe.seq == 12 );
    CHECK( got.describe.x == 147 );
    CHECK( got.describe.y == 39 );
    CHECK( got.describe.w == 40 );
    CHECK( got.describe.lines.empty() );
}

TEST_CASE( "mp_control_round_trips_a_describe_response", "[mp]" )
{
    mp_control sent;
    sent.kind = mp_control_kind::describe;
    sent.describe.seq = 13;
    sent.describe.frame = 417;
    sent.describe.x = 20;
    sent.describe.y = 5;
    sent.describe.w = 30;

    mp_log_line first;
    first.runs.push_back( { "a zombie corpse", 2 } );
    first.runs.push_back( { " lies here.", 8 } );
    sent.describe.lines.push_back( first );
    // An empty inner line is a blank separator the tooltip renders as-is.
    sent.describe.lines.push_back( mp_log_line() );

    mp_control got;
    REQUIRE( mp_control_from_json( mp_control_to_json( sent ), got ) );
    CHECK( got.kind == mp_control_kind::describe );
    CHECK( got.describe.seq == 13 );
    CHECK( got.describe.frame == 417 );
    CHECK( got.describe.x == 20 );
    CHECK( got.describe.y == 5 );
    CHECK( got.describe.w == 30 );
    REQUIRE( got.describe.lines.size() == 2 );
    REQUIRE( got.describe.lines[0].runs.size() == 2 );
    CHECK( got.describe.lines[0].runs[0].text == "a zombie corpse" );
    CHECK( got.describe.lines[0].runs[0].color == 2 );
    CHECK( got.describe.lines[0].runs[1].text == " lies here." );
    CHECK( got.describe.lines[0].runs[1].color == 8 );
    CHECK( got.describe.lines[1].runs.empty() );
}

TEST_CASE( "mp_control_rejects_a_malformed_describe", "[mp]" )
{
    mp_control got;

    // Every required member must be present; none of them has a default.
    CHECK_FALSE( mp_control_from_json( R"({"t":"describe","seq":1,"y":0,"w":1})", got ) );
    CHECK_FALSE( mp_control_from_json( R"({"t":"describe","seq":1,"x":0,"w":1})", got ) );
    CHECK_FALSE( mp_control_from_json( R"({"t":"describe","seq":1,"x":0,"y":0})", got ) );
    CHECK_FALSE( mp_control_from_json( R"({"t":"describe","x":0,"y":0,"w":1})", got ) );

    // A hover answer is a tooltip, not a log: 24 lines, 256 runs, 16 KiB.
    std::string many = R"({"t":"describe","seq":1,"x":0,"y":0,"w":1,"lines":[)";
    for( int i = 0; i < 25; i++ ) {
        if( i > 0 ) {
            many += ",";
        }
        many += "[]";
    }
    many += "]}";
    CHECK_FALSE( mp_control_from_json( many, got ) );

    std::string runs =
        R"({"t":"describe","seq":1,"x":0,"y":0,"w":1,"lines":[[)";
    for( int i = 0; i < 257; i++ ) {
        if( i > 0 ) {
            runs += ",";
        }
        runs += R"({"t":"x","c":1})";
    }
    runs += "]]}";
    CHECK_FALSE( mp_control_from_json( runs, got ) );

    CHECK_FALSE( mp_control_from_json(
                     R"({"t":"describe","seq":1,"x":0,"y":0,"w":1,"lines":[[{"t":"x","c":256}]]})",
                     got ) );

    std::string big =
        R"({"t":"describe","seq":1,"x":0,"y":0,"w":1,"lines":[[{"t":")";
    big.append( 16u * 1024u + 1u, 'x' );
    big += R"(","c":1}]]})";
    CHECK_FALSE( mp_control_from_json( big, got ) );

    // A body that is not an object is not a message at all.
    CHECK_FALSE( mp_control_from_json( "[1,2,3]", got ) );
}

TEST_CASE( "mp_control_accepts_a_describe_at_the_limits", "[mp]" )
{
    // Exactly 24 lines, exactly 256 runs on one line and exactly 16 KiB of
    // text must pass; each rejection above is one past one of these.
    std::string at_limits =
        R"({"t":"describe","seq":1,"x":0,"y":0,"w":1,"lines":[[)";
    for( int i = 0; i < 256; i++ ) {
        if( i > 0 ) {
            at_limits += ",";
        }
        at_limits += R"({"t":")";
        if( i == 0 ) {
            at_limits.append( 16u * 1024u, 'x' );
        }
        at_limits += R"(","c":0})";
    }
    at_limits += "]";
    for( int i = 1; i < 24; i++ ) {
        at_limits += ",[]";
    }
    at_limits += "]}";

    mp_control got;
    REQUIRE( mp_control_from_json( at_limits, got ) );
    REQUIRE( got.describe.lines.size() == 24 );
    REQUIRE( got.describe.lines[0].runs.size() == 256 );
    CHECK( got.describe.lines[0].runs[0].text.size() == 16u * 1024u );
}

TEST_CASE( "mp_control_round_trips_a_plan", "[mp]" )
{
    mp_control sent;
    sent.kind = mp_control_kind::plan;
    sent.plan.seq = 7;
    sent.plan.kind = mp_plan_kind::melee;
    sent.plan.x = 33;
    sent.plan.y = 44;
    sent.plan.z = -1;
    sent.plan.cost = 65;
    sent.plan.label = "zombie";

    mp_control got;
    REQUIRE( mp_control_from_json( mp_control_to_json( sent ), got ) );
    CHECK( got.kind == mp_control_kind::plan );
    CHECK( got.plan.seq == 7 );
    CHECK( got.plan.kind == mp_plan_kind::melee );
    CHECK( got.plan.x == 33 );
    CHECK( got.plan.y == 44 );
    CHECK( got.plan.z == -1 );
    CHECK( got.plan.cost == 65 );
    CHECK( got.plan.label == "zombie" );
}

TEST_CASE( "mp_control_round_trips_a_party", "[mp]" )
{
    mp_control sent;
    sent.kind = mp_control_kind::party;
    sent.party.phase = 1;
    sent.party.you = 1;
    sent.party.view_known = true;
    sent.party.vx = -37;
    sent.party.vy = 42;
    mp_party_seat a;
    a.slot = 0; a.moves = 120; a.speed = 120; a.ready = true;
    a.plan = mp_plan_kind::move; a.px = 10; a.py = 11; a.pcost = 100; a.menu = "inventory";
    a.name = "Alice";
    a.availability = "available";
    mp_party_seat b;
    b.slot = 1; b.moves = 0; b.speed = 100; b.passed = true; b.connected = true;
    b.name = "Bob";
    b.error = "needs 100 AP, you have 65";
    b.availability = "recovering";
    sent.party.seats = { a, b };

    mp_control got;
    REQUIRE( mp_control_from_json( mp_control_to_json( sent ), got ) );
    CHECK( got.kind == mp_control_kind::party );
    CHECK( got.party.phase == 1 );
    CHECK( got.party.you == 1 );
    CHECK( got.party.view_known );
    CHECK( got.party.vx == -37 );
    CHECK( got.party.vy == 42 );
    REQUIRE( got.party.seats.size() == 2 );
    CHECK( got.party.seats[0].slot == 0 );
    CHECK( got.party.seats[0].moves == 120 );
    CHECK( got.party.seats[0].speed == 120 );
    CHECK( got.party.seats[0].ready );
    CHECK_FALSE( got.party.seats[0].passed );
    CHECK_FALSE( got.party.seats[0].connected );
    CHECK_FALSE( got.party.seats[0].away );
    CHECK( got.party.seats[0].plan == mp_plan_kind::move );
    CHECK( got.party.seats[0].px == 10 );
    CHECK( got.party.seats[0].py == 11 );
    CHECK( got.party.seats[0].pcost == 100 );
    CHECK( got.party.seats[0].menu == "inventory" );
    CHECK( got.party.seats[0].name == "Alice" );
    CHECK( got.party.seats[0].error.empty() );
    CHECK( got.party.seats[0].availability == "available" );
    CHECK( got.party.seats[1].passed );
    CHECK( got.party.seats[1].connected );
    CHECK( got.party.seats[1].name == "Bob" );
    CHECK( got.party.seats[1].error == "needs 100 AP, you have 65" );
    CHECK( got.party.seats[1].availability == "recovering" );
}

TEST_CASE( "mp_control_party_requires_a_known_availability", "[mp][ap_debt]" )
{
    // A seat with no state field is refused: the client cannot draw a state it
    // was never told, and guessing `available` would let it plan for a seat in
    // recovery.
    mp_control got;
    CHECK_FALSE( mp_control_from_json(
        R"({"t":"party","phase":0,"seats":[{"slot":0,"moves":5,"speed":100,"ready":false,"passed":false,"connected":true,"away":false,"plan":"none","px":0,"py":0,"pcost":0,"menu":"","name":"A","error":""}]})",
        got ) );
    // An unknown state is refused as well, not carried as text.
    CHECK_FALSE( mp_control_from_json(
        R"({"t":"party","phase":0,"seats":[{"slot":0,"moves":5,"speed":100,"ready":false,"passed":false,"connected":true,"away":false,"plan":"none","px":0,"py":0,"pcost":0,"menu":"","name":"A","error":"","availability":"tired"}]})",
        got ) );

    // Every state the server can send round-trips by name.
    for( const mp_action_availability state :
         { mp_action_availability::available, mp_action_availability::recovering,
           mp_action_availability::passed, mp_action_availability::busy,
           mp_action_availability::away, mp_action_availability::disconnected,
           mp_action_availability::dead, mp_action_availability::unavailable } ) {
        mp_control sent;
        sent.kind = mp_control_kind::party;
        mp_party_seat seat;
        seat.slot = 0;
        seat.name = "A";
        seat.availability = mp_action_availability_name( state );
        sent.party.seats = { seat };
        mp_control round_tripped;
        REQUIRE( mp_control_from_json( mp_control_to_json( sent ), round_tripped ) );
        REQUIRE( round_tripped.party.seats.size() == 1 );
        CHECK( round_tripped.party.seats[0].availability ==
               mp_action_availability_name( state ) );
    }
}

TEST_CASE( "mp_control_party_without_the_header_defaults_to_no_anchor", "[mp]" )
{
    // Bodies from before the header existed carry no you/view_known/vx/vy.
    // The decoder must default them rather than refuse the message: a client
    // (or an older peer) with no anchor simply does not place plans.
    mp_control got;
    REQUIRE( mp_control_from_json( R"({"t":"party","phase":2,"seats":[]})", got ) );
    CHECK( got.party.phase == 2 );
    CHECK( got.party.you == -1 );
    CHECK_FALSE( got.party.view_known );
    CHECK( got.party.vx == 0 );
    CHECK( got.party.vy == 0 );
}

TEST_CASE( "mp_control_round_trips_plan_ready_and_phase", "[mp]" )
{
    // plan_cancel, plan_ready and plan_pass carry only the client sequence.
    for( const mp_control_kind kind :
         { mp_control_kind::plan_cancel, mp_control_kind::plan_ready,
           mp_control_kind::plan_pass } ) {
        mp_control sent;
        sent.kind = kind;
        sent.plan.seq = 19;

        mp_control got;
        REQUIRE( mp_control_from_json( mp_control_to_json( sent ), got ) );
        CHECK( got.kind == kind );
        CHECK( got.plan.seq == 19 );
    }

    mp_control sent;
    sent.kind = mp_control_kind::phase;
    sent.phase = 2;

    mp_control got;
    REQUIRE( mp_control_from_json( mp_control_to_json( sent ), got ) );
    CHECK( got.kind == mp_control_kind::phase );
    CHECK( got.phase == 2 );
}

TEST_CASE( "mp_control_rejects_an_unknown_plan_kind", "[mp]" )
{
    mp_control got;
    CHECK_FALSE( mp_control_from_json(
                     R"({"t":"plan","seq":1,"kind":"nonsense","x":0,"y":0,"z":0,"cost":0,"label":""})",
                     got ) );
    // The party seats carry the same enum, so an unknown value must fail there
    // too rather than falling back to none.
    CHECK_FALSE( mp_control_from_json(
                     R"({"t":"party","phase":1,"seats":[{"slot":0,"moves":0,"speed":0,"ready":false,)"
                     R"("passed":false,"connected":false,"away":false,"plan":"nonsense",)"
                     R"("px":0,"py":0,"pcost":0,"menu":"","name":"","error":""}]})",
                     got ) );
}

TEST_CASE( "mp_control_from_json_ignores_an_unknown_describe_member", "[mp]" )
{
    mp_control got;
    bool parsed = false;
    const std::string dmsg = capture_debugmsg_during( [&got, &parsed]() {
        parsed = mp_control_from_json(
                     R"({"t":"describe","seq":1,"x":2,"y":3,"w":4,"field_from_the_future":7})",
                     got );
    } );

    CHECK( dmsg.empty() );
    REQUIRE( parsed );
    CHECK( got.kind == mp_control_kind::describe );
    CHECK( got.describe.x == 2 );
    CHECK( got.describe.y == 3 );
    CHECK( got.describe.w == 4 );
}
#endif // MULTIPLAYER
