#include "cata_catch.h"

#if defined(MULTIPLAYER)
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "mp/mp_protocol.h"
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

TEST_CASE( "tcp_drop_pending_screen_messages_drops_screen_frames_without_corrupting_the_stream",
           "[mp]" )
{
    std::string err;
    std::optional<mp_listener> listener = mp_listener::bind( 0, err );
    REQUIRE( listener.has_value() );
    std::shared_ptr<mp_transport> client = mp_tcp_connect( "127.0.0.1", listener->port(), err );
    REQUIRE( client );
    std::shared_ptr<mp_transport> server_side = listener->accept( 1000 );
    REQUIRE( server_side );

    // The client never reads, so the server's outbound backs up -- the exact
    // slow-client scenario backpressure exists for. The first screen frame is
    // as large as the wire allows (8 MiB payload, 8 MiB + 4 bytes framed),
    // well past the kernel send buffer, so it is only partially handed to the
    // socket before the buffer fills: the outbound head ends up mid-frame. The
    // drop path must survive that head and never re-frame the stream.
    const std::vector<uint8_t> screen_body( 8 * 1024 * 1024 - 1, 's' );
    const auto screen = [&]() {
        return mp_wrap( mp_msg_kind::screen, screen_body );
    };
    const auto control = []() {
        return mp_wrap( mp_msg_kind::control, std::vector<uint8_t> { 'c' } );
    };

    server_side->send( screen() );   // head: partially sent, must survive
    server_side->send( control() );
    server_side->send( screen() );   // fully queued: must be dropped
    server_side->send( control() );
    server_side->send( screen() );   // fully queued: must be dropped

    REQUIRE( server_side->pending_bytes() > 0 );
    const size_t before = server_side->pending_bytes();

    server_side->drop_pending_screen_messages();

    // The fully-queued screen frames are gone; the partially-sent head and the
    // control frames remain, so the backlog shrinks but does not vanish.
    CHECK( server_side->pending_bytes() < before );

    // A final control frame gives the server a reason to flush the surviving
    // backlog to the client, which then drains it.
    server_side->send( control() );

    int screens = 0;
    int controls = 0;
    bool framing_ok = true;
    auto count = [&]( const std::vector<uint8_t> &msg ) {
        mp_msg_kind kind = mp_msg_kind::control;
        std::vector<uint8_t> body;
        if( !mp_unwrap( msg, kind, body ) ) {
            framing_ok = false;
            return;
        }
        if( kind == mp_msg_kind::screen ) {
            ++screens;
        } else {
            ++controls;
        }
    };

    // Alternate pushing the server's outbound (recv flushes first) and draining
    // the client until both sides are quiet. The client's receive buffer must
    // be read before the server's send buffer frees up, so the two alternate.
    for( int i = 0; i < 1000; ++i ) {
        server_side->recv( 0 );
        bool got = false;
        while( const std::optional<std::vector<uint8_t>> msg = client->recv( 0 ) ) {
            got = true;
            count( *msg );
        }
        if( server_side->pending_bytes() == 0 && !got ) {
            break;
        }
    }
    // A final positive-timeout read catches any frame that was still in flight
    // when the server reported an empty outbound.
    while( const std::optional<std::vector<uint8_t>> msg = client->recv( 100 ) ) {
        count( *msg );
    }

    CHECK( framing_ok );
    // The head screen frame survives (it was already partly on the wire); the
    // two fully-queued screen frames were dropped. All control frames arrived.
    CHECK( screens >= 1 );
    CHECK( screens < 3 );
    CHECK( controls == 3 );
    // A corrupted stream would trip the client's frame reader, which drops the
    // connection -- so both ends staying connected is the framing guarantee.
    CHECK( client->connected() );
    CHECK( server_side->connected() );
}
#endif // MULTIPLAYER
