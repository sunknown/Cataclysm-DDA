#include "cata_catch.h"

#if defined(MULTIPLAYER)
#include <vector>

#include "avatar.h"
#include "coordinates.h"
#include "debug.h"
#include "game.h"
#include "map.h"
#include "map_helpers_tests.h"
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
#endif
