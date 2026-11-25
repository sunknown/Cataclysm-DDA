#include "cata_catch.h"

#if defined(MULTIPLAYER)

#include "activity_actor_definitions.h"
#include "avatar.h"
#include "bodypart.h"
#include "calendar.h"
#include "debug.h"
#include "display.h"
#include "game.h"
#include "game_test_helper.h"
#include "map_helpers.h"
#include "map_helpers_tests.h"
#include "mp/mp_context.h"
#include "mp/mp_plan.h"
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
    // Menus: the key only opens the menu, so the spec makes it free in every
    // phase. What is confirmed inside the menu is a menu action, not this
    // gate's question.
    CHECK( mp_action_is_free( ACTION_DROP ) );
    CHECK( mp_action_is_free( ACTION_COMPARE ) );
    CHECK( mp_action_is_free( ACTION_EXAMINE ) );
    CHECK( mp_action_is_free( ACTION_CRAFT ) );
    CHECK( mp_action_is_free( ACTION_CONSTRUCT ) );
    CHECK( mp_action_is_free( ACTION_WIELD ) );
    CHECK( mp_action_is_free( ACTION_WEAR ) );
    CHECK( mp_action_is_free( ACTION_TAKE_OFF ) );
    CHECK( mp_action_is_free( ACTION_UNLOAD ) );
    CHECK( mp_action_is_free( ACTION_ITEMACTION ) );
    // The read menu opens first; the activity starts on its confirmed choice.
    CHECK( mp_action_is_free( ACTION_READ ) );

    // A key that spends time or starts a targeting flow of its own stays
    // refused, and so does the one menu-ish key with a no-menu path: a grazer
    // eats from the map without a menu, spending time.
    CHECK_FALSE( mp_action_is_free( ACTION_EAT ) );
    CHECK_FALSE( mp_action_is_free( ACTION_MOVE_FORTH ) );
    CHECK_FALSE( mp_action_is_free( ACTION_PICKUP ) );
    CHECK_FALSE( mp_action_is_free( ACTION_FIRE ) );
    CHECK_FALSE( mp_action_is_free( ACTION_THROW ) );
    CHECK_FALSE( mp_action_is_free( ACTION_PAUSE ) );
}

TEST_CASE( "the_away_toggle_is_free", "[mp]" )
{
    CHECK( mp_action_is_free( ACTION_COOP_AWAY ) );
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

TEST_CASE( "an_away_player_does_not_hold_the_barrier", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    mp_turn_reset_all();

    avatar &u = g->player_at( 0 );
    u.set_moves( 100 );
    u.activity = player_activity();
    mp_set_waiting_for_input( 0, true );
    REQUIRE( mp_player_state_of( 0 ) == mp_player_state::holding );

    mp_set_away( 0, true );

    CHECK( mp_player_state_of( 0 ) == mp_player_state::away );
    CHECK( mp_barrier_satisfied() );
}

TEST_CASE( "away_survives_the_turn_reset_and_dies_at_the_session_start", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    mp_turn_reset_all();

    mp_set_away( 0, true );
    mp_turn_reset();
    CHECK( mp_player_is_away( 0 ) );

    mp_turn_reset_all();
    CHECK_FALSE( mp_player_is_away( 0 ) );
}

TEST_CASE( "an_away_player_banks_no_moves", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    mp_turn_reset_all();

    avatar &u = g->player_at( 0 );
    u.set_moves( 100 );
    mp_set_away( 0, true );

    mp_forfeit_dropped_moves();
    CHECK( u.get_moves() == 0 );
}

TEST_CASE( "returning_from_away_makes_the_player_hold_again", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    mp_turn_reset_all();

    avatar &u = g->player_at( 0 );
    u.set_moves( 100 );
    mp_set_away( 0, true );
    mp_return_from_away( 0 );

    CHECK_FALSE( mp_player_is_away( 0 ) );
    CHECK( mp_player_state_of( 0 ) == mp_player_state::acting );
}

TEST_CASE( "a_disconnected_player_does_not_hold_the_barrier", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    mp_turn_reset_all();

    avatar &u = g->player_at( 0 );
    u.set_moves( 100 );
    u.activity = player_activity();
    mp_set_waiting_for_input( 0, true );
    REQUIRE( mp_player_state_of( 0 ) == mp_player_state::holding );

    mp_set_disconnected( 0, true );

    CHECK( mp_player_state_of( 0 ) == mp_player_state::disconnected );
    CHECK( mp_barrier_satisfied() );
    CHECK( mp_player_is_disconnected( 0 ) );
}

TEST_CASE( "disconnected_survives_the_turn_reset_and_dies_at_the_session_start", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    mp_turn_reset_all();

    mp_set_disconnected( 0, true );
    mp_turn_reset();
    CHECK( mp_player_is_disconnected( 0 ) );

    mp_turn_reset_all();
    CHECK_FALSE( mp_player_is_disconnected( 0 ) );
}

TEST_CASE( "a_disconnected_player_banks_no_moves", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    mp_turn_reset_all();

    avatar &u = g->player_at( 0 );
    u.set_moves( 100 );
    mp_set_disconnected( 0, true );

    mp_forfeit_dropped_moves();
    CHECK( u.get_moves() == 0 );
}

TEST_CASE( "the_turn_stays_open_while_every_living_player_is_away", "[mp]" )
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

    CHECK_FALSE( mp_turn_all_away() );
    mp_set_away( 0, true );
    CHECK_FALSE( mp_turn_all_away() );
    mp_set_away( 1, true );
    CHECK( mp_turn_all_away() );
    mp_set_away( 0, false );
    CHECK_FALSE( mp_turn_all_away() );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_turn_reset_all();
}

TEST_CASE( "the_turn_stays_open_when_every_living_player_is_away_or_disconnected", "[mp]" )
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

    mp_set_away( 0, true );
    mp_set_disconnected( 1, true );
    CHECK( mp_turn_all_away() );

    // A living player who is at the table, connected and not away, keeps the
    // turn open as before.
    mp_set_disconnected( 1, false );
    CHECK_FALSE( mp_turn_all_away() );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_turn_reset_all();
}

TEST_CASE( "a_hostile_in_sight_paces_a_turn_nobody_is_playing_by_hand", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    clear_map();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();
    mp_turn_reset_all();

    // Both busy with an activity, so nobody is playing by hand: without a
    // hostile that is exactly the free run the phase exists for.
    g->player_at( 0 ).assign_activity( wait_activity_actor( 10_minutes ) );
    b.assign_activity( wait_activity_actor( 10_minutes ) );
    mp_set_turn_open( true );
    CHECK_FALSE( mp_turn_paced() );

    mp_set_turn_open( false );
    spawn_test_monster( "mon_zombie", g->player_at( 0 ).pos_bub() + tripoint::east );
    mp_set_turn_open( true );
    CHECK( mp_turn_paced() );

    // Same rule with one player away instead of busy: the floor does not care
    // who is absent, only that a fight is on and nobody is playing by hand.
    mp_set_away( 1, true );
    b.activity = player_activity();
    CHECK( mp_turn_paced() );

    // A player back at the keyboard paces the world by themselves, hostile or
    // not: their actions are the clock.
    mp_set_away( 1, false );
    b.set_moves( 100 );
    mp_set_waiting_for_input( 1, true );
    CHECK_FALSE( mp_turn_paced() );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_turn_reset_all();
}

TEST_CASE( "an_away_player_is_not_a_left_behind_one", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    clear_map();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();
    mp_turn_reset_all();

    // Slot 0 is busy and never touches a key, so the only floor that could
    // appear is the one the left-behind rule would set.
    g->player_at( 0 ).assign_activity( wait_activity_actor( 10_minutes ) );
    // Slot 1 is dropped by a hurry while reading a menu...
    b.set_moves( 100 );
    b.activity = player_activity();
    mp_set_waiting_for_input( 1, true );
    mp_request_hurry();
    mp_advance_hurry_for_test( MP_HURRY_MS + 1 );
    mp_tick_hurry();
    REQUIRE( mp_player_left_behind( 1 ) );

    // ...and only then leaves the keyboard. The drop must go with them: a
    // countdown aimed at somebody who was still reading is not a countdown
    // aimed at an empty chair, or a busy partner pays MP_HURRY_MS per turn for
    // a player who is not there -- exactly what going away exists to stop.
    mp_set_away( 1, true );
    mp_set_turn_open( true );
    CHECK_FALSE( mp_turn_paced() );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_turn_reset_all();
}

TEST_CASE( "the_sidebar_names_who_is_away", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &b = g->add_player_slot();
    b.set_body();
    // Freshly constructed avatars carry no name, and every assertion below
    // compares against it. Name both slots so the checks cannot pass vacuously.
    g->player_at( 0 ).name = "Alice";
    b.name = "Bob";
    mp_sync_context_count();
    mp_turn_reset_all();

    const std::string away_name = g->player_at( 1 ).get_name();
    mp_set_away( 1, true );
    g->player_at( 0 ).set_moves( 100 );
    mp_set_waiting_for_input( 0, true );

    const std::string text = display::coop_player_text( g->player_at( 1 ) );

    // Both halves matter: the away player is reported, and is NOT reported as
    // somebody the world is waiting on.
    CHECK( text.find( "away: " + away_name ) != std::string::npos );
    CHECK( text.find( away_name + "," ) == std::string::npos );
    CHECK( text.find( "waiting: " + away_name ) == std::string::npos );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_turn_reset_all();
}

TEST_CASE( "the_hurry_lengths_can_be_set_for_a_session", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    mp_turn_reset_all();

    REQUIRE( mp_set_hurry_lengths( 3000, 1000 ) );
    mp_request_hurry();
    const int left = mp_hurry_remaining_ms( mp_hurry_source::manual );
    CHECK( left > 0 );
    CHECK( left <= 3000 );

    // The defaults come back with the session reset, not with the turn reset:
    // the lengths are the operator's, not the turn's.
    mp_turn_reset_all();
    mp_request_hurry();
    const int reset_left = mp_hurry_remaining_ms( mp_hurry_source::manual );
    CHECK( reset_left > 3000 );
    CHECK( reset_left <= MP_HURRY_MS );
}

TEST_CASE( "a_nonsense_hurry_length_is_refused", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    mp_turn_reset_all();

    CHECK_FALSE( mp_set_hurry_lengths( 0, 1000 ) );
    CHECK_FALSE( mp_set_hurry_lengths( 1000, -1 ) );
    CHECK_FALSE( mp_set_hurry_lengths( 1000, 600001 ) );
    // Each refusal above logs at D_ERROR on purpose, and the harness fails the
    // whole run on any observed error (test_main.cpp). Clear the flag so this
    // test does not fail its own shard.
    debug_reset_error_observed();
    mp_request_hurry();
    CHECK( mp_hurry_remaining_ms( mp_hurry_source::manual ) > 3000 );
}

// --- Planning rounds -------------------------------------------------------
//
// The round rule, pinned at the unit level: every seat that can still act has
// to commit or pass before the world resolves, and a seat that cannot act --
// no AP, an activity, a destination, away, disconnected, dead or passed -- does
// not hold the round. The tick around a resolved round (the world half, the
// moves handed back, the next tick) is covered end to end in
// mp_session_test.cpp, where a real map and threads exist.

namespace
{
// Two seats on the test map, both able to act, with no plan or turn state left
// over from an earlier case.
void set_up_two_planning_seats()
{
    clear_avatar();
    clear_map();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();
    mp_turn_reset_all();
    mp_plan_reset_all();
    for( size_t i = 0; i < g->player_slot_count(); ++i ) {
        g->player_at( i ).set_moves( 100 );
        g->player_at( i ).activity = player_activity();
    }
}

void tear_down_two_planning_seats()
{
    mp_plan_reset_all();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_turn_reset_all();
}

// Commits a plan the submission code does not check against the world. The
// round rule under test reads readiness, not what the plan would do, so a
// ranged intent -- accepted with any target -- is the cheapest committable one.
void commit_a_placeholder_plan( const size_t slot )
{
    mp_plan plan;
    plan.kind = mp_plan_kind::ranged;
    std::string err;
    REQUIRE( mp_plan_submit( slot, plan, err ) );
    REQUIRE( mp_plan_ready( slot, err ) );
    REQUIRE( mp_plan_is_ready( slot ) );
}
} // namespace

TEST_CASE( "a_plan_round_waits_for_a_seat_with_moves", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    set_up_two_planning_seats();

    // Both seats can act and nobody has committed: the world must wait. This is
    // the rule the whole WEGO turn rests on -- a present seat with AP is never
    // skipped for its partner's convenience.
    REQUIRE( mp_turn_player_can_act( 0 ) );
    REQUIRE( mp_turn_player_can_act( 1 ) );
    CHECK_FALSE( mp_turn_round_complete() );

    // One committed seat leaves the other holding the round.
    commit_a_placeholder_plan( 0 );
    CHECK_FALSE( mp_turn_round_complete() );

    // Every eligible seat committed: the round is complete.
    commit_a_placeholder_plan( 1 );
    CHECK( mp_turn_round_complete() );

    tear_down_two_planning_seats();
}

TEST_CASE( "a_seat_with_no_moves_or_activity_does_not_hold_the_round", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    set_up_two_planning_seats();
    commit_a_placeholder_plan( 0 );

    SECTION( "no moves" ) {
        g->player_at( 1 ).set_moves( 0 );
        CHECK_FALSE( mp_turn_player_can_act( 1 ) );
        CHECK( mp_turn_round_complete() );
    }
    SECTION( "an activity" ) {
        // Occupied, not thinking: the activity will spend the turn whether or
        // not the player touches a key.
        g->player_at( 1 ).activity = player_activity( activity_id( "ACT_HACKING" ), 500, 0 );
        CHECK_FALSE( mp_turn_player_can_act( 1 ) );
        CHECK( mp_turn_round_complete() );
    }
    SECTION( "a destination" ) {
        avatar &b = g->player_at( 1 );
        b.set_destination( { b.pos_bub() + tripoint_rel_ms::east }, player_activity() );
        REQUIRE( b.has_destination() );
        CHECK_FALSE( mp_turn_player_can_act( 1 ) );
        CHECK( mp_turn_round_complete() );
        b.clear_destination();
    }
    SECTION( "away" ) {
        mp_set_away( 1, true );
        CHECK_FALSE( mp_turn_player_can_act( 1 ) );
        CHECK( mp_turn_round_complete() );
        mp_set_away( 1, false );
    }
    SECTION( "disconnected" ) {
        mp_set_disconnected( 1, true );
        CHECK_FALSE( mp_turn_player_can_act( 1 ) );
        CHECK( mp_turn_round_complete() );
        mp_set_disconnected( 1, false );
    }
    SECTION( "dead" ) {
        g->player_at( 1 ).set_part_hp_cur( bodypart_id( "torso" ), 0 );
        REQUIRE( g->player_at( 1 ).is_dead_state() );
        CHECK_FALSE( mp_turn_player_can_act( 1 ) );
        CHECK( mp_turn_round_complete() );
    }
    SECTION( "passed" ) {
        mp_plan_pass( 1 );
        // Passing spends the AP. Give it back so the flag, not the empty pool,
        // is what this check is about.
        g->player_at( 1 ).set_moves( 100 );
        REQUIRE( mp_plan_is_passed( 1 ) );
        CHECK_FALSE( mp_turn_player_can_act( 1 ) );
        CHECK( mp_turn_round_complete() );
    }
    SECTION( "ready" ) {
        // The mirror case: a seat that committed still can act -- it simply
        // does not hold the round any more.
        commit_a_placeholder_plan( 1 );
        CHECK( mp_turn_player_can_act( 1 ) );
        CHECK( mp_turn_round_complete() );
    }

    tear_down_two_planning_seats();
}

TEST_CASE( "mp_turn_availability_says_why_a_seat_cannot_act", "[mp][ap_debt]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    set_up_two_planning_seats();

    // One source of truth for the barrier, the resolver and the party
    // snapshot: `available` is the only state that may start a new action.
    CHECK( mp_turn_player_availability( 0 ) == mp_action_availability::available );
    CHECK( mp_turn_player_availability( 1 ) == mp_action_availability::available );
    // No such seat.
    CHECK( mp_turn_player_availability( 2 ) == mp_action_availability::unavailable );

    SECTION( "an empty pool" ) {
        g->player_at( 1 ).set_moves( 0 );
        CHECK( mp_turn_player_availability( 1 ) == mp_action_availability::recovering );
    }
    SECTION( "a debt" ) {
        g->player_at( 1 ).set_moves( -150 );
        CHECK( mp_turn_player_availability( 1 ) == mp_action_availability::recovering );
    }
    SECTION( "an activity" ) {
        g->player_at( 1 ).activity = player_activity( activity_id( "ACT_HACKING" ), 500, 0 );
        CHECK( mp_turn_player_availability( 1 ) == mp_action_availability::busy );
    }
    SECTION( "a destination" ) {
        avatar &b = g->player_at( 1 );
        b.set_destination( { b.pos_bub() + tripoint_rel_ms::east }, player_activity() );
        REQUIRE( b.has_destination() );
        CHECK( mp_turn_player_availability( 1 ) == mp_action_availability::busy );
        b.clear_destination();
    }
    SECTION( "away" ) {
        mp_set_away( 1, true );
        CHECK( mp_turn_player_availability( 1 ) == mp_action_availability::away );
        mp_set_away( 1, false );
    }
    SECTION( "disconnected" ) {
        mp_set_disconnected( 1, true );
        CHECK( mp_turn_player_availability( 1 ) == mp_action_availability::disconnected );
        mp_set_disconnected( 1, false );
    }
    SECTION( "dead" ) {
        g->player_at( 1 ).set_part_hp_cur( bodypart_id( "torso" ), 0 );
        REQUIRE( g->player_at( 1 ).is_dead_state() );
        CHECK( mp_turn_player_availability( 1 ) == mp_action_availability::dead );
    }
    SECTION( "passed" ) {
        mp_plan_pass( 1 );
        // Passing spends the AP. Give it back so the flag, not the empty pool,
        // is what this state is about.
        g->player_at( 1 ).set_moves( 100 );
        REQUIRE( mp_plan_is_passed( 1 ) );
        CHECK( mp_turn_player_availability( 1 ) == mp_action_availability::passed );
    }
    SECTION( "available is the only state that may act" ) {
        g->player_at( 1 ).set_moves( 0 );
        CHECK_FALSE( mp_turn_player_can_act( 1 ) );
        g->player_at( 1 ).set_moves( 1 );
        CHECK( mp_turn_player_can_act( 1 ) );
    }

    tear_down_two_planning_seats();
}

TEST_CASE( "a_debt_is_paid_back_by_ticks", "[mp][ap_debt]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    set_up_two_planning_seats();

    avatar &a = g->player_at( 0 );
    avatar &b = g->player_at( 1 );
    a.set_moves( -150 );
    b.set_moves( 0 );

    // An empty pool or a debt is a recovery state, not a decision: nobody
    // holds the round and no manual pass is needed.
    CHECK_FALSE( mp_turn_player_can_act( 0 ) );
    CHECK_FALSE( mp_turn_player_can_act( 1 ) );
    CHECK( mp_turn_round_complete() );

    // The real tick tail hands the moves back, exactly once per tick.
    mp_turn_close_tick();
    CHECK( a.get_moves() == -150 + a.get_speed() );
    CHECK( b.get_moves() == 0 + b.get_speed() );
    CHECK( mp_turn_player_can_act( 1 ) );

    mp_turn_close_tick();
    CHECK( a.get_moves() == -50 + a.get_speed() );
    CHECK( a.get_moves() == 50 );
    CHECK( mp_turn_player_can_act( 0 ) );

    tear_down_two_planning_seats();
}

TEST_CASE( "a_new_tick_clears_the_pass", "[mp][ap_debt]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    set_up_two_planning_seats();

    g->player_at( 0 ).set_moves( 120 );
    mp_plan_pass( 0 );
    REQUIRE( mp_plan_is_passed( 0 ) );

    mp_turn_open_tick();

    // The pass ends with the tick that took it. AP is granted by the tick's
    // tail, not by the opening, so the pool is still empty here -- what
    // changed is that the seat may plan again once the next tail pays it.
    CHECK_FALSE( mp_plan_is_passed( 0 ) );
    CHECK( mp_turn_is_open() );
    CHECK( g->player_at( 0 ).get_moves() == 0 );

    tear_down_two_planning_seats();
}

TEST_CASE( "an_absent_seat_forfeits_only_the_positive_part", "[mp][ap_debt]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    set_up_two_planning_seats();

    avatar &a = g->player_at( 0 );
    avatar &b = g->player_at( 1 );
    mp_set_away( 0, true );
    mp_set_disconnected( 1, true );

    SECTION( "a positive bank is burned" ) {
        a.set_moves( 120 );
        b.set_moves( 80 );
        mp_forfeit_dropped_moves();
        CHECK( a.get_moves() == 0 );
        CHECK( b.get_moves() == 0 );
    }
    SECTION( "a debt survives" ) {
        a.set_moves( -150 );
        b.set_moves( -20 );
        mp_forfeit_dropped_moves();
        // An absence is not a way to have the debt forgiven.
        CHECK( a.get_moves() == -150 );
        CHECK( b.get_moves() == -20 );
    }
    SECTION( "a present seat keeps its pool" ) {
        mp_set_away( 0, false );
        a.set_moves( 120 );
        mp_forfeit_dropped_moves();
        CHECK( a.get_moves() == 120 );
    }

    mp_set_away( 0, false );
    mp_set_disconnected( 1, false );
    tear_down_two_planning_seats();
}

TEST_CASE( "the_tick_tail_forfeits_an_absent_seat_s_bank", "[mp][ap_debt]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    set_up_two_planning_seats();

    avatar &a = g->player_at( 0 );
    mp_set_away( 0, true );

    // The bank is burned before the tick's own grant, so an absence cannot
    // accumulate moves: one tick's worth, never several.
    a.set_moves( 120 );
    mp_turn_close_tick();
    CHECK( a.get_moves() == a.get_speed() );

    // A debt is not forgiven either: one grant pays part of it.
    a.set_moves( -150 );
    mp_turn_close_tick();
    CHECK( a.get_moves() == -150 + a.get_speed() );
    CHECK( a.get_moves() == -50 );

    mp_set_away( 0, false );
    tear_down_two_planning_seats();
}

TEST_CASE( "an_absent_seat_s_activity_still_gets_the_tick", "[mp][ap_debt]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    set_up_two_planning_seats();

    avatar &a = g->player_at( 0 );
    a.activity = player_activity( activity_id( "ACT_HACKING" ), 500, 0 );
    mp_set_away( 0, true );
    a.set_moves( 0 );

    mp_turn_close_tick();

    // The body keeps working while its owner is away: the tick grants its
    // budget and leaves the activity alone.
    CHECK( a.get_moves() == a.get_speed() );
    CHECK_FALSE( a.activity.is_null() );

    mp_set_away( 0, false );
    tear_down_two_planning_seats();
}

TEST_CASE( "going_away_drops_the_unapplied_plan", "[mp][ap_debt]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    set_up_two_planning_seats();

    commit_a_placeholder_plan( 0 );
    REQUIRE( mp_plan_is_ready( 0 ) );

    mp_set_away( 0, true );

    // A committed plan belongs to a player who is still at the table.
    CHECK( mp_plan_of( 0 ).kind == mp_plan_kind::none );
    CHECK_FALSE( mp_plan_is_ready( 0 ) );

    mp_set_away( 0, false );
    tear_down_two_planning_seats();
}

TEST_CASE( "a_returning_seat_keeps_its_balance", "[mp][ap_debt]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    set_up_two_planning_seats();

    avatar &a = g->player_at( 0 );
    a.set_moves( -150 );

    // Neither leaving nor coming back normalises the pool: a debt is the
    // seat's, and a positive remainder is only burned at the tick's tail.
    mp_set_away( 0, true );
    CHECK( a.get_moves() == -150 );
    mp_set_away( 0, false );
    CHECK( a.get_moves() == -150 );
    mp_set_disconnected( 0, true );
    CHECK( a.get_moves() == -150 );
    mp_set_disconnected( 0, false );
    CHECK( a.get_moves() == -150 );

    tear_down_two_planning_seats();
}

TEST_CASE( "a_session_seed_keeps_a_debt_and_drops_a_bank", "[mp][ap_debt]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    set_up_two_planning_seats();

    avatar &a = g->player_at( 0 );

    // A character created just now has not played a turn: it owes nothing,
    // whatever its body carried in.
    a.set_moves( 120 );
    mp_seed_session_moves( 0, /*is_new=*/true );
    CHECK( a.get_moves() == 0 );

    // A loaded character keeps its debt and loses only the positive bank.
    a.set_moves( -150 );
    mp_seed_session_moves( 0, false );
    CHECK( a.get_moves() == -150 );
    a.set_moves( 120 );
    mp_seed_session_moves( 0, false );
    CHECK( a.get_moves() == 0 );
    a.set_moves( 0 );
    mp_seed_session_moves( 0, false );
    CHECK( a.get_moves() == 0 );

    // No such seat: nothing to seed, nothing to crash on.
    mp_seed_session_moves( 2, false );

    tear_down_two_planning_seats();
}

#endif // defined(MULTIPLAYER)
