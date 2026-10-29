#if defined(MP_SERVER)

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
#include "mp/mp_context.h"
#include "mp/mp_protocol.h"
#include "mp/mp_server_log.h"
#include "mp/mp_server_screen.h"
#include "mp/mp_session.h"
#include "mp/mp_transport.h"

// Defined here, called by the screen module's reset (mp_server_screen.cpp) so
// one reset drops both modules' state. Declared here so the definition below
// has a prior declaration in this translation unit.
void mp_server_input_reset_for_test();

namespace
{

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

// Handles one parsed control message for the player `s` and `ctx` belong to.
// Only the kinds the client actually sends the server do anything; hello,
// hello_ack and cursor travel client-ward and are ignored here.
//
// Both are passed in rather than looked up, because the input wait calls this
// after its player has handed the baton on (see mp_server_wait_for_input_of):
// by then the active session and context are whoever runs next, not this player.
void dispatch( const mp_control &msg, mp_session *const s, player_context *const ctx )
{
    if( s == nullptr ) {
        // No client is addressed: the world phase never reads the wire, and a
        // test without a session has nowhere to put this.
        return;
    }
    switch( msg.kind ) {
        case mp_control_kind::input:
            // mp_ensure_player_context() runs at game startup, before the main
            // menu, so the queue is live from the start; the null check is
            // defensive for tests that run without a context.
            if( ctx != nullptr ) {
                ctx->input_queue.push_back( msg.event );
            }
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
                s->pending_resize = msg.resize;
            }
            break;
        case mp_control_kind::bye:
            // Closing this client's transport is what makes its own recv
            // return; the other clients' waits are untouched.
            if( s->transport ) {
                s->transport->close();
            }
            break;
        default:
            break;
    }
}

// Parses one raw framed payload. A malformed frame, a non-control kind, or a
// control message that does not parse is ignored rather than asserted on:
// nothing a peer sends should ever be able to crash the server loop.
void dispatch_frame( const std::vector<uint8_t> &payload, mp_session *const s,
                     player_context *const ctx )
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
    dispatch( msg, s, ctx );
}

bool take_queued( player_context *const ctx, input_event &out )
{
    // The context is ensured before the main menu, so the queue is live from
    // startup; the null check is defensive for tests without a context.
    if( ctx == nullptr ) {
        return false;
    }
    std::deque<input_event> &q = ctx->input_queue;
    if( q.empty() ) {
        return false;
    }
    out = std::move( q.front() );
    q.pop_front();
    return true;
}

} // namespace

bool mp_server_handshake( mp_transport &transport, mp_session &session,
                          std::string &reject_reason )
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

    // Accepted: size the session's grid to what the client can show, record
    // the name, then let them in.
    session.grid.resize( hello.hello.width, hello.hello.height );
    session.name = hello.hello.name;
    session.card_save_id = hello.hello.save_id;
    send_hello_ack( transport, true, std::string() );
    mp_server_log_mark_connected();
    mp_server_log( "client \"" + session.name + "\" connected from " +
                   transport.peer_description() + " (terminal " +
                   std::to_string( hello.hello.width ) + "x" +
                   std::to_string( hello.hello.height ) + ", " +
                   ( hello.hello.colors_256 ? "256 colours" : "16 colours" ) +
                   ", protocol " + std::to_string( hello.hello.proto ) + ")" );
    return true;
}

void mp_server_input_reset_for_test()
{
    // mp_active_context() asserts a non-null pointer, so only clear the queue
    // when a context exists; a future caller may not have set one up.
    if( player_context *ctx = mp_active_context_ptr() ) {
        ctx->input_queue.clear();
    }
    // Clear pending resize on the active session if there is one.
    if( mp_session *s = mp_active_session() ) {
        s->pending_resize.reset();
    }
}

input_event mp_server_wait_for_input( const int timeout_ms )
{
    // This flush is the one place the screen goes out: everything the game
    // drew since the last turn is packed into a single message here, before
    // the wait blocks.
    mp_server_flush_screen();
    return mp_server_wait_for_input_of( mp_active_session(), mp_active_context_ptr(),
                                        timeout_ms );
}

input_event mp_server_wait_for_input_of( mp_session *const s, player_context *const ctx,
        const int timeout_ms )
{
    // Nothing below may read the active session, the active context or any
    // other process-wide state: see the header. Everything goes through `s`
    // and `ctx`.
    input_event buffered;
    if( take_queued( ctx, buffered ) ) {
        return buffered;
    }

    std::shared_ptr<mp_transport> t;
    if( s ) {
        t = s->transport;
    }
    if( !t || !t->connected() ) {
        // No wire at all, or the peer closed it: there is nothing to wait
        // on, so the game sees a timeout event rather than a spin or a
        // crash.
        return make_timeout_event();
    }

    const clock::time_point deadline = timeout_ms < 0
                                       ? clock::time_point::max()
                                       : clock::now() + std::chrono::milliseconds( timeout_ms );

    while( true ) {
        if( !t->connected() ) {
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

        dispatch_frame( *msg, s, ctx );
        if( take_queued( ctx, buffered ) ) {
            return buffered;
        }
        if( s && s->pending_resize ) {
            // A resize is not a keystroke, but it still has to end the wait:
            // the curses backend applies it on the way back in, redraws at the
            // new size and flushes that redraw. Blocking on here would leave
            // the client -- which has already blanked its terminal to the new
            // size -- staring at nothing until the player pressed a key.
            // Input wins the race above, so waking this way never drops a
            // keystroke; the request stays recorded for the backend to take.
            return make_timeout_event();
        }
    }
}

void mp_server_pump()
{
    mp_session *const s = mp_active_session();
    if( !s || !s->transport || !s->transport->connected() ) {
        return;
    }
    player_context *const ctx = mp_active_context_ptr();
    while( const std::optional<std::vector<uint8_t>> msg = s->transport->recv( 0 ) ) {
        dispatch_frame( *msg, s, ctx );
    }
}

bool mp_server_take_resize_request( int &width, int &height )
{
    mp_session *const s = mp_active_session();
    if( !s || !s->pending_resize ) {
        return false;
    }
    width = s->pending_resize->width;
    height = s->pending_resize->height;
    s->pending_resize.reset();
    return true;
}

bool mp_server_resize_pending()
{
    mp_session *const s = mp_active_session();
    if( !s ) {
        return false;
    }
    return s->pending_resize.has_value();
}

#endif // MP_SERVER
