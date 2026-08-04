#include "cata_catch.h"

#if defined(MULTIPLAYER)
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "mp/mp_tcp_transport.h"
#include "mp/mp_transport.h"

// Port 0 asks the OS for a free port, so the suite never collides with a
// running server or with a parallel test run.
TEST_CASE( "tcp_transport_carries_messages_over_a_real_socket", "[mp]" )
{
    std::string err;
    std::optional<mp_listener> listener = mp_listener::bind( 0, err );
    REQUIRE( listener.has_value() );
    REQUIRE( err.empty() );

    std::shared_ptr<mp_transport> client = mp_tcp_connect( "127.0.0.1", listener->port(), err );
    REQUIRE( client );
    REQUIRE( err.empty() );

    std::shared_ptr<mp_transport> server_side = listener->accept( 1000 );
    REQUIRE( server_side );

    client->send( { 7, 7, 7 } );
    const std::optional<std::vector<uint8_t>> got = server_side->recv( 1000 );
    REQUIRE( got.has_value() );
    CHECK( *got == std::vector<uint8_t> { 7, 7, 7 } );

    server_side->send( { 1 } );
    CHECK( client->recv( 1000 ) == std::vector<uint8_t> { 1 } );
}

TEST_CASE( "tcp_transport_reassembles_a_large_message", "[mp]" )
{
    // Two hundred kilobytes is the order of a full redraw of a large terminal;
    // it will not fit in one socket write, so this exercises partial IO on
    // both ends.
    std::string err;
    std::optional<mp_listener> listener = mp_listener::bind( 0, err );
    REQUIRE( listener.has_value() );
    std::shared_ptr<mp_transport> client = mp_tcp_connect( "127.0.0.1", listener->port(), err );
    REQUIRE( client );
    std::shared_ptr<mp_transport> server_side = listener->accept( 1000 );
    REQUIRE( server_side );

    std::vector<uint8_t> big( 200000 );
    for( size_t i = 0; i < big.size(); ++i ) {
        big[i] = static_cast<uint8_t>( i % 251 );
    }
    client->send( big );

    const std::optional<std::vector<uint8_t>> got = server_side->recv( 5000 );
    REQUIRE( got.has_value() );
    CHECK( *got == big );
}

TEST_CASE( "tcp_transport_preserves_message_order_under_one_read", "[mp]" )
{
    std::string err;
    std::optional<mp_listener> listener = mp_listener::bind( 0, err );
    REQUIRE( listener.has_value() );
    std::shared_ptr<mp_transport> client = mp_tcp_connect( "127.0.0.1", listener->port(), err );
    REQUIRE( client );
    std::shared_ptr<mp_transport> server_side = listener->accept( 1000 );
    REQUIRE( server_side );

    client->send( { 1 } );
    client->send( { 2 } );
    client->send( { 3 } );

    CHECK( server_side->recv( 1000 ) == std::vector<uint8_t> { 1 } );
    CHECK( server_side->recv( 1000 ) == std::vector<uint8_t> { 2 } );
    CHECK( server_side->recv( 1000 ) == std::vector<uint8_t> { 3 } );
}

TEST_CASE( "tcp_transport_reports_a_closed_peer", "[mp]" )
{
    std::string err;
    std::optional<mp_listener> listener = mp_listener::bind( 0, err );
    REQUIRE( listener.has_value() );
    std::shared_ptr<mp_transport> client = mp_tcp_connect( "127.0.0.1", listener->port(), err );
    REQUIRE( client );
    std::shared_ptr<mp_transport> server_side = listener->accept( 1000 );
    REQUIRE( server_side );

    client->close();
    server_side->recv( 500 );
    CHECK_FALSE( server_side->connected() );
}

TEST_CASE( "tcp_transport_unblocks_on_peer_close_with_negative_timeout", "[mp]" )
{
    // recv(-1) blocks until a message or the peer goes away; a dropped peer
    // must unblock with nullopt rather than hanging the server forever.
    std::string err;
    std::optional<mp_listener> listener = mp_listener::bind( 0, err );
    REQUIRE( listener.has_value() );
    std::shared_ptr<mp_transport> client = mp_tcp_connect( "127.0.0.1", listener->port(), err );
    REQUIRE( client );
    std::shared_ptr<mp_transport> server_side = listener->accept( 1000 );
    REQUIRE( server_side );

    client->close();
    CHECK_FALSE( server_side->recv( -1 ).has_value() );
    CHECK_FALSE( server_side->connected() );
}

TEST_CASE( "tcp_connect_to_a_dead_port_fails_with_a_message", "[mp]" )
{
    std::string err;
    // Port 1 is privileged and nothing will be listening on it.
    std::shared_ptr<mp_transport> client = mp_tcp_connect( "127.0.0.1", 1, err );
    CHECK_FALSE( client );
    CHECK_FALSE( err.empty() );
}

TEST_CASE( "accept_times_out_when_nobody_connects", "[mp]" )
{
    std::string err;
    std::optional<mp_listener> listener = mp_listener::bind( 0, err );
    REQUIRE( listener.has_value() );
    CHECK_FALSE( listener->accept( 50 ) );
}
#endif // MULTIPLAYER
