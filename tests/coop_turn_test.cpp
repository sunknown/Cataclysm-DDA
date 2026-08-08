#include "cata_catch.h"

#include "activity_type.h"
#include "avatar.h"
#include "bodypart.h"
#include "calendar.h"
#include "game.h"
#include "game_test_helper.h"
#include "map_helpers_tests.h"
#include "mp/mp_context.h"
#include "player_activity.h"
#include "player_helpers.h"
#include "type_id.h"
#include "units.h"

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

TEST_CASE( "avatar_turn_post_replenishes_moves", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();

    avatar &u = g->player_at( 0 );
    u.set_moves( 0 );
    u.activity = player_activity();

    g->avatar_turn_post( u );

    CHECK( u.get_moves() > 0 );
}

TEST_CASE( "avatar_turn_pre_drains_a_running_activity", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();

    avatar &u = g->player_at( 0 );
    u.set_moves( 100 );
    u.activity = player_activity( activity_id( "ACT_HACKING" ), 500, 0 );

    g->avatar_turn_pre( u );

    // The activity consumed the turn's moves without the player pressing
    // anything: this is what makes an occupied player count as "has acted"
    // at the barrier (mp_turn, task 7).
    CHECK( u.get_moves() <= 0 );
}

TEST_CASE( "avatar_turn_post_runs_for_every_slot_independently", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();
    game_test_helper::reset_player_slots();

    avatar &a = g->player_at( 0 );
    avatar &b = g->add_player_slot();
    b.set_body();
    a.set_moves( 0 );
    b.set_moves( 0 );
    // Start slot b's torso well below normal so update_bodytemp() has
    // somewhere to move it: a slot that never got processed would stay put.
    const units::temperature cold_start = units::from_celsius( 20 );
    b.set_part_temp_cur( bodypart_id( "torso" ), cold_start );

    g->avatar_turn_post( a );
    g->avatar_turn_post( b );

    // The defect the old hotseat loop had: slot 1 only got process_turn(), so
    // its body temperature, wetness and morale never updated. Both slots now
    // go through the same function.
    CHECK( a.get_moves() > 0 );
    CHECK( b.get_moves() > 0 );
    CHECK( b.get_part_temp_cur( bodypart_id( "torso" ) ) != cold_start );

    game_test_helper::reset_player_slots();
}

TEST_CASE( "calendar_advances_once_per_turn", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();

    // With no moves and no activity a slot's action loop is skipped, which is
    // what makes this headless: handle_action() would read real input.
    auto idle_all_slots = [&]() {
        for( size_t i = 0; i < g->player_slot_count(); ++i ) {
            avatar &idle = g->player_at( i );
            idle.set_moves( 0 );
            idle.activity = player_activity();
        }
    };

    idle_all_slots();
    g->do_turn();          // one-time new_game pass

    idle_all_slots();
    const int before = to_turn<int>( calendar::turn );
    g->do_turn();
    CHECK( to_turn<int>( calendar::turn ) == before + 1 );
}

TEST_CASE( "every_slot_gets_its_moves_back", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();
    game_test_helper::reset_player_slots();

    avatar &a = g->player_at( 0 );
    avatar &b = g->add_player_slot();
    b.set_body();

    // Both slots spend their turn on an activity: the action loop reads real
    // input and cannot run headless, the activity branch can.
    a.set_moves( 100 );
    b.set_moves( 100 );
    a.activity = player_activity( activity_id( "ACT_HACKING" ), 500, 0 );
    b.activity = player_activity( activity_id( "ACT_HACKING" ), 500, 0 );

    g->do_turn();

    CHECK( a.get_moves() > 0 );
    CHECK( b.get_moves() > 0 );

    game_test_helper::reset_player_slots();
}

TEST_CASE( "moves_come_back_only_after_the_world_moved", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();

    avatar &u = g->player_at( 0 );
    u.set_moves( 0 );
    u.activity = player_activity();

    // The single-player ordering the whole barrier rests on: world_turn_post
    // (monmove) happens while the player is still out of moves.
    g->world_turn_pre();
    CHECK( u.get_moves() <= 0 );
    g->world_turn_post();
    CHECK( u.get_moves() <= 0 );
    g->avatar_turn_post( u );
    CHECK( u.get_moves() > 0 );
}
