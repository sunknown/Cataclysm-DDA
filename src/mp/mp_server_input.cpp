#include "mp/mp_server_input.h"

#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
            g_pending_resize = msg.resize;
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
