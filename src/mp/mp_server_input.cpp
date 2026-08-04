#include "mp/mp_server_input.h"

#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "debug.h"
#include "game_constants.h"
#include "mp/mp_protocol.h"
#include "mp/mp_server_screen.h"
#include "mp/mp_transport.h"

// Defined here, called by the screen module's reset (mp_server_screen.cpp) so
// one reset drops both modules' state. Declared here so the definition below
// has a prior declaration in this translation unit.
void mp_server_input_reset_for_test();

namespace
{

// Keystrokes the game has not yet consumed, and the resize the client asked
// for but the curses backend has not yet acted on. Both are process-wide for
// the same reason the screen state is.
std::deque<input_event> g_input_queue;
std::optional<mp_resize> g_pending_resize;

using clock = std::chrono::steady_clock;

input_event make_timeout_event()
{
    input_event ev;
    ev.type = input_event_t::timeout;
    return ev;
}

// Sends a hello_ack control carrying the handshake verdict to the client. The
// reason is empty on acceptance; on refusal it repeats the same text the caller
// reports locally, so the client learns why it was turned away.
void send_hello_ack( mp_transport &transport, const bool accepted, const std::string &reason )
{
    mp_control ack;
    ack.kind = mp_control_kind::hello_ack;
    ack.hello_ack.accepted = accepted;
    ack.hello_ack.reason = reason;
    const std::string json = mp_control_to_json( ack );
    transport.send( mp_wrap( mp_msg_kind::control,
                             std::vector<uint8_t>( json.begin(), json.end() ) ) );
}

// Handles one parsed control message. Only the kinds the client actually sends
// the server do anything; hello, hello_ack and cursor travel client-ward and
// are ignored here.
void dispatch( const mp_control &msg )
{
    switch( msg.kind ) {
        case mp_control_kind::input:
            g_input_queue.push_back( msg.event );
            break;
        case mp_control_kind::resize:
            // A size above the cap would make the grid allocation abort the
            // server (see MP_MAX_TERMINAL_WIDTH/HEIGHT in mp_server_input.h).
            // Dropping the request is safe: a resize the server cannot honour
            // just leaves the client on its current terminal size, and a
            // well-behaved client can ask again with a sane value. This is the
            // peer-data entry point, so it is the first line of defence; the
            // curses backend re-checks the same bound in handle_resize() for
            // any caller that bypasses dispatch (e.g. resize_term).
            if( msg.resize.width <= MP_MAX_TERMINAL_WIDTH &&
                msg.resize.height <= MP_MAX_TERMINAL_HEIGHT ) {
                g_pending_resize = msg.resize;
            }
            break;
        case mp_control_kind::bye:
            // The peer is leaving. Closing the transport is what makes the
            // next recv return, so wait_for_input unblocks and reports a
            // timeout event rather than hanging on a dead wire.
            if( std::shared_ptr<mp_transport> t = mp_server_shared_transport() ) {
                t->close();
            }
            break;
        default:
            break;
    }
}

// Parses one raw framed payload. A malformed frame, a non-control kind, or a
// control message that does not parse is ignored rather than asserted on:
// nothing a peer sends should ever be able to crash the server loop.
void dispatch_frame( const std::vector<uint8_t> &payload )
{
    mp_msg_kind kind = mp_msg_kind::control;
    std::vector<uint8_t> body;
    if( !mp_unwrap( payload, kind, body ) ) {
        return;
    }
    if( kind != mp_msg_kind::control ) {
        return;
    }
    mp_control msg;
    if( !mp_control_from_json( std::string( body.begin(), body.end() ), msg ) ) {
        return;
    }
    dispatch( msg );
}

bool take_queued( input_event &out )
{
    if( g_input_queue.empty() ) {
        return false;
    }
    out = std::move( g_input_queue.front() );
    g_input_queue.pop_front();
    return true;
}

} // namespace

bool mp_server_handshake( mp_transport &transport, std::string &reject_reason )
{
    reject_reason.clear();

    // The client's first message is the hello; a peer that says nothing within
    // the budget is refused rather than waited on forever. The verdict is sent
    // in every case, so the client always learns its fate.
    const std::optional<std::vector<uint8_t>> msg = transport.recv( 5000 );
    if( !msg ) {
        reject_reason = "no hello received within the handshake timeout";
        send_hello_ack( transport, false, reject_reason );
        return false;
    }

    // Only a control frame can be a hello; anything else is refused. The parse
    // goes through mp_control_from_json only, which is the single entry point
    // for peer bytes (it already ignores unknown fields).
    mp_msg_kind kind = mp_msg_kind::control;
    std::vector<uint8_t> body;
    if( !mp_unwrap( *msg, kind, body ) || kind != mp_msg_kind::control ) {
        reject_reason = "the first message was not a control message";
        send_hello_ack( transport, false, reject_reason );
        return false;
    }
    mp_control hello;
    if( !mp_control_from_json( std::string( body.begin(), body.end() ), hello ) ) {
        reject_reason = "the first message was not a parseable control message";
        send_hello_ack( transport, false, reject_reason );
        return false;
    }
    if( hello.kind != mp_control_kind::hello ) {
        reject_reason = "expected a hello message, got " +
                        std::to_string( static_cast<int>( hello.kind ) );
        send_hello_ack( transport, false, reject_reason );
        return false;
    }
    if( hello.hello.proto != MP_PROTOCOL_VERSION ) {
        reject_reason = "protocol version mismatch: client speaks " +
                        std::to_string( hello.hello.proto ) + ", server speaks " +
                        std::to_string( MP_PROTOCOL_VERSION );
        send_hello_ack( transport, false, reject_reason );
        return false;
    }
    if( hello.hello.width < EVEN_MINIMUM_TERM_WIDTH ||
        hello.hello.height < EVEN_MINIMUM_TERM_HEIGHT ) {
        reject_reason = "terminal is smaller than the game minimum (" +
                        std::to_string( EVEN_MINIMUM_TERM_WIDTH ) + "x" +
                        std::to_string( EVEN_MINIMUM_TERM_HEIGHT ) + ")";
        send_hello_ack( transport, false, reject_reason );
        return false;
    }
    if( hello.hello.width > MP_MAX_TERMINAL_WIDTH ||
        hello.hello.height > MP_MAX_TERMINAL_HEIGHT ) {
        reject_reason = "terminal is larger than the server's maximum (" +
                        std::to_string( MP_MAX_TERMINAL_WIDTH ) + "x" +
                        std::to_string( MP_MAX_TERMINAL_HEIGHT ) + "), requested " +
                        std::to_string( hello.hello.width ) + "x" +
                        std::to_string( hello.hello.height );
        // mp_server_main logs the rejection reason too; this line spells out
        // the requested size for an operator trying to diagnose a weird client.
        DebugLog( D_WARNING, D_MAIN ) << "mp server: refusing a hello with an "
                                      << "oversized terminal " << hello.hello.width << "x"
                                      << hello.hello.height << " (cap " << MP_MAX_TERMINAL_WIDTH
                                      << "x" << MP_MAX_TERMINAL_HEIGHT << ")";
        send_hello_ack( transport, false, reject_reason );
        return false;
    }

    // Accepted: size the screen to what the client can show, then let them in.
    mp_server_set_terminal_size( hello.hello.width, hello.hello.height );
    send_hello_ack( transport, true, std::string() );
    return true;
}

void mp_server_input_reset_for_test()
{
    g_input_queue.clear();
    g_pending_resize.reset();
}

input_event mp_server_wait_for_input( const int timeout_ms )
{
    // This flush is the one place the screen goes out: everything the game
    // drew since the last turn is packed into a single message here, before
    // the wait blocks.
    mp_server_flush_screen();

    input_event buffered;
    if( take_queued( buffered ) ) {
        return buffered;
    }

    const clock::time_point deadline = timeout_ms < 0
                                       ? clock::time_point::max()
                                       : clock::now() + std::chrono::milliseconds( timeout_ms );

    while( true ) {
        std::shared_ptr<mp_transport> t = mp_server_shared_transport();
        if( !t || !t->connected() ) {
            // No wire at all, or the peer closed it: there is nothing to wait
            // on, so the game sees a timeout event rather than a spin or a
            // crash.
            return make_timeout_event();
        }

        // A negative timeout means "wait as long as it takes"; recv() reads
        // that as -1. Otherwise pass the time left until the deadline, never a
        // negative number, which recv() would also read as "wait forever".
        int remaining = -1;
        if( timeout_ms >= 0 ) {
            remaining = static_cast<int>( std::chrono::duration_cast<std::chrono::milliseconds>(
                                              deadline - clock::now() ).count() );
            if( remaining < 0 ) {
                remaining = 0;
            }
        }
        const std::optional<std::vector<uint8_t>> msg = t->recv( remaining );
        if( !msg ) {
            // Either the wait expired or the transport went away; both read as
            // "nothing arrived", and the module cannot tell them apart.
            return make_timeout_event();
        }

        dispatch_frame( *msg );
        if( take_queued( buffered ) ) {
            return buffered;
        }
    }
}

void mp_server_pump()
{
    std::shared_ptr<mp_transport> t = mp_server_shared_transport();
    if( !t || !t->connected() ) {
        return;
    }
    while( const std::optional<std::vector<uint8_t>> msg = t->recv( 0 ) ) {
        dispatch_frame( *msg );
    }
}

bool mp_server_take_resize_request( int &width, int &height )
{
    if( !g_pending_resize ) {
        return false;
    }
    width = g_pending_resize->width;
    height = g_pending_resize->height;
    g_pending_resize.reset();
    return true;
}
