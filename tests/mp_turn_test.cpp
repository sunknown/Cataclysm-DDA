#include "cata_catch.h"

#if defined(MULTIPLAYER)

#include "avatar.h"
#include "display.h"
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

TEST_CASE( "a_hurry_countdown_drops_the_holder_from_the_barrier", "[mp]" )
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
    REQUIRE_FALSE( mp_barrier_satisfied() );

    mp_request_hurry();
    CHECK( mp_hurry_remaining_ms() > 0 );

    // Expire it by ticking past the countdown.
    mp_advance_hurry_for_test( MP_HURRY_MS + 1 );
    mp_tick_hurry();

    // The holder is dropped, not interrupted: still at their input wait, still
    // with moves, but no longer keeping the world from moving.
    CHECK( mp_player_state_of( 1 ) == mp_player_state::not_waited );
    CHECK( b.get_moves() > 0 );
    CHECK( mp_barrier_satisfied() );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_turn_reset_all();
}

// Two players, slot 1 holding at an input wait, and a zombie next to slot 0 that
// can see them: the one setup in which both countdowns can be armed.
static void set_up_holder_with_a_hostile_in_view()
{
    clear_avatar();
    clear_map();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();
    mp_turn_reset_all();

    avatar &a = g->player_at( 0 );
    a.set_moves( 0 );
    b.set_moves( 100 );
    b.activity = player_activity();
    mp_set_waiting_for_input( 1, true );
    spawn_test_monster( "mon_zombie", a.pos_bub() + tripoint::east );
}

TEST_CASE( "a_running_combat_countdown_does_not_swallow_the_hurry_press", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    set_up_holder_with_a_hostile_in_view();

    mp_tick_hurry();
    REQUIRE( mp_hurry_remaining_ms() > 0 );
    REQUIRE( mp_hurry_remaining_ms() <= MP_HURRY_COMBAT_MS );

    mp_request_hurry();

    // The combat countdown expires and drops the holder. The press made while
    // it was running must still be counting: it was swallowed whole when both
    // shared one flag, and the player's deliberate request vanished unanswered.
    mp_advance_hurry_for_test( MP_HURRY_COMBAT_MS + 1, mp_hurry_source::combat );
    mp_tick_hurry();
    REQUIRE( mp_player_state_of( 1 ) == mp_player_state::not_waited );
    CHECK( mp_hurry_remaining_ms() > MP_HURRY_COMBAT_MS );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_turn_reset_all();
}

TEST_CASE( "a_running_hurry_press_does_not_swallow_the_combat_countdown", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    set_up_holder_with_a_hostile_in_view();

    mp_request_hurry();
    mp_tick_hurry();

    // Combat urgency applies its own short length rather than waiting out the
    // fifteen seconds of a press that happened to come first.
    CHECK( mp_hurry_remaining_ms() <= MP_HURRY_COMBAT_MS );
    mp_advance_hurry_for_test( MP_HURRY_COMBAT_MS + 1, mp_hurry_source::combat );
    mp_tick_hurry();
    CHECK( mp_player_state_of( 1 ) == mp_player_state::not_waited );
    // Expiring combat disarms combat only; the press is still counting.
    CHECK( mp_hurry_remaining_ms() > MP_HURRY_COMBAT_MS );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_turn_reset_all();
}

TEST_CASE( "the_countdown_clears_with_the_turn", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    mp_turn_reset_all();

    mp_request_hurry();
    REQUIRE( mp_hurry_remaining_ms() > 0 );

    mp_turn_reset();
    CHECK( mp_hurry_remaining_ms() == -1 );
}

TEST_CASE( "a_drop_lasts_while_the_player_stays_at_the_same_input_wait", "[mp]" )
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

    g->player_at( 0 ).set_moves( 0 );
    b.set_moves( 100 );
    b.activity = player_activity();
    mp_set_waiting_for_input( 1, true );
    mp_request_hurry();
    mp_advance_hurry_for_test( MP_HURRY_MS + 1 );
    mp_tick_hurry();
    REQUIRE( mp_player_state_of( 1 ) == mp_player_state::not_waited );

    // Next turn opens. They are still in the same menu, so they stay dropped:
    // one press of "hurry up" that bought exactly one tick would mean hurrying
    // a partner again every turn for as long as they read, which is the phase
    // gate made unplayable by its own safety valve.
    mp_turn_reset();
    CHECK( mp_player_state_of( 1 ) == mp_player_state::not_waited );
    CHECK( mp_barrier_satisfied() );

    // They press a key and act: the backend clears the wait, and the next turn
    // waits for them again like anybody else.
    mp_set_waiting_for_input( 1, false );
    mp_turn_reset();
    CHECK( mp_player_state_of( 1 ) == mp_player_state::acting );
    CHECK_FALSE( mp_barrier_satisfied() );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_turn_reset_all();
}

TEST_CASE( "a_dropped_player_does_not_bank_the_moves_they_never_spent", "[mp]" )
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

    g->player_at( 0 ).set_moves( 0 );
    b.set_moves( 100 );
    b.activity = player_activity();
    mp_set_waiting_for_input( 1, true );
    mp_request_hurry();
    mp_advance_hurry_for_test( MP_HURRY_MS + 1 );
    mp_tick_hurry();
    REQUIRE( mp_player_state_of( 1 ) == mp_player_state::not_waited );
    // The drop itself takes nothing away: the menu is still open and the
    // character is untouched, exactly as the design promises.
    REQUIRE( b.get_moves() > 0 );

    // Closing the turn is where the standing still is paid for. Moves
    // accumulate, so without this the reader banks a turn's worth per tick.
    mp_forfeit_dropped_moves();
    CHECK( b.get_moves() == 0 );
    // Whoever is not dropped keeps whatever they have.
    CHECK( g->player_at( 0 ).get_moves() == 0 );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_turn_reset_all();
}

TEST_CASE( "menu_actions_are_free_while_waiting_and_costly_ones_are_not", "[mp]" )
{
    // The whitelist is inverted on purpose: anything not named here is refused
    // for a player who already acted, so a gap in the list is an annoyance
    // rather than a free action outside the turn.
    CHECK( mp_action_is_free( ACTION_INVENTORY ) );
    CHECK( mp_action_is_free( ACTION_PL_INFO ) );
    CHECK( mp_action_is_free( ACTION_MAP ) );
    CHECK( mp_action_is_free( ACTION_LOOK ) );
    CHECK( mp_action_is_free( ACTION_MESSAGES ) );
    CHECK( mp_action_is_free( ACTION_OPTIONS ) );
    CHECK( mp_action_is_free( ACTION_HELP ) );
    CHECK( mp_action_is_free( ACTION_ZOOM_IN ) );
    CHECK( mp_action_is_free( ACTION_ZOOM_OUT ) );
    CHECK( mp_action_is_free( ACTION_COOP_HURRY ) );

    CHECK_FALSE( mp_action_is_free( ACTION_MOVE_FORTH ) );
    CHECK_FALSE( mp_action_is_free( ACTION_PICKUP ) );
    CHECK_FALSE( mp_action_is_free( ACTION_FIRE ) );
    CHECK_FALSE( mp_action_is_free( ACTION_CRAFT ) );
    CHECK_FALSE( mp_action_is_free( ACTION_PAUSE ) );
}

TEST_CASE( "the_sidebar_says_who_is_being_waited_on", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &b = g->add_player_slot();
    b.set_body();
    b.name = "Bo";
    mp_sync_context_count();
    mp_turn_reset_all();

    avatar &a = g->player_at( 0 );
    a.name = "Ada";
    a.set_moves( 0 );
    a.activity = player_activity();
    b.set_moves( 100 );
    b.activity = player_activity();
    mp_set_waiting_for_input( 1, true );

    REQUIRE( mp_player_holds_barrier( 1 ) );
    // Ada is waiting on Bo, and has to be told so -- otherwise the only signal
    // that the game has not frozen is that it has not frozen.
    const std::string line = display::coop_player_text( a );
    CHECK( line.find( "Bo" ) != std::string::npos );

    mp_request_hurry();
    const std::string counting = display::coop_player_text( a );
    CHECK( counting.find( "Bo" ) != std::string::npos );
    // A countdown that is running but invisible is worse than none: the player
    // cannot tell whether pressing hurry did anything.
    CHECK( counting != line );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_turn_reset_all();
}

#endif // defined(MULTIPLAYER)
