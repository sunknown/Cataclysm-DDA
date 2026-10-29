#include "cata_catch.h"

#if defined(MP_SERVER)

#include "avatar.h"
#include "calendar.h"
#include "coordinates.h"
#include "game.h"
#include "mp/mp_tether.h"
#include "mp_session_harness.h"

// A session that starts the way a real one now does: nobody has a character,
// and each client goes through its own join menu.

TEST_CASE( "the_world_does_not_tick_until_everyone_has_chosen", "[mp]" )
{
    mp_session_harness harness( 2, /*latency_ms=*/5, mp_harness_mode::join );

    const time_point turn_before = calendar::turn;
    REQUIRE_FALSE( harness.run_until( []() {
        return false;
    }, 3 ) );
    // One player still in the menu means the world is not running: the roster
    // is fixed, everyone arrives together, and joining mid-session is Phase 5.
    harness.choose_random_character( 0 );
    REQUIRE_FALSE( harness.run_until( []() {
        return false;
    }, 20 ) );
    CHECK( calendar::turn == turn_before );

    harness.choose_random_character( 1 );
    CHECK( harness.run_until( [turn_before]() {
        return calendar::turn > turn_before;
    }, 100 ) );
}

TEST_CASE( "two_fresh_characters_start_next_to_each_other", "[mp]" )
{
    mp_session_harness harness( 2, 5, mp_harness_mode::join );
    harness.choose_random_character( 0 );
    harness.choose_random_character( 1 );
    REQUIRE( harness.run_until( [&harness]() {
        return harness.seated();
    }, 100 ) );

    // Both inside the tether means the bubble holds them both, which is the
    // whole premise of tethered co-op; on different tiles, or one stands on the
    // other.
    const tripoint_abs_ms a = g->player_at( 0 ).pos_abs();
    const tripoint_abs_ms b = g->player_at( 1 ).pos_abs();
    CHECK( a != b );
    CHECK( rl_dist( a, b ) <= MP_TETHER_SOFT_LIMIT );
    // Real characters, made by the creation screen, not the blank slots.
    CHECK_FALSE( g->player_at( 0 ).name.empty() );
    CHECK( g->player_at( 0 ).name != g->player_at( 1 ).name );
}

#endif // MP_SERVER
