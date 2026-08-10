#include "cata_catch.h"

#if defined(MULTIPLAYER)

#include "avatar.h"
#include "game.h"
#include "game_test_helper.h"
#include "map_helpers.h"
#include "map_helpers_tests.h"
#include "mp/mp_context.h"
#include "mp/mp_turn.h"
#include "player_activity.h"
#include "player_helpers.h"

// The barrier rule is the whole turn model. These tests pin the four ways a
// player can stop holding it -- and the one way they keep holding it.

TEST_CASE( "a_player_out_of_moves_has_acted", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    mp_turn_reset_all();

    avatar &u = g->player_at( 0 );
    u.set_moves( 0 );
    u.activity = player_activity();

    CHECK( mp_player_state_of( 0 ) == mp_player_state::acted );
    CHECK( mp_barrier_satisfied() );
}

TEST_CASE( "a_player_at_an_input_wait_with_moves_holds_the_barrier", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    mp_turn_reset_all();

    avatar &u = g->player_at( 0 );
    u.set_moves( 100 );
    u.activity = player_activity();
    mp_set_waiting_for_input( 0, true );

    // This is the case the whole design is about: a turn is typically one
    // action, and browsing a menu costs no time, so a player reading a recipe
    // list would freeze their partner for as long as they read.
    CHECK( mp_player_state_of( 0 ) == mp_player_state::holding );
    CHECK_FALSE( mp_barrier_satisfied() );

    mp_set_waiting_for_input( 0, false );
    CHECK( mp_player_state_of( 0 ) == mp_player_state::acting );
}

TEST_CASE( "a_running_activity_counts_as_having_acted", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    mp_turn_reset_all();

    avatar &u = g->player_at( 0 );
    u.set_moves( 100 );
    u.activity = player_activity( activity_id( "ACT_HACKING" ), 500, 0 );
    mp_set_waiting_for_input( 0, true );

    // Occupied, not thinking: the activity will spend the turn whether or not
    // the player is looking at the screen, so the barrier must not wait.
    CHECK( mp_player_state_of( 0 ) == mp_player_state::acted );
    CHECK( mp_barrier_satisfied() );
}

TEST_CASE( "a_travel_destination_counts_as_having_acted", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    mp_turn_reset_all();

    avatar &u = g->player_at( 0 );
    u.set_moves( 100 );
    u.activity = player_activity();
    // Set up a full distant destination: route + ACT_TRAVELLING activity + omt_path.
    // An adjacent tile alone isn't "distant" -- has_distant_destination() also
    // requires the travelling activity and a non-empty overmap tile path.
    u.set_destination( { u.pos_bub() + tripoint::east },
                       player_activity( activity_id( "ACT_TRAVELLING" ), 500, 0 ) );
    u.omt_path.emplace_back();
    mp_set_waiting_for_input( 0, true );

    // This is the cheap form of planned turns: the player set a route, walked
    // away into a menu, and their character keeps moving.
    REQUIRE( u.has_distant_destination() );
    CHECK( mp_player_state_of( 0 ) == mp_player_state::acted );
    CHECK( mp_barrier_satisfied() );

    u.clear_destination();
}

TEST_CASE( "the_barrier_waits_for_the_slowest_player", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();
    mp_turn_reset_all();

    avatar &a = g->player_at( 0 );
    a.set_moves( 0 );
    a.activity = player_activity();
    b.set_moves( 100 );
    b.activity = player_activity();
    mp_set_waiting_for_input( 1, true );

    CHECK_FALSE( mp_barrier_satisfied() );

    b.set_moves( 0 );
    CHECK( mp_barrier_satisfied() );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_turn_reset_all();
}

#endif // defined(MULTIPLAYER)
