#include "cata_catch.h"

#include "avatar.h"
#include "game.h"
#include "game_test_helper.h"
#include "mp/mp_context.h"

TEST_CASE( "single_player_has_exactly_one_slot", "[coop]" )
{
    game_test_helper::reset_player_slots();
    REQUIRE( g->player_slot_count() == 1 );
    CHECK( g->active_slot() == 0 );
    CHECK( &g->player_at( 0 ) == &get_avatar() );
}

TEST_CASE( "second_slot_is_a_distinct_avatar", "[coop]" )
{
    game_test_helper::reset_player_slots();
    REQUIRE( g->player_slot_count() == 1 );
    avatar &first = g->player_at( 0 );

    avatar &second = g->add_player_slot();
    REQUIRE( g->player_slot_count() == 2 );
    CHECK( &second != &first );

    g->set_active_slot( 1 );
    CHECK( &get_avatar() == &second );

    g->set_active_slot( 0 );
    CHECK( &get_avatar() == &first );

    // Clean up for subsequent tests — every sibling test in this file does
    // this; this one didn't, and the leaked second slot (a bare
    // default-constructed avatar with an unset ID) broke an unrelated
    // crafting test later in the ordered suite.
    game_test_helper::reset_player_slots();
}

TEST_CASE( "second_slot_has_own_map_memory", "[coop]" )
{
    game_test_helper::reset_player_slots();
    avatar &second = g->add_player_slot();
    second.clear_map_memory();
    CHECK( &second != &g->player_at( 0 ) );
    game_test_helper::reset_player_slots();
}

#if defined(MULTIPLAYER)
#include "debug_menu.h"
#include "mp/mp_mode.h"

TEST_CASE( "coop_mode_is_derived_from_the_slot_count", "[coop]" )
{
    game_test_helper::reset_player_slots();
    CHECK_FALSE( mp_coop_active() );

    g->add_player_slot();
    CHECK( mp_coop_active() );

    g->remove_extra_player_slots();
    CHECK( g->player_slot_count() == 1 );
    CHECK( g->active_slot() == 0 );
    CHECK_FALSE( mp_coop_active() );
}

TEST_CASE( "removing_slots_mid_turn_restores_slot_zero_to_u", "[coop]" )
{
    game_test_helper::reset_player_slots();
    g->player_at( 0 ).name = "SlotZero";
    avatar &second = g->add_player_slot();
    second.name = "SlotOne";

    // Reproduce the state the co-op turn loop is in during slot 1's turn: slot 1's
    // data swapped into u, so `u` is the acting character. The activation goes
    // through the swap point (mp_activate_player); ensure is mandatory because
    // it asserts a non-empty contexts. The sync brings slot 1's context into
    // being before activating it.
    mp_ensure_player_context();
    mp_sync_context_count();
    mp_activate_player( 1 );
    REQUIRE( get_avatar().name == "SlotOne" );

    // Disabling from inside that turn must give slot 0's data back to u rather
    // than destroying it along with the extra avatar object.
    g->remove_extra_player_slots();
    CHECK( g->player_slot_count() == 1 );
    CHECK( g->active_slot() == 0 );
    CHECK( g->player_at( 0 ).name == "SlotZero" );
    CHECK( get_avatar().name == "SlotZero" );
    CHECK_FALSE( mp_coop_active() );

    // Leave the contexts consistent with the single remaining slot.
    mp_sync_context_count();
}

TEST_CASE( "coop_debug_toggle_creates_slot_and_enables_mode", "[coop]" )
{
    game_test_helper::reset_player_slots();
    REQUIRE_FALSE( mp_coop_active() );

    debug_menu::toggle_coop_slot( "" );
    CHECK( g->player_slot_count() == 2 );
    CHECK( mp_coop_active() );

    CHECK_FALSE( g->player_at( 1 ).get_body().empty() );
    CHECK( g->player_at( 1 ).getID().is_valid() );
    CHECK( g->player_at( 1 ).pos_abs() == g->player_at( 0 ).pos_abs() );

    // Toggling again destroys the extra slot, which is what turns the mode off.
    debug_menu::toggle_coop_slot( "" );
    CHECK( g->player_slot_count() == 1 );
    CHECK_FALSE( mp_coop_active() );

    // Re-enabling builds a fresh slot rather than a third one.
    debug_menu::toggle_coop_slot( "" );
    CHECK( g->player_slot_count() == 2 );
    CHECK( mp_coop_active() );

    game_test_helper::reset_player_slots();
}
#endif
