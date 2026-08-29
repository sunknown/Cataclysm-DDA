#if defined(MP_SERVER)

#include "cata_catch.h"

#include <memory>

#include "calendar.h"

#include "avatar.h"
#include "debug.h"
#include "game.h"
#include "game_test_helper.h"
#include "mp_session_harness.h"
#include "messages.h"
#include "mp/mp_context.h"
#include "mp/mp_server_main.h"
#include "mp/mp_session.h"
#include "mp/mp_turn.h"

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
// would notice if the pipeline stopped reaching the grid, but both of them are
// red for reasons that are not the pipeline's -- the teardown race and the hurry
// flag -- so neither can be relied on to report it. This one asserts the thing
// those two only imply.
//
// It is red today too, and for the first of those two reasons: it aborts in
// ~mp_session_harness on mp_sched.cpp:94's `holder_ == from`. One player does
// not avoid that race. A thread parked INSIDE player_act() -- which is where a
// player sitting in a menu is -- wakes on mp_sched's `stopping_` without ever
// holding the baton, unwinds out of player_act, and the harness thread body then
// releases a baton it does not hold. One parked-in-menu thread is enough; a
// thread parked at the top of that loop unwinds cleanly, because the body checks
// stopping() there and returns before any release. Task B is what makes this
// green. Closing the menu with a keystroke first would also make it green and is
// exactly the wrong fix: the point of the guard is the pipeline, and dodging the
// abort would cost the assertions below, which are evaluated and printed before
// it. Read those, not the exit status, until task B lands.
TEST_CASE( "one_players_menu_reaches_their_own_client_screen", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 1 );

    // The harness constructor zeroes every slot's moves, which reads as "already
    // acted": the barrier is satisfied before anyone is handed the baton, so no
    // player_act runs, the key is never read and no menu opens. Give the moves
    // back so this player is actionable this turn.
    g->player_at( 0 ).set_moves( 100 );
    // '@' opens the character sheet, which carries the character's own name --
    // though so does the sidebar, now that game::draw() is no longer gated off,
    // so this does not pin down WHICH screen arrived. It does not need to. What
    // the assertion proves is the claim the phase rests on: content specific to
    // this player travelled the whole pipeline to this player's client grid. A
    // pipeline that discards writes cannot produce it, and one that carried some
    // other player's screen would not carry this name.
    harness.press( 0, '@' );
    REQUIRE( harness.run_until( [&]() {
        return mp_grid_contains( harness.screen( 0 ), "P0" );
    }, 5 ) );

    const mp_screen_grid &screen = harness.screen( 0 );
    CHECK( mp_grid_contains( screen, "P0" ) );
    // A rendered character sheet, not one stray cell. It measured 944 non-blank
    // cells when the pipeline was first made to work at this terminal size; the
    // floor is set far below that so ordinary UI churn does not trip it, and far
    // above what any partial write could reach.
    CHECK( mp_grid_nonblank_cells( screen ) > 200 );
}

// The six menus the design names as the heavy ones: each is a different UI
// path, and a global leaked by any of them shows up as one player seeing the
// other's screen. The keys are the DEFAULTMODE defaults.
TEST_CASE( "two_players_sit_in_different_heavy_menus", "[mp]" )
{
    restore_active_state restore;
    struct menu_case {
        const char *name;
        int first;
        int second;
    };
    const std::vector<menu_case> menus = {
        { "inventory vs character sheet", 'i', '@' },
        { "crafting vs map",              '&', 'm' },
        { "vehicle vs construction",      '^', '*' },
    };

    for( const menu_case &c : menus ) {
        CAPTURE( c.name );
        mp_session_harness harness( 2 );

        // The harness constructor zeroes every slot's moves (R6) so the
        // OTHER new test's turn-counting comment is true, but that leaves
        // both players read as already having acted this turn: the barrier
        // is satisfied before anyone is ever handed the baton, so neither
        // player_act runs, the pressed key is never read, and no menu opens.
        // Give both players moves back before pressing, so they are
        // actionable and actually receive the baton this turn. Neither key
        // costs time, so both players hold nothing and both stay live --
        // which is the whole point of the per-player context. Run until both
        // menus are open and rendered, then compare the screens.
        g->player_at( 0 ).set_moves( 100 );
        g->player_at( 1 ).set_moves( 100 );
        harness.press( 0, c.first );
        harness.press( 1, c.second );
        REQUIRE( harness.run_until( [&]() {
            return !mp_grids_equal( harness.screen( 0 ), harness.screen( 1 ) );
        }, 5 ) );

        // The main class of bug this design can have is a leaked global showing
        // one player the other's screen. Comparing the grids catches it.
        CHECK_FALSE( mp_grids_equal( harness.screen( 0 ), harness.screen( 1 ) ) );
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
    harness.press( 1, 'i' );
    const int turn_before = to_turn<int>( calendar::turn );
    REQUIRE( harness.run_until( []() {
        return false;
    }, 10 ) == false );
    CHECK( to_turn<int>( calendar::turn ) == turn_before + 1 );

    // Player 0 hurries them. The countdown must be running before we expire it:
    // the key reaches the server asynchronously through the wire, so wait for
    // the countdown to start, then advance it past expiry.
    harness.press( 0, '\'' );
    REQUIRE( harness.run_until( []() {
        return mp_hurry_remaining_ms() > 0;
    }, 5 ) );
    mp_advance_hurry_for_test( MP_HURRY_MS + 1 );
    // After the countdown the world moves on and the partner's menu is still
    // open -- untouched, just not waited on.
    CHECK( harness.run_until( [turn_before]() {
        return to_turn<int>( calendar::turn ) > turn_before;
    }, 10 ) );
}
#endif // MP_SERVER
