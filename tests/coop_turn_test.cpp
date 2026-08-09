#include "cata_catch.h"

#include "activity_type.h"
#include "avatar.h"
#include "bodypart.h"
#include "calendar.h"
#include "game.h"
#include "game_test_helper.h"
#include "map_helpers_tests.h"
#include "mp/mp_context.h"
#include "player_activity.h"
#include "player_helpers.h"
#include "type_id.h"
#include "units.h"

// The world halves of a turn are what the orchestrator runs with no active
// player. These tests pin the two properties the co-op barrier depends on:
// the calendar advances exactly once in the pre half, and the post half never
// hands anybody their moves back.

TEST_CASE( "world_turn_pre_advances_the_calendar_once", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();

    // The first call in a process is the one-time new_game pass
    // (weather.on_game_start), not a calendar advance.
    g->world_turn_pre();

    const int before = to_turn<int>( calendar::turn );
    g->world_turn_pre();
    CHECK( to_turn<int>( calendar::turn ) == before + 1 );
}

TEST_CASE( "world_turn_post_does_not_replenish_moves", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();

    avatar &u = g->player_at( 0 );
    u.set_moves( 0 );
    u.activity = player_activity();

    // world_turn_post() is the world half: do_turn() always calls it under
    // mp_activate_world(), and this test replicates that call convention.
    // world_turn_post() is defined as running with no player active; calling
    // it under a player's own stack instead would silently violate that.
    mp_activate_world();
    g->world_turn_post();

    // Moves come back in avatar_turn_post, which the orchestrator runs after
    // this. Replenishing here would let a player act again before the monsters
    // had moved, and would make the barrier meaningless.
    CHECK( u.get_moves() == 0 );
}

TEST_CASE( "avatar_turn_post_replenishes_moves", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();

    avatar &u = g->player_at( 0 );
    u.set_moves( 0 );
    u.activity = player_activity();

    g->avatar_turn_post( u );

    CHECK( u.get_moves() > 0 );
}

TEST_CASE( "avatar_turn_pre_drains_a_running_activity", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();

    avatar &u = g->player_at( 0 );
    u.set_moves( 100 );
    u.activity = player_activity( activity_id( "ACT_HACKING" ), 500, 0 );

    g->avatar_turn_pre( u );

    // The activity consumed the turn's moves without the player pressing
    // anything: this is what makes an occupied player count as "has acted"
    // at the barrier (mp_turn, task 7).
    CHECK( u.get_moves() <= 0 );
}

TEST_CASE( "avatar_turn_post_runs_for_every_slot_independently", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();
    game_test_helper::reset_player_slots();

    avatar &a = g->player_at( 0 );
    avatar &b = g->add_player_slot();
    b.set_body();
    a.set_moves( 0 );
    b.set_moves( 0 );
    // Start slot b's torso well below normal so update_bodytemp() has
    // somewhere to move it: a slot that never got processed would stay put.
    const units::temperature cold_start = units::from_celsius( 20 );
    b.set_part_temp_cur( bodypart_id( "torso" ), cold_start );

    g->avatar_turn_post( a );
    g->avatar_turn_post( b );

    // The defect the old hotseat loop had: slot 1 only got process_turn(), so
    // its body temperature, wetness and morale never updated. Both slots now
    // go through the same function.
    CHECK( a.get_moves() > 0 );
    CHECK( b.get_moves() > 0 );
    CHECK( b.get_part_temp_cur( bodypart_id( "torso" ) ) != cold_start );

    game_test_helper::reset_player_slots();
}

TEST_CASE( "calendar_advances_once_per_turn", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();

    // With no moves and no activity a slot's action loop is skipped, which is
    // what makes this headless: handle_action() would read real input.
    auto idle_all_slots = [&]() {
        for( size_t i = 0; i < g->player_slot_count(); ++i ) {
            avatar &idle = g->player_at( i );
            idle.set_moves( 0 );
            idle.activity = player_activity();
        }
    };

    idle_all_slots();
    g->do_turn();          // one-time new_game pass

    idle_all_slots();
    const int before = to_turn<int>( calendar::turn );
    g->do_turn();
    CHECK( to_turn<int>( calendar::turn ) == before + 1 );
}

TEST_CASE( "every_slot_gets_its_moves_back", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();
    game_test_helper::reset_player_slots();

    avatar &a = g->player_at( 0 );
    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();

    // Both slots spend their turn on an activity: the action loop reads real
    // input and cannot run headless, the activity branch can.
    a.set_moves( 100 );
    b.set_moves( 100 );
    a.activity = player_activity( activity_id( "ACT_HACKING" ), 500, 0 );
    b.activity = player_activity( activity_id( "ACT_HACKING" ), 500, 0 );

    g->do_turn();

    CHECK( a.get_moves() > 0 );
    CHECK( b.get_moves() > 0 );

    // get_moves() alone cannot fail this test: the test set both slots' moves
    // to 100 itself, and a reverted orchestrator that only ever touches slot 0
    // would leave b untouched, still positive. avatar_turn_pre() drains a
    // running activity (do_turn.cpp), so this only passes if slot 1 actually
    // went through avatar_turn_pre()/player_act(), not just slot 0.
    CHECK( b.activity.moves_left < 500 );

    // The activity is deliberately left running (the moves_left check above is
    // the proof), which leaves each player's wait_popup -- a per-player
    // player_context member, lazily built by handle_progress_ui() during that
    // player's own half of avatar_turn_post() -- alive with its ui_adaptor
    // owned by that same player's own ui_stack. reset_player_slots()/
    // mp_sync_context_count() below pops slot 1's context off; popping it
    // while its wait_popup (and the ui_adaptor it owns) is still alive would
    // trip the ui_stack-must-be-empty assert mp_sync_context_count() carries
    // for exactly this reason. Tear each player's popup down under its own
    // context, matching how it was built.
    for( size_t i = 0; i < g->player_slot_count(); ++i ) {
        mp_activate_player( i );
        g->wait_popup_reset();
    }

    // Back to slot 0 before touching player_slots: game_test_helper::
    // reset_player_slots() (tests/game_test_helper.h) captures player_at(0) as
    // "the" avatar to keep and then clears owned_extra_players. It has no
    // fallback for slot 0 not being the currently-active one -- every other
    // caller in the suite reaches it with slot 0 active (do_turn() always
    // ends that way) -- so leaving the loop above on the last slot (as it did
    // before this fix) makes player_at(0) alias the object owned_extra_players
    // is about to free, leaving player_slots[0] dangling instead of &g->u.
    // That is what actually tripped mp_activate_player()'s invariant assert
    // on the next activation, not mp_activate_world().
    mp_activate_player( 0 );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

TEST_CASE( "an_activity_spanning_turns_finishes_without_aborting", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();

    avatar &u = g->player_at( 0 );
    // TIME-based, enough total moves that a single 100-move turn cannot finish
    // it: turn 1 must leave it running, which is what makes handle_progress_ui()
    // build a progress popup (get_progress_message() only returns non-nullopt
    // while the activity is still going). A later turn then drains the rest
    // and finishes it, which calls wait_popup_reset() (player_activity.cpp,
    // moves_left <= 0 branch) from inside avatar_turn_pre() -- squarely in
    // the player phase.
    //
    // This is the regression fix-round-1 Finding 1 describes: the popup used
    // to be built while world_turn_post() ran under mp_activate_world(), so
    // its ui_adaptor's owner was the world stack; tearing it down here, under
    // a player's own stack, tripped ui_adaptor's owner == active_stack assert
    // and aborted the process. It reproduces on HEAD 733c5a75c9 (handle_
    // progress_ui() called from world_turn_post()) and passes once handle_
    // progress_ui() runs from the player half (avatar_turn_post()) instead.
    u.activity = player_activity( activity_id( "ACT_HACKING" ), 250, 0 );

    // Drive full turns (do_turn(), including player_act()'s headless branch)
    // only while moves_left is guaranteed >= 100 before the turn's decrement:
    // player_activity::do_turn()'s TIME branch then always takes
    // `moves_left -= 100 * mult; you.set_moves(0);`, unconditionally zeroing
    // the avatar's moves. That is what keeps player_act() out of its
    // real-input branch (get_moves() == 0 after the drain, every time). The
    // turn that would drop moves_left below 100 is deliberately NOT driven
    // through a full do_turn(): that branch reduces moves by a *fraction* of
    // the turn instead of to zero (`mod_moves( -get_moves() * moves_left /
    // 100 )`), which leaves the avatar with moves > 0 and would push
    // player_act() into real_input territory (the other headless tests in
    // this file avoid exactly this).
    int full_turns = 0;
    while( u.activity && u.activity.moves_left >= 100 && full_turns < 20 ) {
        u.set_moves( 100 );
        g->do_turn();
        ++full_turns;
    }
    REQUIRE( full_turns > 0 );

    if( u.activity ) {
        // Finish it directly through avatar_turn_pre() -- the actual
        // player-phase call site the finding is about -- instead of a full
        // do_turn(): this reaches player_activity::do_turn()'s moves_left <=
        // 0 branch (and its wait_popup_reset() call) without ever touching
        // player_act()'s real-input branch.
        mp_activate_player( 0 );
        u.set_moves( 100 );
        g->avatar_turn_pre( u );
    }

    // Reaching this line at all is most of the proof: on the unfixed code the
    // process aborts inside one of the calls above instead of returning.
    CHECK_FALSE( u.activity );
    // The activity had to span more than one turn -- otherwise handle_
    // progress_ui() would never have built a popup for wait_popup_reset() to
    // tear down under a mismatched stack, and the test would pass vacuously.
    CHECK( full_turns > 1 );
}

TEST_CASE( "moves_come_back_only_after_the_world_moved", "[coop]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();

    avatar &u = g->player_at( 0 );
    u.set_moves( 0 );
    u.activity = player_activity();

    // The single-player ordering the whole barrier rests on: world_turn_post
    // (monmove) happens while the player is still out of moves.
    //
    // world_turn_pre/world_turn_post are the world half, and do_turn() always
    // runs them under mp_activate_world(); this test replicates that call
    // convention rather than calling them under whatever stack happens to be
    // active. avatar_turn_post() is the player half again (it calls
    // handle_progress_ui(), which opens UI for whoever is active), so it
    // needs mp_activate_player( 0 ) first, matching do_turn()'s own handoff.
    mp_activate_world();
    g->world_turn_pre();
    CHECK( u.get_moves() <= 0 );
    g->world_turn_post();
    CHECK( u.get_moves() <= 0 );
    mp_activate_player( 0 );
    g->avatar_turn_post( u );
    CHECK( u.get_moves() > 0 );
}
