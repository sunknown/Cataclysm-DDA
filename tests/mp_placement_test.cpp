#include "cata_catch.h"

#include <optional>
#include <vector>

#include "avatar.h"
#include "calendar.h"
#include "coordinates.h"
#include "enums.h"
#include "game.h"
#include "game_test_helper.h"
#include "map.h"
#include "map_helpers.h"
#include "map_helpers_tests.h"
#include "mp/mp_context.h"
#include "mp/mp_placement.h"
#include "mp/mp_tether.h"
#include "player_helpers.h"

// Placement of characters into a world that already exists. Every test here
// stands slot 0 on a cleared map and treats its position as the anchor.

static void stand_on_a_cleared_map()
{
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    // build_test_map() leaves the creatures the test world generated.
    clear_creatures();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

TEST_CASE( "a_free_tile_near_a_target_is_passable_and_unoccupied", "[coop]" )
{
    restore_active_state restore;
    stand_on_a_cleared_map();

    const tripoint_abs_ms target = g->player_at( 0 ).pos_abs();

    const std::optional<tripoint_abs_ms> spot = mp_find_free_tile_near( target );
    REQUIRE( spot.has_value() );

    map &here = get_map();
    CHECK( here.passable( here.get_bub( *spot ) ) );
    // Not the target itself: somebody is standing there, and two avatars on one
    // tile is the failure this function exists to prevent.
    CHECK( *spot != target );
    CHECK( rl_dist( *spot, target ) <= MP_TETHER_SOFT_LIMIT );
}

TEST_CASE( "two_placements_in_a_row_do_not_collide", "[coop]" )
{
    restore_active_state restore;
    stand_on_a_cleared_map();

    const tripoint_abs_ms target = g->player_at( 0 ).pos_abs();

    // Freshly created characters, as the join menu hands them over: a bare
    // avatar has no profession, and placement hands out its items.
    avatar &a = g->add_player_slot();
    avatar &b = g->add_player_slot();
    mp_sync_context_count();
    REQUIRE( a.create( character_type::NOW ) );
    REQUIRE( b.create( character_type::NOW ) );

    REQUIRE( place_new_character( a, target ) );
    REQUIRE( place_new_character( b, target ) );

    // The second placement runs while the first avatar already stands on its
    // tile -- and an avatar outside `u` is in no creature cache at all, which
    // is the case a creature-tracker-only occupancy check misses.
    CHECK( a.pos_abs() != b.pos_abs() );
    CHECK( a.pos_abs() != target );
    CHECK( b.pos_abs() != target );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

TEST_CASE( "a_new_character_next_to_an_anchor_gets_its_profession_items", "[coop]" )
{
    restore_active_state restore;
    stand_on_a_cleared_map();

    const tripoint_abs_ms target = g->player_at( 0 ).pos_abs();
    avatar &fresh = g->add_player_slot();
    mp_sync_context_count();
    // NOW is the random character without the creation screen.
    REQUIRE( fresh.create( character_type::NOW ) );

    const time_point turn_before = calendar::turn;
    REQUIRE( place_new_character( fresh, target ) );

    // The personal half of start_game ran...
    CHECK( fresh.getID().is_valid() );
    CHECK_FALSE( fresh.inv_dump().empty() );
    // ...and none of its world steps did. start_calendar() here would reset the
    // clock of a world that has been played for weeks.
    CHECK( calendar::turn == turn_before );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

TEST_CASE( "the_anchor_is_the_first_continuing_slot_not_slot_zero", "[coop]" )
{
    restore_active_state restore;
    stand_on_a_cleared_map();

    // Slot 0 is a brand new character; slot 1 continued from a save. Slots are
    // handed out by connection order, so this ordering is ordinary, and
    // anchoring on slot 0 would anchor on a character with no position yet.
    avatar &continuing = g->add_player_slot();
    continuing.set_body();
    mp_sync_context_count();
    const tripoint_abs_ms saved_pos = g->player_at( 0 ).pos_abs() + tripoint::east * 5;
    continuing.setpos( saved_pos );
    // Slot 0 as a brand new character arrives: no ID yet, and standing nowhere
    // near the anchor.
    g->player_at( 0 ).setID( character_id(), /*force=*/true );
    g->player_at( 0 ).setpos( saved_pos + tripoint::west * 20 );

    std::vector<bool> is_new = { true, false };
    std::vector<size_t> moved;
    REQUIRE( mp_place_roster( is_new, moved ) );

    // The continuing character stayed put; the new one came to it.
    CHECK( continuing.pos_abs() == saved_pos );
    CHECK( rl_dist( g->player_at( 0 ).pos_abs(), saved_pos ) <= 1 );
    CHECK( moved.empty() );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

TEST_CASE( "scattered_continuations_are_pulled_to_the_anchor_and_reported", "[coop]" )
{
    restore_active_state restore;
    stand_on_a_cleared_map();

    avatar &far_away = g->add_player_slot();
    far_away.set_body();
    mp_sync_context_count();
    const tripoint_abs_ms anchor_pos = g->player_at( 0 ).pos_abs();
    // Far outside the bubble: played solo on the other side of the map.
    far_away.setpos( anchor_pos + tripoint::east * 500 );

    std::vector<bool> is_new = { false, false };
    std::vector<size_t> moved;
    REQUIRE( mp_place_roster( is_new, moved ) );

    CHECK( rl_dist( far_away.pos_abs(), anchor_pos ) <= MP_TETHER_SOFT_LIMIT );
    // Reported, not silent: it is that player's save, and they must be told
    // before the session starts rather than discover it later.
    CHECK( moved == std::vector<size_t> { 1 } );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}
