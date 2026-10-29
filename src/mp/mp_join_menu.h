#pragma once
#ifndef CATA_SRC_MP_MP_JOIN_MENU_H
#define CATA_SRC_MP_MP_JOIN_MENU_H

#include <optional>
#include <string>
#include <vector>

#include "worldfactory.h"

// What one player decided in the join menu.
struct mp_join_choice {
    enum class kind {
        // Created a fresh character, now in the active slot; needs placing.
        created,
        // Continuing an existing one; its position comes from its save.
        continued,
        // Left before choosing.
        quit,
    };
    kind decision = kind::quit;
    // Set when decision == continued: which save to load.
    std::optional<save_t> save;
};

// The save the continue entry offers, given the world's saves and the save id
// the player's client reported from its card. Empty when there is no card or
// the card names no character in this world -- the card outlived the
// character, or another world runs on the same address -- which simply means
// the entry is absent. Never an error: a first connect must not fail.
std::optional<save_t> mp_continue_target( const std::vector<save_t> &world_saves,
        const std::string &card_save_id );

// Runs the join menu for the active player, in their own context and their own
// thread, and returns what they chose. Streams to their client like any other
// screen. Called with the baton held; the menu's input waits pass it on.
//
// `card_save_id` is the save id their client reported in its hello.
//
// A character is only ever handed back with a name nobody else has: not one of
// the world's saves (creating it would overwrite that save) and not one another
// player of this session has already taken. Two players racing for one name is
// settled by whoever confirms first; the other is asked for a different one.
mp_join_choice mp_run_join_menu( const std::string &card_save_id );

// Forgets the names claimed by a previous join phase.
void mp_join_reset_claims();

// Draws the "waiting for the others" screen into the active context: who is
// still choosing. An ordinary drawn screen, streamed like any other; the world
// redraws it for every player who has chosen whenever the list changes.
void mp_draw_waiting_screen( const std::vector<std::string> &still_choosing );

#endif // CATA_SRC_MP_MP_JOIN_MENU_H
