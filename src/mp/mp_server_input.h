#pragma once
#ifndef CATA_SRC_MP_MP_SERVER_INPUT_H
#define CATA_SRC_MP_MP_SERVER_INPUT_H

#include <cstddef>
#include <memory>
#include <string>

#include "input_enums.h"

class mp_transport;
struct mp_control;
struct mp_session;
struct player_context;

// The largest terminal a peer may declare, in either dimension. The server's
// screen grid multiplies width by height (mp_screen_grid::resize), so an
// unbounded peer value would make it try to allocate the product of two 31-bit
// ints -- roughly 2^62 cells -- and abort on std::bad_alloc before the
// exception can be caught. Real terminals live far below this cap: 1000x1000
// cells already exceeds every device the game runs on. The cap exists purely
// to bound what a hostile or buggy peer can make the server allocate; a client
// that genuinely needs more room is a non-existent client.
constexpr int MP_MAX_TERMINAL_WIDTH = 1000;
constexpr int MP_MAX_TERMINAL_HEIGHT = 1000;

// Performs the connection handshake on a freshly-accepted transport: reads the
// client's first message (a `hello` control), checks its protocol version and
// the terminal size it wants, and sends a `hello_ack` verdict either way.
//
// `timeout_ms` bounds how long a peer may stay silent before it is refused.
// The join phase can afford the protocol's generous budget; the turn path
// cannot, because a silent peer there would freeze every player for that long.
//
// Fills `session.name` and sizes `session.grid` from the accepted hello instead
// of writing process-wide state. The transport parameter stays separate from
// the session because the handshake runs before the session is worth keeping:
// a refused peer never becomes one.
//
// Returns true and sizes the screen grid when the client is accepted, in which
// case `reject_reason` is left empty. Returns false on any refusal -- a silent
// client, a message that is not a parseable `hello`, a protocol mismatch, or a
// terminal size outside the accepted range (too small to run the game, or too
// large for the server to allocate) -- with a human-readable reason in
// `reject_reason` that is also sent to the client in the ack.
bool mp_server_handshake( mp_transport &transport, mp_session &session,
                          std::string &reject_reason, int timeout_ms = 5000 );

// Blocks until the connected player presses something, a resize arrives, the
// timeout expires, or the connection drops. Flushes the pending screen first:
// this is the single point at which the player sees what the game drew.
//
// `timeout_ms` of a negative value blocks until one of those happens. Returns
// an event of type `timeout` when nothing arrived and when the wait ended on a
// resize -- the caller tells the two apart with mp_server_resize_pending().
input_event mp_server_wait_for_input( int timeout_ms );

// The same wait, minus the flush, for player `session` / `ctx` named explicitly.
// mp_server_wait_for_input() is this applied to the active session and context.
//
// This is the half that runs WITHOUT the baton. A player thread hands the baton
// on before it blocks on its own wire (the curses backend's get_input_event),
// and from that moment the world and the other players run: they repoint the
// active context, flip the world-phase flag and swap avatars. So this reads no
// process-wide state at all -- only `session` (its transport, pending resize
// and hover request) and `ctx` (its input queue), both of which belong to this
// player alone. A pending hover request also caps the wait to its due time and
// wakes when it falls due, so the caller's next flush can build the answer
// instead of sleeping past it; this path only wakes, it never builds the answer
// itself. The caller captures them, and flushes the screen, while it still
// holds the baton.
input_event mp_server_wait_for_input_of( mp_session *session, player_context *ctx,
        int timeout_ms );

// Consumes whatever has already arrived without blocking. Keystrokes seen here
// are buffered for the next mp_server_wait_for_input().
void mp_server_pump();

// Reports a resize the client asked for, once. The curses backend acts on it:
// it resizes the grid and rebuilds the game's windows.
bool mp_server_take_resize_request( int &width, int &height );

// Whether a resize is recorded and still waiting to be taken. Lets the curses
// backend tell "the wait ended because the client resized" from an ordinary
// timeout without consuming the request.
bool mp_server_resize_pending();

// --- Per-seat readers ------------------------------------------------------
//
// A session's reader is the only thread that reads its transport. It parses
// each frame and routes it: keystrokes to the session's input_inbox, every
// other control message to its control_inbox. It never calls the input
// dispatcher, never reads game state and never touches a player_context -- a
// plan or a resize arrives while another player holds the baton, and the game
// state belongs to that player.
//
// Start is idempotent: a session that already has a reader keeps it. Stop is
// idempotent too, and it is not optional -- a std::thread destroyed joinable
// terminates the process. Every path that erases a session, replaces its
// transport or resets the roster stops that session's reader first.

// Starts a reader for every session that has a transport and none yet. Called
// once the wires are installed and before the player threads run.
void mp_server_start_readers();
// Stops and joins every session's reader. Safe to call when none is running.
void mp_server_stop_readers();

// One session's reader, for the paths that act on a single seat: dropping it
// from the join phase, or installing a replacement client's wire on it. The
// single-session stop also discards whatever the previous wire queued -- those
// events belong to the client that left, not to the one taking the seat.
//
// The start takes an optional replacement wire: when one is supplied it is
// adopted and claimed for the new reader under the same lock, so no other
// thread can observe the session's new transport while reader_running still
// says "nobody owns it". Pass nothing to start a reader on the wire the session
// already has.
void mp_server_start_reader( mp_session &session,
                             std::shared_ptr<mp_transport> transport = nullptr );
void mp_server_stop_reader( mp_session &session );

// Parks until some seat has something for somebody: an event was queued, or a
// reader exited because its wire went away. `timeout_ms` is a negative value
// for "as long as it takes". Returns true when woken, false on timeout.
//
// Wake-ups are edge-triggered: the call returns when something happened since
// the previous call, so a seat that is permanently dead does not make the
// world spin -- one wake per reader exit is all a reader that has stopped can
// produce. A caller therefore must drain after every wake, calling
// mp_server_take_control() until it returns false, and must not assume one wake
// means one message: several can be queued before the wait returns.
bool mp_server_wait_for_events( int timeout_ms );

// Takes the oldest message for the round loop off any seat's control queue and
// reports which seat it belongs to. Only the four plan kinds come back; the
// describe, resize and bye messages that share the queue are consumed here,
// through the same handler the seat's input wait uses, so the round loop never
// sees them. Returns false when no seat has a plan.
//
// A seat whose wait state a consumed message changed (a resize, a new hover)
// is woken so it re-evaluates before its own timeout.
bool mp_server_take_control( size_t &slot, mp_control &out );

// Whether `slot` has a keystroke waiting. Cheap and non-blocking: it answers
// "could this seat act now", not "will it".
bool mp_server_seat_has_input( size_t slot );

#endif // CATA_SRC_MP_MP_SERVER_INPUT_H
