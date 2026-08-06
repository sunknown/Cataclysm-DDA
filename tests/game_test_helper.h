#pragma once

#include "avatar.h"
#include "game.h"
#include "mp/mp_context.h"
#include "ui_manager.h"

// Test helper to reset player slots between tests (friend of game).
class game_test_helper
{
    public:
        static void reset_player_slots()
        {
            // Get the base avatar reference before clearing (get_avatar() uses player_at).
            avatar &base = g->player_at( 0 );
            // Clear extra slots, keeping only the original avatar at index 0.
            g->owned_extra_players.clear();
            // Rebuild player_slots with just the base avatar.
            g->player_slots.clear();
            g->player_slots.push_back( &base );
            g->active_slot_index = 0;
            // Note: do NOT clear the base avatar's ID here. Setting it to a
            // fresh character_id() leaves the avatar with ID -1, and any later
            // test that finalizes a passive craft (which stores the crafter's
            // ID) then sees "crafter -1 gone" and crashes. The ID reset was
            // only ever needed to deserialize a save into the live game, which
            // the round-trip test no longer does.
        }
};

// Restores the active context, the active ui stack and catacurses::stdscr on
// scope exit. Any test that points the globals at stack locals must own one of
// these (tests that replace stdscr with a scratch window leave it there
// otherwise, leaking a test window into every later test).
class restore_active_state
{
        player_context *const ctx_;
        ui_stack_t *const stack_;
        catacurses::window const stdscr_;
    public:
        restore_active_state() :
            ctx_( mp_active_context_ptr() ), stack_( ui_manager::get_active_stack() ),
            stdscr_( catacurses::stdscr ) {}
        ~restore_active_state()
        {
            mp_set_active_context( ctx_ );
            ui_manager::set_active_stack( stack_ );
            catacurses::stdscr = stdscr_;
        }
};
