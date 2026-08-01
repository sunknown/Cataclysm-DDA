#include "cata_catch.h"

#if defined(MULTIPLAYER)
#include "mp/mp_mode.h"

TEST_CASE( "mp_mode_defaults_to_inactive", "[hotseat]" )
{
    mp_mode_set_active( false );
    CHECK_FALSE( mp_mode_active() );
    mp_mode_set_active( true );
    CHECK( mp_mode_active() );
    mp_mode_set_active( false );
}
#endif

#include "avatar.h"
#include "game.h"

TEST_CASE( "single_player_has_exactly_one_slot", "[hotseat]" )
{
    REQUIRE( g->player_slot_count() == 1 );
    CHECK( g->active_slot() == 0 );
    CHECK( &g->player_at( 0 ) == &get_avatar() );
}

TEST_CASE( "second_slot_is_a_distinct_avatar", "[hotseat]" )
{
    REQUIRE( g->player_slot_count() == 1 );
    avatar &first = g->player_at( 0 );

    avatar &second = g->add_player_slot();
    REQUIRE( g->player_slot_count() == 2 );
    CHECK( &second != &first );

    g->set_active_slot( 1 );
    CHECK( &get_avatar() == &second );

    g->set_active_slot( 0 );
    CHECK( &get_avatar() == &first );
}

TEST_CASE( "second_slot_has_own_map_memory", "[hotseat]" )
{
    while( g->player_slot_count() > 1 ) {
        // Ensure a clean starting point regardless of test order.
        g->set_active_slot( 0 );
        break;
    }
    avatar &second = g->add_player_slot();
    second.clear_map_memory();
    CHECK( &second != &g->player_at( 0 ) );
}
