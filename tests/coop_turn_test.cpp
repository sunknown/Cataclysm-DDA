#include "cata_catch.h"

#include "avatar.h"
#include "calendar.h"
#include "game.h"
#include "game_test_helper.h"
#include "map_helpers_tests.h"
#include "mp/mp_context.h"
#include "player_activity.h"
#include "player_helpers.h"

// The world halves of a turn are what the orchestrator runs with no active
// player. These tests pin the two properties the co-op barrier depends on:
// the calendar advances exactly once in the pre half, and the post half never
// hands anybody their moves back.

TEST_CASE( "world_turn_pre_advances_the_calendar_once", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();

    // The first call in a process is the one-time new_game pass
    // (weather.on_game_start), not a calendar advance.
    g->world_turn_pre();

    const int before = to_turn<int>( calendar::turn );
    g->world_turn_pre();
    CHECK( to_turn<int>( calendar::turn ) == before + 1 );
}

TEST_CASE( "world_turn_post_does_not_replenish_moves", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();

    avatar &u = g->player_at( 0 );
    u.set_moves( 0 );
    u.activity = player_activity();

    g->world_turn_post();

    // Moves come back in avatar_turn_post, which the orchestrator runs after
    // this. Replenishing here would let a player act again before the monsters
    // had moved, and would make the barrier meaningless.
    CHECK( u.get_moves() == 0 );
}
