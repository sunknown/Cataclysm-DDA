#include "cata_catch.h"

#if defined(MULTIPLAYER)
#include <chrono>
#include <cstdint>
#include <vector>

#include "mp/mp_loopback_transport.h"
#include "mp/mp_transport.h"

TEST_CASE( "loopback transport delivers messages both ways", "[mp]" )
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

TEST_CASE( "loopback transport preserves message order", "[mp]" )
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

TEST_CASE( "loopback transport reports a closed peer", "[mp]" )
{
    mp_loopback_pair pair = mp_make_loopback_pair();
    REQUIRE( pair.a->connected() );
    pair.b->close();
    CHECK_FALSE( pair.a->connected() );
}

TEST_CASE( "loopback transport honours simulated latency", "[mp]" )
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
#endif // MULTIPLAYER
