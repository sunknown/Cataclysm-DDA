#if defined(MP_SERVER)

#include "mp/mp_server_input.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "debug.h"
#include "game_constants.h"
#include "mp/mp_context.h"
#include "mp/mp_describe.h"
#include "mp/mp_protocol.h"
#include "mp/mp_server_log.h"
#include "mp/mp_server_screen.h"
#include "mp/mp_session.h"
#include "mp/mp_session_ui.h"
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

// How long the describe feed still owes its pending request before the server
// may answer it. Only the session's own fields are read: the input wait runs
// without the baton, when the game's state belongs to whoever holds it.
std::chrono::milliseconds describe_time_left( const mp_session &s )
{
    const clock::time_point due = s.describe_last +
                                  std::chrono::milliseconds( MP_DESCRIBE_MIN_INTERVAL_MS );
    const clock::time_point now = clock::now();
    if( now >= due ) {
        return std::chrono::milliseconds( 0 );
    }
    // Round up, so a fraction of a millisecond never becomes a zero-length
    // recv that spins until the due instant.
    return std::chrono::ceil<std::chrono::milliseconds>( due - now );
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
    // The same set on acceptance and refusal: every answer names what this
    // server can serve, so a client needs no second query.
    ack.hello_ack.caps = MP_SERVER_CAPS;
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
                // Written under inbox_mutex: the seat's input wait reads it in
                // its predicate, and the world thread is the one dispatching.
                // The lock is released before this function returns, and every
                // caller notifies the seat afterwards.
                std::lock_guard<std::mutex> lock( s->inbox_mutex );
                s->pending_resize = msg.resize;
            }
            break;
        case mp_control_kind::describe:
            // A hover asks about one cell of the current view: only the newest
            // question is worth answering, so this overwrites instead of
            // queueing and a fast pointer cannot grow the session. The cell is
            // not checked here -- a cell the flush cannot describe is answered
            // with empty lines, so the client always gets its seq back. The
            // answer itself is built in the flush, which is the only place the
            // baton and the game state are available.
            {
                mp_describe_request req;
                req.seq = msg.describe.seq;
                req.x = msg.describe.x;
                req.y = msg.describe.y;
                req.w = msg.describe.w;
                // Same mutex as the seat's input wait: describe_pending is part
                // of its predicate, and this write comes from whichever thread
                // dispatched the message (the seat itself or the world).
                std::lock_guard<std::mutex> lock( s->inbox_mutex );
                if( mp_session_can_describe( *s ) ) {
                    s->describe_pending = req;
                }
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

// --- Per-seat readers ------------------------------------------------------

// The world's doorbell. Every enqueue and every reader exit bumps the
// generation; a wait call returns the moment the generation differs from the
// one it last observed, so a wake-up is delivered exactly once per event and a
// seat that stays dead cannot make the world spin.
std::mutex g_events_mutex;
std::condition_variable g_events_cv;
uint64_t g_events_generation = 0;
uint64_t g_events_seen = 0;

void signal_events()
{
    {
        std::lock_guard<std::mutex> lock( g_events_mutex );
        ++g_events_generation;
    }
    g_events_cv.notify_all();
}

// The four kinds the round loop owns. Everything else on the control queue is
// input plumbing: describe, resize, bye, and the handshake messages.
bool is_round_control( const mp_control_kind kind )
{
    switch( kind ) {
        case mp_control_kind::plan:
        case mp_control_kind::plan_cancel:
        case mp_control_kind::plan_ready:
        case mp_control_kind::plan_pass:
            return true;
        default:
            return false;
    }
}

// The seat's reader: the only consumer of its transport. It parses frames and
// routes them -- keystrokes to input_inbox, every other control message to
// control_inbox -- and does nothing else. It must not call dispatch or read
// game state: it runs while another player holds the baton and the game belongs
// to that player.
//
// The 50 ms recv slice is what makes reader_stop answerable without a wake-up
// frame on the wire.
void reader_main( mp_session *const s )
{
    // The wire this reader was started for. A replacement client installs a new
    // transport on the same session and gets a fresh reader; this one must not
    // follow the session's transport pointer to it. Read under the mutex that
    // the install path holds while it swaps the wire in.
    std::shared_ptr<mp_transport> t;
    {
        std::lock_guard<std::mutex> lock( s->inbox_mutex );
        t = s->transport;
    }
    while( !s->reader_stop.load() ) {
        if( !t ) {
            break;
        }
        const std::optional<std::vector<uint8_t>> payload = t->recv( 50 );
        if( !payload ) {
            if( !t->connected() ) {
                // The wire went away: the world has to learn about it, and it
                // is the world that marks the seat, not this thread.
                break;
            }
            continue;
        }
        mp_msg_kind kind = mp_msg_kind::control;
        std::vector<uint8_t> body;
        if( !mp_unwrap( *payload, kind, body ) || kind != mp_msg_kind::control ) {
            continue;
        }
        mp_control msg;
        if( !mp_control_from_json( std::string( body.begin(), body.end() ), msg ) ) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lock( s->inbox_mutex );
            if( msg.kind == mp_control_kind::input ) {
                s->input_inbox.push_back( msg.event );
            } else {
                s->control_inbox.push_back( std::move( msg ) );
            }
        }
        s->inbox_cv.notify_all();
        signal_events();
    }
    // A reader that exits on its own -- a dead wire, or a stop -- must drop
    // reader_running under the inbox mutex before waking anybody. The seat's
    // input wait is parked on a predicate that includes this flag, and with the
    // production's blocking (-1) timeout a stale `true` would keep it asleep
    // forever when the client quits while its player is waiting for input.
    {
        std::lock_guard<std::mutex> lock( s->inbox_mutex );
        s->reader_running = false;
    }
    // Wake both audiences: the seat may be parked in its input wait and the
    // world may be parked in mp_server_wait_for_events(). A deliberate stop is
    // the teardown's own business and is not an event the world must handle.
    s->inbox_cv.notify_all();
    if( !s->reader_stop.load() ) {
        signal_events();
    }
}

bool session_reader_running( mp_session &s )
{
    std::lock_guard<std::mutex> lock( s.inbox_mutex );
    return s.reader_running;
}

std::shared_ptr<mp_transport> session_transport( mp_session &s )
{
    std::lock_guard<std::mutex> lock( s.inbox_mutex );
    return s.transport;
}

// Whether the control queue holds a message the seat handles itself. Round
// controls are deliberately excluded: a queued plan stays until the world
// takes it, so counting it as an arrival would make the input wait return at
// once on every pass and burn the seat's thread until the plan was drained.
// Caller holds inbox_mutex.
bool has_local_control_locked( const mp_session &s )
{
    for( const mp_control &msg : s.control_inbox ) {
        if( !is_round_control( msg.kind ) ) {
            return true;
        }
    }
    return false;
}

bool resize_pending_of( mp_session &s )
{
    std::lock_guard<std::mutex> lock( s.inbox_mutex );
    return s.pending_resize.has_value();
}

// Whether the pending hover may be answered now. Caller holds inbox_mutex: the
// input wait's predicate runs under the same lock, so it cannot call the public
// wrapper below (which would lock again).
bool describe_pending_due_locked( const mp_session &s )
{
    return s.describe_pending && describe_time_left( s ).count() == 0;
}

bool describe_due_of( mp_session &s )
{
    std::lock_guard<std::mutex> lock( s.inbox_mutex );
    return describe_pending_due_locked( s );
}

// How long the input wait may still sleep: the caller's budget, shortened by a
// hover answer that falls due earlier. A negative value means "no limit".
// Caller holds inbox_mutex.
int wait_remaining_locked( const mp_session &s, const int timeout_ms,
                           const clock::time_point deadline )
{
    int remaining = -1;
    if( timeout_ms >= 0 ) {
        remaining = static_cast<int>( std::chrono::duration_cast<std::chrono::milliseconds>(
                                          deadline - clock::now() ).count() );
        if( remaining < 0 ) {
            remaining = 0;
        }
    }
    // The hover feed's own deadline: a request that is due must be built by the
    // caller's next flush, so the wait may not sleep past the interval even when
    // the caller asked for longer. It only ever shortens the caller's budget,
    // never extends it -- a seat that lost its clock must not turn a brief wait
    // into a long one.
    if( s.describe_pending ) {
        const int until_due = static_cast<int>( describe_time_left( s ).count() );
        if( remaining < 0 || until_due < remaining ) {
            remaining = until_due;
        }
    }
    return remaining;
}

int wait_remaining( mp_session &s, const int timeout_ms, const clock::time_point deadline )
{
    std::lock_guard<std::mutex> lock( s.inbox_mutex );
    return wait_remaining_locked( s, timeout_ms, deadline );
}

// Moves everything the reader collected into the player's own queue, which is
// what the rest of the game reads input from. A null context has nowhere to put
// them, so they stay queued for the next call rather than being dropped.
void drain_input_inbox( mp_session &s, player_context *const ctx )
{
    if( ctx == nullptr ) {
        return;
    }
    std::lock_guard<std::mutex> lock( s.inbox_mutex );
    while( !s.input_inbox.empty() ) {
        ctx->input_queue.push_back( std::move( s.input_inbox.front() ) );
        s.input_inbox.pop_front();
    }
}

// Handles the control messages that are not the round loop's: a resize, a hover
// request, a bye. They only write this session -- never the game -- so a seat
// can handle them without the baton. Plan kinds are left in the queue for
// mp_server_take_control().
bool handle_local_controls( mp_session &s )
{
    std::deque<mp_control> handled;
    {
        std::lock_guard<std::mutex> lock( s.inbox_mutex );
        std::deque<mp_control> &q = s.control_inbox;
        for( auto it = q.begin(); it != q.end(); ) {
            if( is_round_control( it->kind ) ) {
                ++it;
                continue;
            }
            handled.push_back( std::move( *it ) );
            it = q.erase( it );
        }
    }
    // Outside the lock: dispatch closes a transport on `bye`, and the reader
    // must not be kept out of the mutex while that happens.
    for( const mp_control &msg : handled ) {
        dispatch( msg, &s, nullptr );
    }
    return !handled.empty();
}

} // namespace

bool mp_server_handshake( mp_transport &transport, mp_session &session,
                          std::string &reject_reason, const int timeout_ms )
{
    reject_reason.clear();

    // The client's first message is the hello; a peer that says nothing within
    // the budget is refused rather than waited on forever. The verdict is sent
    // in every case, so the client always learns its fate.
    const std::optional<std::vector<uint8_t>> msg = transport.recv( timeout_ms );
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
    session.card_world = hello.hello.world;
    // Capabilities are only honoured after every version and size check has
    // passed, so a refused client leaves the session untouched.
    session.caps = hello.hello.caps;
    session.scene_z_levels = hello.hello.z_levels;
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
        {
            std::lock_guard<std::mutex> lock( s->inbox_mutex );
            s->pending_resize.reset();
        }
        // The hover feed's per-seat state dies with the test's world, exactly
        // as leaving the UI clears it: a stale request or interval must not
        // leak into the next case.
        mp_server_describe_clear( *s );
    }
}

void mp_server_start_reader( mp_session &s, std::shared_ptr<mp_transport> transport )
{
    std::lock_guard<std::mutex> lock( s.inbox_mutex );
    // A replacement wire is adopted and its reader claimed under this one lock.
    // The seat's input wait checks reader_running to decide whether it may read
    // the wire itself, so publishing a live fresh transport while that flag was
    // still false would let the seat read it -- and mp_transport::recv is not
    // reentrant, so the reader this function is about to start would corrupt
    // the frame stream.
    if( transport ) {
        s.transport = std::move( transport );
    }
    if( s.reader_running || s.reader.joinable() || !s.transport ) {
        return;
    }
    s.reader_stop.store( false );
    s.reader_running = true;
    try {
        s.reader = std::thread( reader_main, &s );
    } catch( ... ) {
        // Do not leave the seat believing a reader owns its wire when none does.
        s.reader_running = false;
        throw;
    }
}

void mp_server_stop_reader( mp_session &s )
{
    {
        std::lock_guard<std::mutex> lock( s.inbox_mutex );
        s.reader_stop.store( true );
    }
    if( s.reader.joinable() ) {
        s.reader.join();
    }
    {
        std::lock_guard<std::mutex> lock( s.inbox_mutex );
        s.reader_running = false;
        // A stopped reader has no wire worth delivering: whatever it queued
        // belonged to the transport this stop is about to leave behind, and the
        // next client must not receive the last one's keys or plans.
        s.input_inbox.clear();
        s.control_inbox.clear();
    }
    // A seat parked in its input wait may have seen reader_running true and
    // gone back to sleep; the stop has to reach it too.
    s.inbox_cv.notify_all();
}

void mp_server_start_readers()
{
    for( const std::unique_ptr<mp_session> &session : mp_sessions() ) {
        if( session ) {
            mp_server_start_reader( *session );
        }
    }
    // A session start is a new slate for the world's wake-ups: nothing a
    // previous session's readers signalled is this session's business.
    std::lock_guard<std::mutex> lock( g_events_mutex );
    g_events_seen = g_events_generation;
}

void mp_server_stop_readers()
{
    // Request every stop first: each reader honours it within its 50 ms slice,
    // so the joins below overlap instead of costing a slice per seat.
    for( const std::unique_ptr<mp_session> &session : mp_sessions() ) {
        if( session ) {
            std::lock_guard<std::mutex> lock( session->inbox_mutex );
            session->reader_stop.store( true );
        }
    }
    for( const std::unique_ptr<mp_session> &session : mp_sessions() ) {
        if( session ) {
            mp_server_stop_reader( *session );
        }
    }
}

bool mp_server_wait_for_events( const int timeout_ms )
{
    std::unique_lock<std::mutex> lock( g_events_mutex );
    const auto ready = []() {
        return g_events_generation != g_events_seen;
    };
    bool woken = true;
    if( !ready() ) {
        if( timeout_ms < 0 ) {
            g_events_cv.wait( lock, ready );
        } else {
            woken = g_events_cv.wait_for( lock, std::chrono::milliseconds( timeout_ms ), ready );
        }
    }
    // Recorded whether or not anything happened: the next call waits for the
    // next event, not for this one to be handled again.
    g_events_seen = g_events_generation;
    return woken;
}

bool mp_server_take_control( size_t &slot, mp_control &out )
{
    for( const std::unique_ptr<mp_session> &holder : mp_sessions() ) {
        mp_session *const s = holder.get();
        if( s == nullptr ) {
            continue;
        }
        std::deque<mp_control> handled;
        bool took = false;
        {
            std::lock_guard<std::mutex> lock( s->inbox_mutex );
            std::deque<mp_control> &q = s->control_inbox;
            // Everything that is not the round loop's is consumed here, so the
            // world never sees a resize, a hover or a goodbye.
            for( auto it = q.begin(); it != q.end(); ) {
                if( is_round_control( it->kind ) ) {
                    ++it;
                    continue;
                }
                handled.push_back( std::move( *it ) );
                it = q.erase( it );
            }
            if( !q.empty() ) {
                out = std::move( q.front() );
                q.pop_front();
                slot = s->context_index;
                took = true;
            }
        }
        for( const mp_control &msg : handled ) {
            dispatch( msg, s, nullptr );
        }
        if( took || !handled.empty() ) {
            // A resize or a hover changed this seat's wait state; it may be
            // parked with a long timeout and has to re-evaluate now.
            s->inbox_cv.notify_all();
        }
        if( took ) {
            return true;
        }
    }
    return false;
}

void mp_server_set_input_in_hand( const size_t slot, const bool in_hand )
{
    mp_session *const s = mp_session_of_slot( slot );
    if( s == nullptr ) {
        return;
    }
    std::lock_guard<std::mutex> lock( s->inbox_mutex );
    s->input_in_hand = in_hand;
}

bool mp_server_seat_has_input( const size_t slot )
{
    mp_session *const s = mp_session_of_slot( slot );
    if( s == nullptr ) {
        return false;
    }
    std::lock_guard<std::mutex> lock( s->inbox_mutex );
    // The inbox half is the key that arrived while the seat was parked on the
    // baton; the in-hand half is the key that seat already took out of it and
    // is parked on the baton for. Both mean the world owes the seat a grant.
    return !s->input_inbox.empty() || s->input_in_hand;
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
    if( s == nullptr ) {
        return take_queued( ctx, buffered ) ? buffered : make_timeout_event();
    }

    // The reader has been filling this seat's inbox since the last call. What
    // it collected is this player's input, and it belongs where the rest of the
    // game reads input from.
    drain_input_inbox( *s, ctx );
    if( take_queued( ctx, buffered ) ) {
        return buffered;
    }
    // A resize, a hover or a goodbye is this seat's own business -- none of
    // them touches the game -- so it is handled here rather than waiting for
    // the world: a busy world must not make a resize unresponsive.
    handle_local_controls( *s );
    if( resize_pending_of( *s ) || describe_due_of( *s ) ) {
        return make_timeout_event();
    }

    const std::shared_ptr<mp_transport> t = session_transport( *s );
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

        if( !session_reader_running( *s ) ) {
            // No reader owns this wire -- the input module's own tests, and any
            // single-seat path that built a session directly -- so this call is
            // the only consumer and reads it as it always did. With a reader,
            // parsing here would race it for the frame.
            const int remaining = wait_remaining( *s, timeout_ms, deadline );
            const std::optional<std::vector<uint8_t>> msg = t->recv( remaining );
            if( !msg ) {
                // Either the wait expired or the transport went away; both read
                // as "nothing arrived", and the module cannot tell them apart.
                return make_timeout_event();
            }
            dispatch_frame( *msg, s, ctx );
        } else {
            // The predicate is what makes this wait impossible to miss: it is
            // evaluated under inbox_mutex, the same lock every enqueue and every
            // wait-state write takes, so an event that landed between the drain
            // above and this lock is still seen. A bare wait() would sleep past
            // it -- and with the negative (blocking) timeout the seat would then
            // stay asleep until some other frame arrived, swallowing the
            // keystroke entirely.
            //
            // Only a message the seat can actually consume counts as an arrival.
            // The round controls (plan*) are deliberately left for the world, so
            // counting them here would make every pass return at once and spin
            // this thread until the world drained the plan.
            const auto arrival = [s, &t]() {
                return !s->input_inbox.empty() || has_local_control_locked( *s ) ||
                       s->pending_resize.has_value() ||
                       describe_pending_due_locked( *s ) ||
                       !t->connected() || !s->reader_running;
            };
            // The remaining time is computed under the same lock the wait
            // releases. Computing it before taking the lock leaves a window in
            // which a hover lands: its predicate is false until the due instant,
            // and with a -1 wait there is no deadline to fall back on.
            //
            // A predicate wait is not enough on its own: nothing signals the
            // cv when the describe interval merely elapses, so a seat parked
            // with the production's blocking (-1) timeout would sleep until the
            // next key, plan or resize. Sleep in bounded slices instead --
            // `wait_remaining_locked` returns the earlier of the caller's
            // deadline and the hover's due instant -- and recompute the slice
            // after every spurious wake. A slice of 0 is the caller's deadline
            // (the due instant itself is an arrival, so it never reaches 0),
            // which keeps the existing timeout return for finite budgets.
            std::unique_lock<std::mutex> lock( s->inbox_mutex );
            while( !arrival() ) {
                const int slice = wait_remaining_locked( *s, timeout_ms, deadline );
                if( slice == 0 ) {
                    return make_timeout_event();
                }
                if( slice < 0 ) {
                    s->inbox_cv.wait( lock, arrival );
                } else {
                    s->inbox_cv.wait_for( lock, std::chrono::milliseconds( slice ),
                                          arrival );
                }
            }
        }

        // Whatever the wake was -- a queued event, a resize, a hover, a closed
        // wire or the deadline -- the next pass decides, and the checks at the
        // top are the same ones the loop has always made. Input wins, so a
        // keystroke that arrived with a resize is never dropped.
        drain_input_inbox( *s, ctx );
        if( take_queued( ctx, buffered ) ) {
            return buffered;
        }
        handle_local_controls( *s );
        if( resize_pending_of( *s ) ) {
            // A resize is not a keystroke, but it still has to end the wait:
            // the curses backend applies it on the way back in, redraws at the
            // new size and flushes that redraw. Blocking on here would leave
            // the client -- which has already blanked its terminal to the new
            // size -- staring at nothing until the player pressed a key.
            // Input wins the race above, so waking this way never drops a
            // keystroke; the request stays recorded for the backend to take.
            return make_timeout_event();
        }
        if( describe_due_of( *s ) ) {
            // The hover answer is due and the caller's flush builds it under
            // the baton. Waking rather than answering keeps game state out of
            // this path; input and a resize above still win this wake, so a
            // keystroke that arrived with the request is never delayed.
            return make_timeout_event();
        }
    }
}

void mp_server_pump()
{
    mp_session *const s = mp_active_session();
    if( !s ) {
        return;
    }
    player_context *const ctx = mp_active_context_ptr();
    // Everything the wire delivered is already on the seat's queue; pumping
    // only moves it into this player's input queue.
    drain_input_inbox( *s, ctx );
    if( session_reader_running( *s ) ) {
        // The reader owns the socket. A second consumer here would race it for
        // a frame, and half a frame is worse than a later one.
        return;
    }
    const std::shared_ptr<mp_transport> t = session_transport( *s );
    if( !t || !t->connected() ) {
        return;
    }
    while( const std::optional<std::vector<uint8_t>> msg = t->recv( 0 ) ) {
        dispatch_frame( *msg, s, ctx );
    }
}

bool mp_server_take_resize_request( int &width, int &height )
{
    mp_session *const s = mp_active_session();
    if( !s ) {
        return false;
    }
    std::lock_guard<std::mutex> lock( s->inbox_mutex );
    if( !s->pending_resize ) {
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
    return resize_pending_of( *s );
}

void mp_server_describe_clear( mp_session &session )
{
    std::lock_guard<std::mutex> lock( session.inbox_mutex );
    session.describe_pending.reset();
    session.describe_last = {};
}

void mp_server_describe_drop_pending( mp_session &session )
{
    std::lock_guard<std::mutex> lock( session.inbox_mutex );
    session.describe_pending.reset();
}

bool mp_server_describe_pending_due( mp_session &session )
{
    std::lock_guard<std::mutex> lock( session.inbox_mutex );
    return describe_pending_due_locked( session );
}

bool mp_server_describe_take_due( mp_session &session, mp_describe_request &out )
{
    std::lock_guard<std::mutex> lock( session.inbox_mutex );
    if( !describe_pending_due_locked( session ) ) {
        return false;
    }
    out = *session.describe_pending;
    session.describe_pending.reset();
    // The flush used to stamp this before building the answer; a lock-held
    // clock read here is the same instant within a fraction of a millisecond
    // and keeps the timestamp and the take atomic.
    session.describe_last = clock::now();
    return true;
}

#endif // MP_SERVER
