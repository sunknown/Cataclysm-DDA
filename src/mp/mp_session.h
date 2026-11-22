#pragma once
#ifndef CATA_SRC_MP_MP_SESSION_H
#define CATA_SRC_MP_MP_SESSION_H

#if defined(MP_SERVER)

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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
    // native log snapshot; zero means a text client that gets neither.
    uint32_t caps = 0;
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
