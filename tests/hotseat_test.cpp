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
