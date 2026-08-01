#include "cata_catch.h"

#if defined(MULTIPLAYER)
#include <vector>

#include "avatar.h"
#include "calendar.h"
#include "coordinates.h"
#include "debug.h"
#include "game.h"
#include "map.h"
#include "map_helpers_tests.h"
#include "mp/mp_hotseat_loop.h"
#include "player_activity.h"
#include "player_helpers.h"
#include "type_id.h"

TEST_CASE( "get_players_returns_every_slot", "[hotseat]" )
{
    const size_t before = g->player_slot_count();
    REQUIRE( g->get_players().size() == before );

    g->add_player_slot();
    CHECK( g->get_players().size() == before + 1 );
}

TEST_CASE( "nearest_player_picks_the_closer_slot", "[hotseat]" )
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

TEST_CASE( "hotseat_turn_advances_calendar_once", "[hotseat]" )
{
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    // The test harness never starts a game, so gamemode is null and do_turn()
    // would dereference it on the second call.
    g->ensure_gamemode_for_test();
    // With no moves and no activity a slot's input loop is skipped, which is
    // what makes this headless. Every slot must be idled, not just slot 0:
    // slots leaked by earlier tests still hold moves, and the world phase
    // replenishes slot 0's moves each turn, so this is applied before every
    // call.
    auto idle_all_slots = [&]() {
        for( size_t i = 0; i < g->player_slot_count(); ++i ) {
            avatar &idle = g->player_at( i );
            idle.set_moves( 0 );
            idle.activity = player_activity();
        }
    };
    idle_all_slots();

    // The first mp_hotseat_do_turn() call in a process is the one-time
    // "new_game" setup pass (weather.on_game_start), not a calendar advance.
    mp_hotseat_do_turn();

    idle_all_slots();
    const int before = to_turn<int>( calendar::turn );
    mp_hotseat_do_turn();
    CHECK( to_turn<int>( calendar::turn ) == before + 1 );
}

TEST_CASE( "hotseat_turn_drains_moves_from_every_slot", "[hotseat]" )
{
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();

    // Idle any slots leaked by earlier tests so only the two under test act.
    for( size_t i = 0; i < g->player_slot_count(); ++i ) {
        g->player_at( i ).set_moves( 0 );
        g->player_at( i ).activity = player_activity();
    }

    avatar &a = g->player_at( 0 );
    avatar &b = g->add_player_slot();
    b.set_body();

    // Both slots get moves plus a time-based activity. The hotseat loop
    // consumes a slot's moves through the activity branch; the handle_action()
    // branch reads real input and cannot run headless in test mode.
    a.set_moves( 100 );
    b.set_moves( 100 );
    a.activity = player_activity( activity_id( "ACT_HACKING" ), 500, 0 );
    b.activity = player_activity( activity_id( "ACT_HACKING" ), 500, 0 );

    mp_hotseat_do_turn();

    // Slot 1 is never replenished, so its moves are fully drained by the
    // player phase; both slots' activities consumed one turn of moves.
    CHECK( b.get_moves() == 0 );
    CHECK( a.activity.moves_left < 500 );
    CHECK( b.activity.moves_left < 500 );
}
#endif
