#include "cata_catch.h"

#if defined(MULTIPLAYER)
#include <chrono>
#include <cstdint>
#include <vector>

#include "mp/mp_loopback_transport.h"
#include "mp/mp_protocol.h"
#include "mp/mp_scene.h"
#include "mp/mp_transport.h"

TEST_CASE( "loopback_transport_delivers_messages_both_ways", "[mp]" )
{
    mp_loopback_pair pair = mp_make_loopback_pair();

    pair.a->send( { 1, 2, 3 } );
    const std::optional<std::vector<uint8_t>> at_b = pair.b->recv( 0 );
    REQUIRE( at_b.has_value() );
    CHECK( *at_b == std::vector<uint8_t> { 1, 2, 3 } );

    pair.b->send( { 9 } );
    const std::optional<std::vector<uint8_t>> at_a = pair.a->recv( 0 );
    REQUIRE( at_a.has_value() );
    CHECK( *at_a == std::vector<uint8_t> { 9 } );
}

TEST_CASE( "loopback_transport_preserves_message_order", "[mp]" )
{
    mp_loopback_pair pair = mp_make_loopback_pair();
    pair.a->send( { 1 } );
    pair.a->send( { 2 } );
    pair.a->send( { 3 } );

    CHECK( pair.b->recv( 0 ) == std::vector<uint8_t> { 1 } );
    CHECK( pair.b->recv( 0 ) == std::vector<uint8_t> { 2 } );
    CHECK( pair.b->recv( 0 ) == std::vector<uint8_t> { 3 } );
    CHECK_FALSE( pair.b->recv( 0 ).has_value() );
}

TEST_CASE( "loopback_transport_reports_a_closed_peer", "[mp]" )
{
    mp_loopback_pair pair = mp_make_loopback_pair();
    REQUIRE( pair.a->connected() );
    pair.b->close();
    CHECK_FALSE( pair.a->connected() );
}

TEST_CASE( "loopback_transport_honours_simulated_latency", "[mp]" )
{
    // 20 ms each way. A message sent now must not be readable immediately,
    // and must be readable when we are willing to wait long enough.
    mp_loopback_pair pair = mp_make_loopback_pair( 20 );
    pair.a->send( { 42 } );

    CHECK_FALSE( pair.b->recv( 0 ).has_value() );

    const std::optional<std::vector<uint8_t>> got = pair.b->recv( 200 );
    REQUIRE( got.has_value() );
    CHECK( *got == std::vector<uint8_t> { 42 } );
}

TEST_CASE( "loopback_transport_drop_pending_screen_messages_drops_scene_frames", "[mp]" )
{
    mp_loopback_pair pair = mp_make_loopback_pair();
    pair.a->send( mp_wrap( mp_msg_kind::control, { 'k' } ) );
    pair.a->send( mp_wrap( mp_msg_kind::screen, { 1, 2, 3 } ) );
    pair.a->send( mp_wrap( mp_msg_kind::scene, { MP_SCENE_BODY_VERSION, 0, 0, 0, 1 } ) );
    pair.a->send( mp_wrap( mp_msg_kind::control, { 'k' } ) );

    pair.a->drop_pending_screen_messages();

    int controls = 0;
    int screens = 0;
    int scenes = 0;
    while( const std::optional<std::vector<uint8_t>> msg = pair.b->recv( 0 ) ) {
        mp_msg_kind kind = mp_msg_kind::control;
        std::vector<uint8_t> body;
        REQUIRE( mp_unwrap( *msg, kind, body ) );
        controls += kind == mp_msg_kind::control ? 1 : 0;
        screens += kind == mp_msg_kind::screen ? 1 : 0;
        scenes += kind == mp_msg_kind::scene ? 1 : 0;
    }
    CHECK( controls == 2 );
    CHECK( screens == 0 );
    CHECK( scenes == 0 );
}
#endif // MULTIPLAYER
