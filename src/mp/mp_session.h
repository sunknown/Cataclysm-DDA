#pragma once
#ifndef CATA_SRC_MP_MP_SESSION_H
#define CATA_SRC_MP_MP_SESSION_H

#if defined(MP_SERVER)

#include <cstddef>
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
    // Offered back as "continue as" in the join menu when the world has it.
    std::string card_save_id;
    // Index into game::contexts and game::player_slots. The two are parallel,
    // and the session is addressed through this index rather than holding a
    // player_context pointer: player_context ships in every build and must not
    // learn about networking types.
    size_t context_index = 0;
};

// The sessions of the running server, indexed the same way as game::contexts.
std::vector<std::unique_ptr<mp_session>> &mp_sessions();

// The session of the player whose context is active, or nullptr during the
// world phase (when no player is active) and in tests without a session.
mp_session *mp_active_session();

// Drops every session. Tests only.
void mp_sessions_reset_for_test();

#endif // MP_SERVER

#endif // CATA_SRC_MP_MP_SESSION_H
