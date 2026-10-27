#if defined(MP_SERVER)

#include "cata_catch.h"

#include <memory>

#include "calendar.h"
#include "cata_scope_helpers.h"

#include "avatar.h"
#include "debug.h"
#include "game.h"
#include "game_test_helper.h"
#include "mp_session_harness.h"
#include "map.h"
#include "map_helpers.h"
#include "messages.h"
#include "output.h"
#include "mp/mp_context.h"
#include "mp/mp_server_main.h"
#include "mp/mp_session.h"
#include "mp/mp_turn.h"
#include "point.h"
#include "string_formatter.h"
#include "type_id.h"
#include "units.h"

static const vproto_id vehicle_prototype_test_shopping_cart( "test_shopping_cart" );

TEST_CASE( "sessions_are_matched_to_slots_by_name", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();

    avatar &a = g->player_at( 0 );
    a.name = "Ada";
    avatar &b = g->add_player_slot();
    b.set_body();
    b.name = "Bo";
    mp_sync_context_count();

    mp_sessions_reset_for_test();
    mp_sessions().push_back( std::make_unique<mp_session>() );
    mp_sessions().push_back( std::make_unique<mp_session>() );
    // Deliberately in the opposite order to the slots: the match is by name,
    // not by who connected first, so that reconnecting in a different order
    // still puts everyone back in their own body.
    mp_sessions()[0]->name = "Bo";
    mp_sessions()[1]->name = "Ada";

    std::string err;
    REQUIRE( mp_server_start_session( err ) );
    CHECK( err.empty() );
    CHECK( mp_sessions()[0]->context_index == 1 );
    CHECK( mp_sessions()[1]->context_index == 0 );

    mp_server_end_session();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

TEST_CASE( "an_unknown_name_is_refused_with_a_reason", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    g->player_at( 0 ).name = "Ada";

    mp_sessions_reset_for_test();
    mp_sessions().push_back( std::make_unique<mp_session>() );
    mp_sessions()[0]->name = "Nobody";

    std::string err;
    CHECK_FALSE( mp_server_start_session( err ) );
    CHECK( err.find( "Nobody" ) != std::string::npos );

    mp_server_end_session();
}

TEST_CASE( "a_roster_with_fewer_clients_than_slots_is_refused", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &a = g->player_at( 0 );
    a.name = "Ada";
    avatar &b = g->add_player_slot();
    b.set_body();
    b.name = "Bo";
    mp_sync_context_count();

    // The roster is fixed at connection time: one client against two avatars
    // would leave the second body without a driver, so start must refuse
    // rather than park an avatar forever.
    mp_sessions_reset_for_test();
    mp_sessions().push_back( std::make_unique<mp_session>() );
    mp_sessions()[0]->name = "Ada";

    std::string err;
    CHECK_FALSE( mp_server_start_session( err ) );
    // Both counts in the message: one client, two characters. A log line
    // must be diagnosable without re-reading the save.
    CHECK( err.find( std::to_string( mp_sessions().size() ) ) != std::string::npos );
    CHECK( err.find( std::to_string( g->player_slot_count() ) ) != std::string::npos );

    mp_server_end_session();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

TEST_CASE( "two_clients_named_the_same_character_are_refused", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &a = g->player_at( 0 );
    a.name = "Ada";
    avatar &b = g->add_player_slot();
    b.set_body();
    b.name = "Bo";
    mp_sync_context_count();

    // Counts line up (two clients, two characters) and both names match an
    // avatar: the failure the name loop cannot see is that both would drive
    // Ada while Bo sits empty.
    mp_sessions_reset_for_test();
    mp_sessions().push_back( std::make_unique<mp_session>() );
    mp_sessions().push_back( std::make_unique<mp_session>() );
    mp_sessions()[0]->name = "Ada";
    mp_sessions()[1]->name = "Ada";

    std::string err;
    CHECK_FALSE( mp_server_start_session( err ) );
    CHECK( err.find( "Ada" ) != std::string::npos );

    mp_server_end_session();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

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

    // Expire it. The world moves on past the frozen turn -- `+ 1` is where the
    // first phase already left it -- and the partner's menu is still open:
    // untouched, just not waited on.
    mp_advance_hurry_for_test( MP_HURRY_MS + 1, mp_hurry_source::manual );
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
#endif // MP_SERVER
