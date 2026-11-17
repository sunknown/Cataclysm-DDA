#include "cata_catch.h"

#if defined(MP_SERVER)

#include <string>
#include <utility>
#include <vector>

#include "avatar.h"
#include "calendar.h"
#include "coordinates.h"
#include "game.h"
#include "game_test_helper.h"
#include "messages.h"
#include "mp/mp_server_main.h"
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

TEST_CASE( "a_client_that_has_chosen_sees_the_waiting_screen", "[mp]" )
{
    mp_session_harness harness( 2, /*latency_ms=*/5, mp_harness_mode::join );
    harness.choose_random_character( 0 );
    // A player who has chosen is parked on the baton, not on its wire, so it
    // never reaches another input wait during the join phase -- and the input
    // waits are the only other place a grid is sent. The screen the world draws
    // for it has to be flushed, or this client sits on the creator's last frame
    // until the game starts.
    REQUIRE( harness.run_until( [&harness]() {
        return mp_grid_contains( harness.screen( 0 ), "Waiting for the others" );
    }, 60 ) );
    // And it names who is still expected: the roster is fixed, so exactly one
    // player is left.
    CHECK( mp_grid_contains( harness.screen( 0 ), "P1" ) );
}

TEST_CASE( "a_client_that_quits_the_join_menu_frees_its_seat", "[mp]" )
{
    mp_session_harness harness( 2, /*latency_ms=*/5, mp_harness_mode::join );

    // P0 has chosen and is waiting for P1, who is still reading its menu.
    harness.choose_random_character( 0 );
    REQUIRE( harness.run_until( [&harness]() {
        return mp_grid_contains( harness.screen( 0 ), "P1" );
    }, 60 ) );

    harness.quit_join_menu( 1 );
    // The quitter is gone: its wire is closed, its session is out of the
    // roster, and P0 is still on its waiting screen with its choice intact.
    REQUIRE( harness.run_until( [&harness]() {
        return !harness.client_connected( 1 ) && !harness.seat_occupied( 1 );
    }, 100 ) );
    CHECK( mp_grid_contains( harness.screen( 0 ), "Waiting for the others" ) );

    // A third client takes the same seat and runs its own join menu.
    harness.connect_replacement_client( 1 );
    harness.choose_random_character( 1 );
    REQUIRE( harness.run_until( [&harness]() {
        return harness.seated();
    }, 100 ) );

    // The party starts with exactly the two characters this session chose: P0
    // was never sent back to its menu, and the quitter's seat was filled.
    CHECK( g->player_slot_count() == 2 );
    CHECK_FALSE( g->player_at( 0 ).name.empty() );
    CHECK_FALSE( g->player_at( 1 ).name.empty() );
    CHECK( g->player_at( 0 ).name != g->player_at( 1 ).name );
}

TEST_CASE( "the_server_keeps_listening_after_everybody_quits_the_join_menu", "[mp]" )
{
    mp_session_harness harness( 2, /*latency_ms=*/5, mp_harness_mode::join );

    harness.quit_join_menu( 0 );
    harness.quit_join_menu( 1 );
    REQUIRE( harness.run_until( [&harness]() {
        return !harness.seat_occupied( 0 ) && !harness.seat_occupied( 1 );
    }, 100 ) );

    // The session did not end with the last client: the world is still loaded,
    // the two seats are still there, and both can be taken again.
    harness.connect_replacement_client( 0 );
    harness.connect_replacement_client( 1 );
    harness.choose_random_character( 0 );
    harness.choose_random_character( 1 );
    REQUIRE( harness.run_until( [&harness]() {
        return harness.seated();
    }, 100 ) );
    CHECK( g->player_slot_count() == 2 );
}

TEST_CASE( "a_client_whose_wire_drops_in_the_join_menu_loses_only_its_seat", "[mp]" )
{
    mp_session_harness harness( 2, /*latency_ms=*/5, mp_harness_mode::join );

    harness.choose_random_character( 0 );
    REQUIRE( harness.run_until( [&harness]() {
        return mp_grid_contains( harness.screen( 0 ), "P1" );
    }, 60 ) );

    harness.drop_client( 1 );
    REQUIRE( harness.run_until( [&harness]() {
        return !harness.seat_occupied( 1 );
    }, 100 ) );
    // Dropping a wire is leaving, not an error: the other client keeps both its
    // choice and its screen, and the seat can be taken again.
    CHECK( mp_grid_contains( harness.screen( 0 ), "Waiting for the others" ) );
    harness.connect_replacement_client( 1 );
    harness.choose_random_character( 1 );
    REQUIRE( harness.run_until( [&harness]() {
        return harness.seated();
    }, 100 ) );
    CHECK_FALSE( g->player_at( 0 ).name.empty() );
}

TEST_CASE( "a_client_that_drops_inside_the_character_creator_loses_only_its_seat", "[mp]" )
{
    mp_session_harness harness( 2, /*latency_ms=*/5, mp_harness_mode::join );

    // P0 has chosen and is waiting for P1, who opens the creator and is then
    // dropped there -- inside the tab loop, not in a menu. That loop has no
    // menu action to cancel on (a tab with a uilist answers the synthetic QUIT
    // as a cancel and asks again; a tab without one asks "return to main
    // menu?", which nothing can answer), so this is the path that used to wedge
    // the join phase for everybody.
    harness.choose_random_character( 0 );
    REQUIRE( harness.run_until( [&harness]() {
        return mp_grid_contains( harness.screen( 0 ), "P1" );
    }, 60 ) );
    harness.press( 1, 'r' );
    REQUIRE( harness.run_until( [&harness]() {
        return !mp_grid_contains( harness.screen( 1 ), "Join the game" );
    }, 60 ) );

    harness.drop_client( 1 );
    REQUIRE( harness.run_until( [&harness]() {
        return !harness.seat_occupied( 1 );
    }, 100 ) );
    // P0 never went back to its menu and keeps its choice; the seat is free.
    CHECK( mp_grid_contains( harness.screen( 0 ), "Waiting for the others" ) );

    harness.connect_replacement_client( 1 );
    harness.choose_random_character( 1 );
    REQUIRE( harness.run_until( [&harness]() {
        return harness.seated();
    }, 150 ) );
    CHECK( g->player_slot_count() == 2 );
    CHECK_FALSE( g->player_at( 0 ).name.empty() );
    CHECK_FALSE( g->player_at( 1 ).name.empty() );
}

TEST_CASE( "a_client_that_quits_inside_the_character_creator_frees_its_seat", "[mp]" )
{
    mp_session_harness harness( 2, /*latency_ms=*/5, mp_harness_mode::join );

    harness.choose_random_character( 0 );
    REQUIRE( harness.run_until( [&harness]() {
        return mp_grid_contains( harness.screen( 0 ), "P1" );
    }, 60 ) );
    harness.press( 1, 'r' );
    REQUIRE( harness.run_until( [&harness]() {
        return !mp_grid_contains( harness.screen( 1 ), "Join the game" );
    }, 60 ) );

    // 27 is the keycode Escape reaches the input layer as; the creator's QUIT
    // asks whether to return to the main menu, which a person answers Y. Back
    // in the join menu, the Quit entry frees the seat.
    harness.press( 1, 27 );
    harness.press( 1, 'Y' );
    REQUIRE( harness.run_until( [&harness]() {
        return mp_grid_contains( harness.screen( 1 ), "Join the game" );
    }, 100 ) );
    harness.quit_join_menu( 1 );
    REQUIRE( harness.run_until( [&harness]() {
        return !harness.seat_occupied( 1 );
    }, 100 ) );
    CHECK( mp_grid_contains( harness.screen( 0 ), "Waiting for the others" ) );

    harness.connect_replacement_client( 1 );
    harness.choose_random_character( 1 );
    REQUIRE( harness.run_until( [&harness]() {
        return harness.seated();
    }, 150 ) );
    CHECK_FALSE( g->player_at( 1 ).name.empty() );
}

TEST_CASE( "a_client_that_has_chosen_and_then_drops_frees_its_seat", "[mp]" )
{
    mp_session_harness harness( 2, /*latency_ms=*/5, mp_harness_mode::join );

    // P0 chooses and is parked on the waiting screen, not on its wire, so only
    // the world's poll can notice the dead transport. Left undetected, the seat
    // would be logged as playing and the first turn would end the session for
    // everybody.
    harness.choose_random_character( 0 );
    REQUIRE( harness.run_until( [&harness]() {
        return mp_grid_contains( harness.screen( 0 ), "P1" );
    }, 60 ) );
    harness.drop_client( 0 );
    REQUIRE( harness.run_until( [&harness]() {
        return !harness.seat_occupied( 0 );
    }, 100 ) );

    // The freed seat is taken again and both players choose: the party starts
    // with two characters, and P1 never left its menu.
    harness.connect_replacement_client( 0 );
    harness.choose_random_character( 0 );
    harness.choose_random_character( 1 );
    REQUIRE( harness.run_until( [&harness]() {
        return harness.seated();
    }, 150 ) );
    CHECK( g->player_slot_count() == 2 );
    CHECK_FALSE( g->player_at( 0 ).name.empty() );
    CHECK_FALSE( g->player_at( 1 ).name.empty() );
    CHECK( g->player_at( 0 ).name != g->player_at( 1 ).name );
}

TEST_CASE( "a_character_pulled_to_the_group_is_announced_once", "[mp]" )
{
    game_test_helper::reset_player_slots();
    g->player_at( 0 ).name = "Margaret";
    g->add_player_slot().name = "Kevin";

    Messages::clear_messages();
    mp_announce_moved_characters( { 0, 1 } );

    // One line per moved character. The message log is one process-wide log, so
    // announcing under each player's context in turn repeated every line once
    // per player and told the players who had not been moved that it was them
    // (seat_roster used to loop over both). The placement itself is covered by
    // the placement tests; this pins the log contract seating relies on.
    const std::vector<std::pair<std::string, std::string>> messages =
        Messages::recent_messages( 0 );
    REQUIRE( messages.size() == 2 );
    CHECK( messages[0].second.find( "Margaret" ) != std::string::npos );
    CHECK( messages[1].second.find( "Kevin" ) != std::string::npos );
}

#endif // MP_SERVER
