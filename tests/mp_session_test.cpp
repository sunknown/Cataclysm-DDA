#if defined(MP_SERVER)

#include "cata_catch.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "calendar.h"
#include "cata_scope_helpers.h"

#include "activity_actor_definitions.h"
#include "avatar.h"
#include "bodypart.h"
#include "debug.h"
#include "game.h"
#include "game_test_helper.h"
#include "input.h"
#include "mp_session_harness.h"
#include "map.h"
#include "map_helpers.h"
#include "messages.h"
#include "output.h"
#include "mp/mp_context.h"
#include "mp/mp_loopback_transport.h"
#include "mp/mp_protocol.h"
#include "mp/mp_server_main.h"
#include "mp/mp_session.h"
#include "mp/mp_turn.h"
#include "overmapbuffer.h"
#include "overmap.h"
#include "point.h"
#include "string_formatter.h"
#include "type_id.h"
#include "ui_manager.h"
#include "units.h"

static const vproto_id vehicle_prototype_test_shopping_cart( "test_shopping_cart" );

TEST_CASE( "a_server_debugmsg_reaches_every_player", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();

    // In test_mode debugmsg is already non-blocking, so "does it block" is not
    // observable from a test. What IS observable is the behaviour that replaces
    // the modal window: the error must land in the message log. A build that
    // still opened a window would put it nowhere.
    //
    // The log is process-wide (src/messages.h has no per-player split), so one
    // add_msg reaches everybody and a second would be a duplicate. Per-player
    // message logs are not part of this phase.
    Messages::clear_messages();
    mp_activate_world();
    debugmsg( "a server debugmsg must reach the players" );
    // The call above is intentional, but a bare debugmsg logs at D_ERROR and
    // the harness fails the whole run on any observed error (test_main.cpp).
    // Clear the flag so this test does not fail its own shard.
    debug_reset_error_observed();

    const std::vector<std::pair<std::string, std::string>> log = Messages::recent_messages( 5 );
    int hits = 0;
    for( const auto &entry : log ) {
        hits += entry.second.find( "must reach the players" ) != std::string::npos ? 1 : 0;
    }
    CHECK( hits == 1 );

    mp_activate_player( 0 );
    Messages::clear_messages();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

// The regression guard for the render pipeline itself. The two tests below it
// would notice if the pipeline stopped reaching the grid, but each can fail for
// reasons that are not the pipeline's -- the menus' own teardown, and the hurry
// flag -- so neither can be relied on to report it. This one asserts the thing
// those two only imply.
//
// It also covers the teardown of a single player left inside a menu, which was
// an abort before mp_sched's unwind right (mp_sched.cpp's release_to assert):
// a thread parked INSIDE player_act() wakes on `stopping_` without the baton,
// and its thread body used to release a baton it did not hold. One parked-in-
// menu thread was enough -- no concurrency involved. Do not "fix" a failure
// here by closing the menu with a keystroke first: that dodges the very case
// this leaves open at teardown.
TEST_CASE( "one_players_menu_reaches_their_own_client_screen", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 1 );

    // The harness constructor zeroes every slot's moves, which reads as "already
    // acted": the barrier is satisfied before anyone is handed the baton, so no
    // player_act runs, the key is never read and no menu opens. Give the moves
    // back so this player is actionable this turn.
    g->player_at( 0 ).set_moves( 100 );
    // '@' opens the character sheet, which carries the character's own name.
    // Nothing else on this screen does -- the sidebar of the main screen behind
    // it shows no name at this terminal size. So the name proves the claim
    // the phase rests on: content specific to this player travelled the whole
    // pipeline to this player's client grid. A pipeline that discards writes
    // cannot produce it, and one that carried some other player's screen would
    // not carry this name.
    harness.press( 0, '@' );
    REQUIRE( harness.run_until( [&]() {
        return mp_grid_contains( harness.screen( 0 ), "P0" );
    } ) );

    const mp_screen_grid &screen = harness.screen( 0 );
    CHECK( mp_grid_contains( screen, "P0" ) );
    // A rendered character sheet, not one stray cell. It measured 944 non-blank
    // cells when the pipeline was first made to work at this terminal size; the
    // floor is set far below that so ordinary UI churn does not trip it, and far
    // above what any partial write could reach.
    CHECK( mp_grid_nonblank_cells( screen ) > 200 );
}

TEST_CASE( "every_player_sees_their_own_main_game_screen", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );

    // Both actionable, no key pressed: each player sits on the main screen
    // waiting for input, which is what a client sees for most of a session.
    // The sidebar does not carry the character's name at this size, but it
    // does carry their strength, so give the two players far-apart ones. The
    // text is read off the character rather than written here: the sidebar
    // shows strength after weight and other penalties.
    g->player_at( 0 ).set_moves( 100 );
    g->player_at( 1 ).set_moves( 100 );
    g->player_at( 1 ).set_str_base( 20 );
    const auto strength = []( const size_t slot ) {
        return string_format( "Str: %d ", g->player_at( slot ).get_str() );
    };
    REQUIRE( harness.run_until( [&]() {
        return mp_grid_contains( harness.screen( 0 ), strength( 0 ) ) &&
               mp_grid_contains( harness.screen( 1 ), strength( 1 ) );
    }, 10 ) );
    REQUIRE( strength( 0 ) != strength( 1 ) );
    // A whole screen, not a stray line: the map and the sidebar drew about 500
    // cells each when this was first made to work at the harness's 80x24.
    CHECK( mp_grid_nonblank_cells( harness.screen( 0 ) ) > 200 );
    CHECK( mp_grid_nonblank_cells( harness.screen( 1 ) ) > 200 );
    // And each player's own: the other's sidebar never lands on it.
    CHECK_FALSE( mp_grid_contains( harness.screen( 0 ), strength( 1 ) ) );
    CHECK_FALSE( mp_grid_contains( harness.screen( 1 ), strength( 0 ) ) );
}

namespace
{
// One of the heavy menus: the key that opens it from the main game screen,
// and text that appears on its screen and on no other menu's here. The texts
// were read off the menus as this harness renders them, at its 80x24.
struct mp_heavy_menu {
    int key;
    const char *marker;
};

const mp_heavy_menu mp_menu_inventory{ 'i', "Item hotkeys assigned" };
const mp_heavy_menu mp_menu_character_sheet{ '@', "ENCUMBRANCE" };
const mp_heavy_menu mp_menu_crafting{ '&', "Craft, D[e]scribe" };
const mp_heavy_menu mp_menu_map{ 'm', "Quick Reference" };
// Examining the one vehicle next to the player auto-selects it and opens the
// vehicle's action menu (a uilist, so this is also the case that has to unwind
// through uilist's own cancel at teardown).
const mp_heavy_menu mp_menu_vehicle{ 'e', "Examine vehicle" };
const mp_heavy_menu mp_menu_construction{ '*', "Construction" };
} // namespace

// The six menus the design names as the heavy ones (inventory, crafting, map,
// character sheet, vehicle, construction): each is a different UI path, and a
// global leaked by any of them shows up as one player seeing the other's
// screen. The keys are the DEFAULTMODE defaults.
TEST_CASE( "two_players_sit_in_different_heavy_menus", "[mp]" )
{
    restore_active_state restore;
    struct menu_case {
        const char *name;
        mp_heavy_menu first;
        mp_heavy_menu second;
    };
    const std::vector<menu_case> menus = {
        { "inventory vs character sheet", mp_menu_inventory, mp_menu_character_sheet },
        { "crafting vs map", mp_menu_crafting, mp_menu_map },
        { "vehicle vs construction", mp_menu_vehicle, mp_menu_construction },
    };

    for( const menu_case &c : menus ) {
        CAPTURE( c.name );
        // The cart outlives the harness; nothing after this test expects it.
        on_out_of_scope remove_vehicles( []() {
            clear_vehicles();
        } );
        mp_session_harness harness( 2 );
        if( c.first.key == mp_menu_vehicle.key ) {
            // Something to examine: without a vehicle in reach the key opens
            // nothing, and the case would compare a menu with a blank screen.
            REQUIRE( get_map().add_vehicle( vehicle_prototype_test_shopping_cart,
                                            g->player_at( 0 ).pos_bub() + point::east,
                                            0_degrees ) != nullptr );
        }

        // The harness zeroes every slot's moves, which reads as "already acted":
        // the barrier would close before either player is handed the baton, and
        // no key would be read. Give both their moves back so both are
        // actionable. Neither key costs time, so both players hold nothing and
        // both stay live -- which is the whole point of the per-player context.
        g->player_at( 0 ).set_moves( 100 );
        g->player_at( 1 ).set_moves( 100 );
        harness.press( 0, c.first.key );
        harness.press( 1, c.second.key );
        REQUIRE( harness.run_until( [&]() {
            return mp_grid_contains( harness.screen( 0 ), c.first.marker ) &&
                   mp_grid_contains( harness.screen( 1 ), c.second.marker );
        } ) );

        // Each player sees their own menu, and nothing of the other's. A
        // leaked global shows one player the other's screen -- the main class
        // of bug this design can have -- and so does a draw that lands in the
        // wrong session's grid.
        CHECK_FALSE( mp_grid_contains( harness.screen( 0 ), c.second.marker ) );
        CHECK_FALSE( mp_grid_contains( harness.screen( 1 ), c.first.marker ) );
    }
}

TEST_CASE( "a_player_in_a_menu_does_not_freeze_their_partner", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );

    // Player 1 parks in a menu with moves left: they hold the barrier. The
    // world may not outrun a thinking player. Fresh avatars start with zero
    // moves, so the first do_turn is a full, closed turn in which neither
    // player_act reads any input; the calendar advances when the SECOND do_turn
    // opens a turn and player 1 parks in the menu. Every do_turn after that is
    // a waiting turn that returns without advancing -- ten of them leave the
    // calendar exactly one turn past turn_before.
    harness.press( 1, mp_menu_inventory.key );
    const int turn_before = to_turn<int>( calendar::turn );
    REQUIRE( harness.run_until( []() {
        return false;
    }, 10 ) == false );
    CHECK( to_turn<int>( calendar::turn ) == turn_before + 1 );
    CHECK( mp_grid_contains( harness.screen( 1 ), mp_menu_inventory.marker ) );

    // Player 0 hurries them. The key reaches the server through the wire, so
    // wait for the press itself to be registered -- the manual countdown
    // specifically: the shorter combat one arms on its own whenever a hostile
    // can see a player, and would make this pass with nobody hurrying. The
    // harness clears the map's creatures, so none should be armed.
    harness.press( 0, '`' );
    REQUIRE( harness.run_until( []() {
        return mp_hurry_remaining_ms( mp_hurry_source::manual ) > 0;
    } ) );
    CHECK( mp_hurry_remaining_ms( mp_hurry_source::combat ) < 0 );
    // The press asks; it does not move the world by itself.
    CHECK( to_turn<int>( calendar::turn ) == turn_before + 1 );

    // Expire it. It drops everyone still holding -- player 0 too, who pressed
    // hurry from the map and so still held the turn. Player 0 plays on: they
    // wait a turn, and the world moves on past the frozen turn -- `+ 1` is
    // where the first phase already left it -- while the partner's menu is
    // still open: untouched, just not waited on.
    mp_advance_hurry_for_test( MP_HURRY_MS + 1, mp_hurry_source::manual );
    harness.press( 0, '.' );
    CHECK( harness.run_until( [turn_before]() {
        return to_turn<int>( calendar::turn ) > turn_before + 1;
    } ) );
    CHECK( mp_grid_contains( harness.screen( 1 ), mp_menu_inventory.marker ) );
}

TEST_CASE( "a_yes_no_prompt_unwinds_when_the_session_ends", "[mp]" )
{
    restore_active_state restore;
    {
        mp_session_harness harness( 2 );
        g->player_at( 0 ).set_moves( 100 );
        // 'S' asks "Save and quit?" through query_yn, the prompt most
        // confirmations in the game share. Its loop asks again on every
        // timeout, and a session ending answers a waiting player with one.
        harness.press( 0, 'S' );
        REQUIRE( harness.run_until( [&]() {
            return mp_grid_contains( harness.screen( 0 ), "Save and quit" );
        } ) );
    }
    // Reaching the end of this test is the assertion: the harness's destructor
    // joins every player thread, and a prompt that never closes holds that join
    // for ever.
}

TEST_CASE( "a_question_the_world_asks_is_answered_without_waiting", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    harness.run_until( []() {
        return false;
    }, 1 );
    // The test thread is the world and holds the baton between turns. Enter
    // the world phase as do_turn() does for world_turn_post(), where an NPC
    // walking up to talk, a monster's question or the death screen asks from.
    // There is no player of the world to answer: every player's input belongs
    // to their own thread, and waiting on one would hold the baton while
    // asking them for it.
    mp_activate_world();
    CHECK_FALSE( query_yn( "A question from the world?" ) );
    mp_activate_player( 0 );
    // The world still runs turns afterwards.
    const int turn_before = to_turn<int>( calendar::turn );
    CHECK( harness.run_until( [turn_before]() {
        return to_turn<int>( calendar::turn ) > turn_before;
    } ) );
}
TEST_CASE( "the_main_screen_outlives_the_end_of_a_session", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    g->add_player_slot().set_body();
    mp_sync_context_count();
    // main() raises context 0's main screen before the session starts and
    // drops it at the end of the same loop iteration, after the session ends.
    shared_ptr_fast<ui_adaptor> ui = g->create_or_get_main_ui_adaptor();

    mp_server_end_session();
    // ~ui_adaptor asserts it dies under the stack it registered in.
    ui.reset();

    // And the menu loop main() goes back to finds the process as it was
    // before the session: player 0's context and stack.
    CHECK( ui_manager::get_active_stack() == &g->contexts[0].ui_stack );
    CHECK( mp_active_context_ptr() == &g->contexts[0] );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}
TEST_CASE( "a_player_left_behind_by_a_hurry_does_not_let_the_world_run_free", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 1 );
    g->player_at( 0 ).set_moves( 100 );
    // The player sits at the map and presses hurry, which costs nothing: they
    // are still holding the turn when the countdown runs out, so it drops them.
    // So does a hostile's countdown with nobody pressing anything.
    harness.press( 0, '`' );
    REQUIRE( harness.run_until( []() {
        return mp_hurry_remaining_ms( mp_hurry_source::manual ) > 0;
    } ) );
    mp_advance_hurry_for_test( MP_HURRY_MS + 1, mp_hurry_source::manual );
    const int turn_before = to_turn<int>( calendar::turn );

    // Twenty polls take about a second. The drop lasts while the player sits
    // at the same input wait, so with nobody else to wait for, every turn
    // would close the moment it opened and the world would run at CPU speed
    // around a player who is only thinking.
    harness.run_until( []() {
        return false;
    }, 20 );
    CHECK( to_turn<int>( calendar::turn ) <= turn_before + 1 );
}

TEST_CASE( "a_dropped_player_keeps_playing_while_their_partner_holds", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    // Player 1 reads their inventory; player 0 has acted (fresh avatars have
    // no moves). Hurrying drops player 1 for as long as they read.
    g->player_at( 1 ).set_moves( 100 );
    harness.press( 1, mp_menu_inventory.key );
    REQUIRE( harness.run_until( [&]() {
        return mp_grid_contains( harness.screen( 1 ), mp_menu_inventory.marker );
    } ) );
    mp_request_hurry();
    mp_advance_hurry_for_test( MP_HURRY_MS + 1, mp_hurry_source::manual );
    const int turn_before = to_turn<int>( calendar::turn );
    REQUIRE( harness.run_until( [turn_before]() {
        return to_turn<int>( calendar::turn ) > turn_before;
    } ) );

    // Next turn player 0 has moves and sits at the map: now they hold the
    // turn. Player 1 closes their inventory. Their key has to be read even
    // though the world is waiting on somebody else.
    harness.press( 1, KEY_ESCAPE );
    CHECK( harness.run_until( [&]() {
        return !mp_grid_contains( harness.screen( 1 ), mp_menu_inventory.marker );
    }, 20 ) );
}
TEST_CASE( "every_client_is_told_why_the_session_ended", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_sessions_reset_for_test();
    mp_turn_reset_all();
    const mp_loopback_pair wire = mp_make_loopback_pair();
    mp_sessions().push_back( std::make_unique<mp_session>() );
    mp_sessions().back()->transport = wire.a;

    mp_request_session_end( "Bo disconnected" );
    mp_server_end_session();

    // The client's end of the wire: everything the server said last.
    std::optional<std::string> reason;
    while( std::optional<std::vector<uint8_t>> payload = wire.b->recv( 100 ) ) {
        mp_msg_kind kind = mp_msg_kind::screen;
        std::vector<uint8_t> body;
        mp_control msg;
        if( mp_unwrap( *payload, kind, body ) && kind == mp_msg_kind::control &&
            mp_control_from_json( std::string( body.begin(), body.end() ), msg ) &&
            msg.kind == mp_control_kind::bye ) {
            reason = msg.bye_reason;
        }
    }
    CHECK( reason == std::optional<std::string>( "Bo disconnected" ) );
    mp_turn_reset_all();
}

TEST_CASE( "a_death_in_a_session_ends_it_and_saves_the_world", "[mp]" )
{
    restore_active_state restore;
    // Slot 1 first: its death used to go unnoticed, which fails a check. Slot
    // 0's used to reach the death screens from the world thread and abort.
    const size_t victim = GENERATE( 1, 0 );
    CAPTURE( victim );
    mp_session_harness harness( 2 );
    harness.run_until( []() {
        return false;
    }, 1 );
    // By name, not by reference: activating a player swaps avatars' contents
    // between the slot objects, so a reference names somebody else afterwards.
    avatar &dead = g->player_at( victim );
    const std::string name = dead.get_name();
    dead.set_part_hp_cur( bodypart_id( "torso" ), 0 );
    REQUIRE( dead.is_dead_state() );

    // The death screens, the deathcam question and the world-end menu are
    // all a single player's. A session asks none of them: it saves the world
    // as it stands -- the dead character with it, their death handled when
    // the world is next loaded alone -- and ends for everyone, saying why.
    CHECK( g->do_turn() );
    CHECK( mp_session_end_reason() == name + " died" );
    CHECK( g->uquit == QUIT_SAVED );

    // The ending's own teardown (turn_handler::cleanup_at_end) empties the map
    // buffer and the overmaps, leaving the map pointing at freed submaps, as
    // main() expects: it goes back to the menu and loads a world. A test
    // process goes on to the next test instead, so put back the world
    // tests/test_main.cpp starts every run with.
    get_map() = map();
    overmap_special_batch empty_specials( point_abs_om{} );
    overmap_buffer.create_custom_overmap( point_abs_om{}, empty_specials );
    get_map().load( tripoint_abs_sm( get_map().get_abs_sub() ), false );
    get_avatar().move_to( tripoint_abs_ms::zero );
}

TEST_CASE( "a_session_where_everyone_is_away_does_not_advance_the_world", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    mp_set_away( 0, true );
    mp_set_away( 1, true );
    const int turn_before = to_turn<int>( calendar::turn );

    // Ten polls take about a second. With every living player away there is
    // nobody to wait for and nobody to pace: the turn must not close at all.
    CHECK_FALSE( harness.run_until( []() {
        return false;
    }, 10 ) );
    CHECK( to_turn<int>( calendar::turn ) == turn_before );

    // One comes back and waits out a turn; the world moves again.
    mp_set_away( 0, false );
    g->player_at( 0 ).set_moves( 100 );
    harness.press( 0, '.' );
    CHECK( harness.run_until( [turn_before]() {
        return to_turn<int>( calendar::turn ) > turn_before;
    } ) );
}

TEST_CASE( "an_away_player_does_not_slow_a_busy_partner", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    g->player_at( 1 ).assign_activity( wait_activity_actor( 10_minutes ) );
    mp_set_away( 0, true );
    const int turn_before = to_turn<int>( calendar::turn );

    // The away player is not a left-behind one: they cost the partner nothing
    // and, alone, nothing paces the world either.
    CHECK( harness.run_until( [turn_before]() {
        return to_turn<int>( calendar::turn ) > turn_before + 20;
    }, 400 ) );
}

TEST_CASE( "two_busy_players_do_not_hold_the_world", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );

    // The phase's criterion, pinned: a long activity is a player telling the
    // game what to do, and the world is free to run while both do it.
    g->player_at( 0 ).assign_activity( wait_activity_actor( 10_minutes ) );
    g->player_at( 1 ).assign_activity( wait_activity_actor( 10_minutes ) );
    const int turn_before = to_turn<int>( calendar::turn );

    CHECK( harness.run_until( [turn_before]() {
        return to_turn<int>( calendar::turn ) > turn_before + 20;
    }, 400 ) );
}

TEST_CASE( "an_away_players_activity_advances_while_a_partner_plays", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );

    // The partner's own long activity keeps the world moving without waiting
    // on a key from either player. The away player's activity must spend the
    // moves each new turn hands them, not sit frozen on their input wait.
    avatar &away = g->player_at( 0 );
    away.assign_activity( wait_activity_actor( 30_minutes ) );
    mp_set_away( 0, true );
    g->player_at( 1 ).assign_activity( wait_activity_actor( 30_minutes ) );

    // Let the opening turns settle. The away player's own action loop spends
    // the moves they had when they left, so progress before this point does
    // not prove that the running world kept their activity going.
    harness.run_until( []() { return false; }, 3 );
    const int moves_before = away.activity.moves_left;
    const int turn_before = to_turn<int>( calendar::turn );

    CHECK( harness.run_until( [&]() {
        return to_turn<int>( calendar::turn ) > turn_before + 5 &&
               away.activity.moves_left < moves_before - 100;
    }, 100 ) );
}

TEST_CASE( "an_away_player_carrying_an_activity_can_still_come_back", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );

    // A long activity spends the player's moves before game::player_act()
    // reaches its input wait, so neither player sets the input-wait flag on
    // its own. Mark both away through the same state the ':' key sets; then
    // give their activities a turn to drain.
    for( size_t i = 0; i < 2; ++i ) {
        g->player_at( i ).assign_activity( wait_activity_actor( 10_minutes ) );
        mp_set_away( i, true );
    }
    harness.run_until( []() { return false; }, 3 );

    const int turn_before = to_turn<int>( calendar::turn );
    // Every living player is away: the turn stays open and the world stands
    // still, although neither player has got to an input wait of their own.
    CHECK_FALSE( harness.run_until( []() { return false; }, 10 ) );
    CHECK( to_turn<int>( calendar::turn ) == turn_before );

    // ':' is the documented way back. The key is read on this player's own
    // input loop, and the world must move again once they return.
    harness.press( 0, ':' );
    CHECK( harness.run_until( [turn_before]() {
        return to_turn<int>( calendar::turn ) > turn_before;
    }, 20 ) );
}

TEST_CASE( "an_away_player_at_an_input_wait_still_gets_the_baton", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    // Let both threads reach their input waits -- the first turn only hands
    // moves back, so run until both are parked holding the barrier. The key
    // below is then one a parked player really holds, with the turn already
    // open, so the calendar below cannot move for the wrong reason.
    REQUIRE( harness.run_until( []() {
        return mp_player_state_of( 0 ) == mp_player_state::holding &&
               mp_player_state_of( 1 ) == mp_player_state::holding;
    } ) );
    mp_set_away( 0, true );
    mp_set_away( 1, true );
    const int turn_before = to_turn<int>( calendar::turn );

    // Nobody owes the turn and nobody paces it: the world holds it open.
    CHECK_FALSE( harness.run_until( []() {
        return false;
    }, 10 ) );
    CHECK( to_turn<int>( calendar::turn ) == turn_before );

    // Going away is not abandoning the terminal. The away player's key has to
    // reach the game even though the world is frozen -- otherwise it is read
    // off the wire and never dispatched, and the only way back from "away" is
    // a key that never arrives.
    harness.press( 0, mp_menu_inventory.key );
    REQUIRE( harness.run_until( [&]() {
        return mp_grid_contains( harness.screen( 0 ), mp_menu_inventory.marker );
    } ) );

    harness.press( 0, KEY_ESCAPE );
    REQUIRE( harness.run_until( [&]() {
        return !mp_grid_contains( harness.screen( 0 ), mp_menu_inventory.marker );
    } ) );

    // Reading a menu while away is still not playing: the world has not moved.
    CHECK( to_turn<int>( calendar::turn ) == turn_before );
}

TEST_CASE( "the_away_key_toggles_and_a_costly_action_brings_a_player_back", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    g->player_at( 0 ).set_moves( 100 );

    harness.press( 0, ':' );
    REQUIRE( harness.run_until( []() {
        return mp_player_is_away( 0 );
    } ) );

    // The key is a toggle: a second press brings the player back, and a third
    // sends them away again. Both must work before any costly action is taken.
    harness.press( 0, ':' );
    REQUIRE( harness.run_until( []() {
        return !mp_player_is_away( 0 );
    } ) );
    harness.press( 0, ':' );
    REQUIRE( harness.run_until( []() {
        return mp_player_is_away( 0 );
    } ) );
    // Reading is not coming back: a free action must not put a player who is
    // away back into the barrier, or the partner freezes while they read.
    harness.press( 0, mp_menu_inventory.key );
    REQUIRE( harness.run_until( [&]() {
        return mp_grid_contains( harness.screen( 0 ), mp_menu_inventory.marker );
    } ) );
    CHECK( mp_player_is_away( 0 ) );
    harness.press( 0, KEY_ESCAPE );
    REQUIRE( harness.run_until( [&]() {
        return !mp_grid_contains( harness.screen( 0 ), mp_menu_inventory.marker );
    } ) );
    CHECK( mp_player_is_away( 0 ) );

    // A turn action does bring them back.
    harness.press( 0, '.' );
    CHECK( harness.run_until( []() {
        return !mp_player_is_away( 0 );
    } ) );
}

TEST_CASE( "the_away_key_does_nothing_without_a_party", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 1 );
    g->player_at( 0 ).set_moves( 100 );
    harness.press( 0, ':' );
    harness.run_until( []() {
        return false;
    }, 3 );
    CHECK_FALSE( mp_player_is_away( 0 ) );
}

TEST_CASE( "a_death_while_away_still_ends_the_session", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    harness.run_until( []() {
        return false;
    }, 1 );
    mp_set_away( 1, true );
    avatar &dead = g->player_at( 1 );
    const std::string name = dead.get_name();
    dead.set_part_hp_cur( bodypart_id( "torso" ), 0 );
    REQUIRE( dead.is_dead_state() );

    CHECK( g->do_turn() );
    CHECK( mp_session_end_reason() == name + " died" );

    // Same teardown as a_death_in_a_session_ends_it_and_saves_the_world.
    get_map() = map();
    overmap_special_batch empty_specials( point_abs_om{} );
    overmap_buffer.create_custom_overmap( point_abs_om{}, empty_specials );
    get_map().load( tripoint_abs_sm( get_map().get_abs_sub() ), false );
    get_avatar().move_to( tripoint_abs_ms::zero );
}

TEST_CASE( "the_session_does_not_end_while_a_client_is_connected", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    CHECK( mp_server_connected_client_count() == 2 );
    CHECK( mp_server_any_client_connected() );
    CHECK_FALSE( mp_session_end_requested() );
}

TEST_CASE( "the_session_ends_when_the_last_client_leaves", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 1 );
    harness.drop_client( 0 );
    CHECK_FALSE( mp_server_any_client_connected() );
    mp_server_end_if_no_clients();
    CHECK( mp_session_end_requested() );
}

TEST_CASE( "a_dead_character_does_not_keep_the_session_alive", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 1 );
    g->player_at( 0 ).set_part_hp_cur( bodypart_id( "torso" ), 0 );
    harness.drop_client( 0 );
    harness.run_until( []() {
        return false;
    }, 3 );
    CHECK_FALSE( mp_server_any_client_connected() );
    mp_server_end_if_no_clients();
    CHECK( mp_session_end_requested() );

    // The dead character also ends the session under the death rule the two
    // death tests above exercise, and that ending's teardown empties the map
    // buffer and the overmaps. A test process goes on to the next test rather
    // than back to the menu, so put back the world tests/test_main.cpp starts
    // every run with, exactly as those tests do.
    get_map() = map();
    overmap_special_batch empty_specials( point_abs_om{} );
    overmap_buffer.create_custom_overmap( point_abs_om{}, empty_specials );
    get_map().load( tripoint_abs_sm( get_map().get_abs_sub() ), false );
    get_avatar().move_to( tripoint_abs_ms::zero );
}

TEST_CASE( "a_partner_disconnect_does_not_end_the_session", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    const std::string name = g->player_at( 1 ).get_name();

    harness.drop_client( 1 );
    harness.run_until( []() {
        return false;
    }, 3 );

    // The dropped seat is marked, and everybody still at a terminal is told so
    // through the one message log the session shares. The session itself keeps
    // running: the server no longer asks the game to end it.
    CHECK_FALSE( mp_session_end_requested() );
    CHECK( mp_player_is_disconnected( 1 ) );
    CHECK( harness.message_log_contains( "Player 2 (" + name + ") disconnected" ) );

    // The partner keeps playing: the disconnected seat no longer holds the
    // barrier, so a turn taken by the player who is still there advances the
    // calendar.
    g->player_at( 0 ).set_moves( 100 );
    const int turn_before = to_turn<int>( calendar::turn );
    harness.press( 0, '.' );
    CHECK( harness.run_until( [turn_before]() {
        return to_turn<int>( calendar::turn ) > turn_before;
    } ) );
}

#endif // MP_SERVER
