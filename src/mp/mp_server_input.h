#pragma once
#ifndef CATA_SRC_MP_MP_SERVER_INPUT_H
#define CATA_SRC_MP_MP_SERVER_INPUT_H

#include <cstddef>
#include <memory>
#include <string>

#include "input_enums.h"

class mp_transport;
struct mp_control;
struct mp_describe_request;
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

// Per-seat bounds on the queues the reader fills, in the same spirit as
// MP_SCREEN_BACKLOG_BYTES and MP_MAX_FRAME_BYTES: the server's memory must be
// bounded by its own constants, not by what a peer chooses to send. A seat's
// wire is closed, with a logged reason, the moment an enqueue would cross one
// of these, so a client that stops being read cannot make the process grow.
// Both limits are far above what playing produces: a keystroke is a few dozen
// bytes and a plan a few hundred, and the seat drains its input queue on every
// baton grant.
//
// The input queue holds only parsed key events, whose size does not depend on
// the frame that carried them, so a count is the honest measure. A queued
// control keeps its parsed strings (a plan's label, a bye reason), so its cap
// is bytes of wire payload; one maximum-size frame fits.
constexpr size_t MP_INPUT_BACKLOG_EVENTS = 2048;
constexpr size_t MP_CONTROL_BACKLOG_BYTES = 8u * 1024u * 1024u;

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

// --- Round UI state --------------------------------------------------------
//
// The party snapshot's `menu` field is the only thing that tells a client its
// own seat is in a menu, and the party body used to be sent only at round
// boundaries. A seat that opens or closes a menu would then keep forwarding
// raw keys (or keep planning them) from a stale flag until somebody else acted.
// The seat's own input wait reports the change before it parks; the world wakes,
// broadcasts a party body, and that body's `menu` values become the baseline.

// Records the seat's UI depth as `in_menu` (the same depth test party_seats()
// uses) if it differs from the last party body; signals the world when it does.
// Called by the seat's input path while it holds the baton, and only for a
// seat that is in game.
void mp_server_note_menu_state( mp_session &session, bool in_menu );

// True when any seat reported a menu-state change since the last call. The
// caller broadcasts the party, which records the new baseline. The marks are
// cleared either way, so one change is reported once.
bool mp_server_take_menu_change();

// --- Hover describe state --------------------------------------------------
//
// A hover request is written by whichever thread dispatches it (the seat's own
// input wait or the world's mp_server_take_control) and read by the flush that
// builds the answer and by the seat's input wait, which sleeps until the
// request falls due. These threads take turns, not the baton together, so the
// pending request and the answer timestamp are guarded by the session's
// inbox_mutex. These functions are the only entry points outside
// mp_server_input.cpp: the screen flush and the UI leave path use them rather
// than touching the fields themselves.

// Drops the pending hover and the interval. Leaving the UI calls this: the next
// entry starts with a clean feed, and a seat that is not looking at a screen is
// never served its text.
void mp_server_describe_clear( mp_session &session );

// Drops only the pending request, keeping the last-answer timestamp. The flush
// uses it when the seat is no longer entitled to an answer or the client is not
// draining its socket.
void mp_server_describe_drop_pending( mp_session &session );

// Whether a request is pending and its interval has elapsed. False when
// nothing is pending, so a caller can skip the rest of the answer path.
bool mp_server_describe_pending_due( mp_session &session );

// Takes the pending request when it is due, records the answer timestamp and
// returns true, with the request in `out`. The flush then builds the answer; a
// request that is not due stays pending for a later pass.
bool mp_server_describe_take_due( mp_session &session, mp_describe_request &out );

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

// Whether `slot` has an input event to deliver: a keystroke waiting in its
// inbox, or one it already took out of the inbox and is parked on the baton
// for. Cheap and non-blocking: it answers "could this seat act now", not "will
// it", and it is exactly the question the world's grant asks.
bool mp_server_seat_has_input( size_t slot );

// Records whether `slot` holds an input event it has taken from its inbox but
// not delivered yet: between the wait that consumed the event and the grant
// that hands it the baton. The seat's own input wait is the only caller -- it
// sets this before it parks and clears it once it holds the baton again -- and
// nowhere else may, because the flag means "this seat is parked, owing the
// world one event". Without it the world sees an empty inbox and a parked seat
// and never grants, leaving the key undelivered. No-op when the seat has no
// session.
void mp_server_set_input_in_hand( size_t slot, bool in_hand );

// Whether `slot` has local control work the world owes it the baton for even
// though it holds no key: a resize waiting to be applied, or a hover request
// that has fallen due. Both are this seat's own business (the seat's input wait
// consumes them), but a seat parked on the baton never reaches that wait, so
// the world must grant it. A hover that is pending but not yet due does not
// count -- the world's next poll covers the due instant.
bool mp_server_seat_needs_local_service( size_t slot );

#endif // CATA_SRC_MP_MP_SERVER_INPUT_H
