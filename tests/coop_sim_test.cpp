#include "cata_catch.h"

#if defined(MULTIPLAYER)
#include <vector>

#include "avatar.h"
#include "calendar.h"
#include "coordinates.h"
#include "debug.h"
#include "game.h"
#include "game_test_helper.h"   // restore_active_state
#include "map.h"
#include "map_helpers_tests.h"
#include "monster.h"
#include "mp/mp_context.h"
#include "mp/mp_tether.h"
#include "player_activity.h"
#include "player_helpers.h"
#include "type_id.h"

TEST_CASE( "get_players_returns_every_slot", "[coop]" )
{
    const size_t before = g->player_slot_count();
    REQUIRE( g->get_players().size() == before );

    g->add_player_slot();
    CHECK( g->get_players().size() == before + 1 );
}

TEST_CASE( "nearest_player_picks_the_closer_slot", "[coop]" )
{
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    map &here = get_map();

    // Slot positions are set explicitly so the assertion does not depend on
    // wherever the test harness left the avatars.
    avatar &a = g->player_at( 0 );
    avatar &b = g->add_player_slot();
    b.set_body();
    a.setpos( here, tripoint_bub_ms( 40, 50, 0 ) );
    b.setpos( here, tripoint_bub_ms( 80, 50, 0 ) );

    // Suppress the invincibility warning from is_dead_state() on the fresh slot.
    capture_debugmsg_during( [&]() {
        CHECK( g->nearest_player( here.get_abs( tripoint_bub_ms( 45, 50, 0 ) ) ) == &a );
        CHECK( g->nearest_player( here.get_abs( tripoint_bub_ms( 75, 50, 0 ) ) ) == &b );
    } );
}

TEST_CASE( "anchor_is_integer_midpoint", "[coop]" )
{
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );

    avatar &a = g->player_at( 0 );
    avatar &b = g->add_player_slot();
    b.set_body();
    a.setpos( get_map(), tripoint_bub_ms( 40, 40, 0 ) );
    b.setpos( get_map(), tripoint_bub_ms( 61, 40, 0 ) );

    // A fresh slot's is_dead_state() warns about invincibility; with both
    // slots alive the anchor is their integer midpoint.
    capture_debugmsg_during( [&]() {
        const tripoint_abs_ms anchor = mp_bubble_anchor();
        // Odd spans round down, deterministically and without floating point.
        CHECK( anchor.x() == ( a.pos_abs().x() + b.pos_abs().x() ) / 2 );
        CHECK( anchor.z() == a.pos_abs().z() );
    } );
}

TEST_CASE( "tether_thresholds_match_the_spec", "[coop]" )
{
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );

    // Park every slot on the same tile so the anchor is exactly that tile no
    // matter how many slots earlier tests leaked.
    for( size_t i = 0; i < g->player_slot_count(); ++i ) {
        g->player_at( i ).setpos( get_map(), tripoint_bub_ms( 60, 60, 0 ) );
    }
    avatar &b = g->add_player_slot();
    b.set_body();
    b.setpos( get_map(), tripoint_bub_ms( 60, 60, 0 ) );

    // Fresh slots warn about invincibility in is_dead_state().
    capture_debugmsg_during( [&]() {
        const tripoint_abs_ms anchor = mp_bubble_anchor();
        CHECK( mp_tether_check( anchor ) == tether_state::ok );
        CHECK( mp_tether_check( anchor + tripoint_rel_ms( 31, 0, 0 ) ) == tether_state::ok );
        CHECK( mp_tether_check( anchor + tripoint_rel_ms( 33, 0, 0 ) ) == tether_state::warning );
        CHECK( mp_tether_check( anchor + tripoint_rel_ms( 43, 0, 0 ) ) == tether_state::blocked );
    } );
}

TEST_CASE( "monster_targets_the_closer_player", "[coop]" )
{
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );

    avatar &a = g->player_at( 0 );
    avatar &b = g->add_player_slot();
    b.set_body();
    // Both slots sit within the monster's (short) test sight range, slot 1
    // strictly closer, so plan() can actually see slot 0 and would pick it
    // before the migration. Slot positions are set explicitly so the assertion
    // does not depend on where the harness left the avatars.
    a.setpos( get_map(), tripoint_bub_ms( 56, 50, 0 ) );
    b.setpos( get_map(), tripoint_bub_ms( 59, 50, 0 ) );

    monster &z = spawn_test_monster( "mon_zombie", tripoint_bub_ms( 58, 50, 0 ) );
    // The test harness leaves the map's visibility cache empty, so no monster
    // can see any player; build it so plan() actually chooses a target.
    get_map().build_map_cache( 0 );
    z.plan();

    // Before the migration the monster targets slot 0, which is one tile
    // further away than slot 1.
    REQUIRE( g->nearest_player( z.pos_abs() ) == &b );
    CHECK( rl_dist( z.get_dest(), b.pos_abs() ) < rl_dist( z.get_dest(), a.pos_abs() ) );
}

#endif
