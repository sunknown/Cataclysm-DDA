#pragma once
#ifndef CATA_SRC_MP_MP_SESSION_H
#define CATA_SRC_MP_MP_SESSION_H

#if defined(MP_SERVER)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "mp/mp_cell.h"
#include "mp/mp_protocol.h"

class mp_transport;

// Everything the server holds for one connected client. This used to be
// process-wide state in mp_server_screen.cpp and mp_server_input.cpp, which was
// only ever right because there was exactly one client.
struct mp_session {
    // The wire. Owned here; the session outlives every use of it.
    std::shared_ptr<mp_transport> transport;
    // What this client's screen looks like, with per-row dirty tracking.
    mp_screen_grid grid{ 80, 24 };
    int cursor_x = 0;
    int cursor_y = 0;
    bool cursor_visible = false;
    bool cursor_changed = false;
    // A resize this client asked for that the curses backend has not applied.
    std::optional<mp_resize> pending_resize;
    // The name from the hello: who this client is in the log and on the
    // waiting screen.
    std::string name;
    // The save id from the client's character card, empty when it has none.
    // Offered back as "continue as" in the join menu when the world has it AND
    // the card came from this world -- see card_world.
    std::string card_save_id;
    // The world the card was written in. A card is one per server AND world:
    // restarting the same address on a different world can offer a stranger's
    // character, so mp_card_save_id_in_world() is what the join menu consults.
    std::string card_world;
    // Index into game::contexts and game::player_slots. The two are parallel,
    // and the session is addressed through this index rather than holding a
    // player_context pointer: player_context ships in every build and must not
    // learn about networking types.
    size_t context_index = 0;
    // Capability bits from the hello. Bit0 is the 3D scene stream; bit1 is the
    // native log snapshot; bit2 is the hover describe feed; zero means a text
    // client that gets none.
    uint32_t caps = 0;
    // Whether this seat is looking at an in-game screen. Only mp_session_ui_*
    // writes it: a join, creation, or blank seat has no UI the log or a hover
    // answer could belong to.
    bool ui_active = false;
    // The newest hover describe request, or nothing. Latest wins; there is no
    // queue, so a fast pointer cannot grow the session. Cleared whenever this
    // seat leaves the UI.
    std::optional<mp_describe_request> describe_pending;
    // When the last describe answer was built, for the server-side interval.
    // The epoch default makes the first request due at once.
    std::chrono::steady_clock::time_point describe_last{};
    // The encoded z-levels this client asked for, already clamped to 1..10.
    uint8_t scene_z_levels = 10;
    // Sequence of the last scene frame actually sent; a skipped frame does not
    // advance it, so the client can tell a repeated full frame from a new one.
    uint32_t scene_seq = 0;
    // Body hash of the last sent frame, with `frame_seq` excluded, and the mask
    // hash at that moment. Together they are the change gate.
    uint64_t scene_hash = 0;
    uint64_t scene_mask_hash = 0;
    // False until a frame has been sent: the first flush must build even when
    // the mask hash happens to equal the zero-initialised one.
    bool scene_sent_once = false;
    // Set by the curses hook when this seat's terrain window blits; cleared on
    // a successful send. The only thing that forces a rebuild without a mask
    // change.
    bool scene_terrain_dirty = false;
    // The log snapshot stream: the sequence of the last snapshot actually sent,
    // its body hash with the sequence removed, and whether one was ever sent.
    // The first flush must send even when the hash is the zero-initialised one.
    uint32_t log_seq = 0;
    uint64_t log_hash = 0;
    bool log_sent_once = false;

    // --- The seat's reader and the two queues it feeds ---------------------
    //
    // Every frame this seat's wire delivers lands in one of these queues first.
    // The reader thread parses it and routes it by kind: a keystroke goes to
    // input_inbox for the seat's own input wait, and everything else -- a plan,
    // a describe, a resize, a bye, a hello -- goes to control_inbox for the
    // world loop (mp_server_take_control) and for the seat's input wait, which
    // handles the non-round ones itself.
    //
    // inbox_mutex guards both queues, reader_running and every read or write of
    // them; inbox_cv is signalled whenever something is enqueued and whenever
    // the reader exits, so a seat parked in mp_server_wait_for_input_of()
    // re-evaluates a resize, a due hover or a dead wire instead of sleeping to
    // its deadline.
    std::mutex inbox_mutex;
    std::condition_variable inbox_cv;
    std::deque<input_event> input_inbox;
    std::deque<mp_control> control_inbox;
    // Set by mp_server_stop_reader(). Atomic because the stop is a plain store
    // from another thread and the reader polls it without taking inbox_mutex.
    std::atomic<bool> reader_stop{ false };
    // True between the reader's creation and its join, guarded by inbox_mutex.
    // The input wait reads it to decide whether it may still read the transport
    // itself: it may only when no reader owns the socket.
    bool reader_running = false;
    // This seat's reader. It is never left joinable: ~mp_session stops it, and
    // every path that erases a session stops it first (see mp_server_input.h).
    std::thread reader;

    mp_session() = default;
    // Stops and joins the reader, so no session can be destroyed with a
    // joinable thread -- which would call std::terminate. Every explicit erase
    // path still stops the reader first; this is the backstop for the ones that
    // are easy to forget.
    ~mp_session();

    mp_session( const mp_session & ) = delete;
    mp_session &operator=( const mp_session & ) = delete;
};

// The sessions of the running server, indexed the same way as game::contexts.
std::vector<std::unique_ptr<mp_session>> &mp_sessions();

// The session seated in `slot`, or nullptr when that seat is empty: before a
// client has connected, during the world phase (no player is active), and after
// a client left the join phase until its replacement takes the seat. This is
// the only way to address a session by seat: the vector is in connection order,
// not seat order, once a replacement has arrived.
mp_session *mp_session_of_slot( size_t slot );

// The session of the player whose context is active, or nullptr during the
// world phase (when no player is active) and in tests without a session.
mp_session *mp_active_session();

// Drops every session. Tests only.
void mp_sessions_reset_for_test();

#endif // MP_SERVER

#endif // CATA_SRC_MP_MP_SESSION_H
