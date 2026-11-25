#include "cata_catch.h"

#if defined( MULTIPLAYER )

#include <string>
#include <vector>

#include "avatar.h"
#include "game.h"
#include "game_test_helper.h"
#include "item.h"
#include "map.h"
#include "map_helpers.h"
#include "map_helpers_tests.h"
#include "mp/mp_context.h"
#include "mp/mp_plan.h"
#include "mp/mp_resolve.h"
#include "mp/mp_turn.h"
#include "player_helpers.h"
#include "string_formatter.h"
#include "type_id.h"

namespace
{
// Without shoes run_cost adds 16 to a plain step (see move_cost_test.cpp), so
// the round's "100 AP for a step on pavement" arithmetic only holds for a
// normally shod avatar.
const itype_id itype_sneakers( "sneakers" );

// The plan module's state is process-wide and sized from the player slots, so
// every test starts from the same one-seat world on plain pavement, mirroring
// coop_turn_test.cpp. The caller owns the restore_active_state that puts the
// globals back afterwards.
void setup_plan_test()
{
    mp_ensure_player_context();
    clear_avatar();
    // A creature left by an earlier case would make a melee plan valid and a
    // tile entrant, so the fixture starts empty and stays that way.
    clear_creatures();
    build_test_map( ter_id( "t_pavement" ) );
    get_avatar().wear_item( item( itype_sneakers ) );
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_turn_reset_all();
    mp_plan_reset_all();
}
} // namespace

TEST_CASE( "mp_plan_ready_accepts_a_step_above_the_current_ap", "[mp][ap_debt]" )
{
    restore_active_state restore;
    setup_plan_test();

    avatar &u = g->player_at( 0 );
    const tripoint_bub_ms start = u.pos_bub();
    const tripoint_bub_ms target = start + tripoint::north_east;
    // The step goes north of the loaded bubble, so the map shifts under the
    // avatar and bub coordinates restart: the absolute cell is what proves the
    // step landed.
    const tripoint_abs_ms abs_target = u.pos_abs() + tripoint::north_east;
    mp_plan p;
    p.kind = mp_plan_kind::move;
    p.target = target;
    std::string err;
    REQUIRE( mp_plan_submit( 0, p, err ) );
    const int cost = mp_plan_cost( 0, mp_plan_of( 0 ) );
    // The case the user hit: a diagonal step whose price is above the 100 AP a
    // turn grants. The old contract refused it and the seat could not move at
    // all.
    REQUIRE( cost > 100 );

    u.set_moves( 100 );
    REQUIRE( mp_plan_ready( 0, err ) );
    CHECK( err.empty() );
    CHECK( mp_plan_is_ready( 0 ) );
    // Committing spends nothing; the engine charges when the action runs.
    CHECK( u.get_moves() == 100 );

    mp_apply_plan( u, mp_plan_of( 0 ) );

    CHECK( u.pos_abs() == abs_target );
    // The engine's full charge stands and the remainder is the seat's debt.
    CHECK( u.get_moves() == 100 - cost );
}

TEST_CASE( "a_plan_starts_with_any_moves_left", "[mp][ap_debt]" )
{
    restore_active_state restore;
    setup_plan_test();

    avatar &u = g->player_at( 0 );
    mp_plan p;
    p.kind = mp_plan_kind::move;
    p.target = u.pos_bub() + tripoint::east;
    std::string err;
    REQUIRE( mp_plan_submit( 0, p, err ) );

    // One point is enough to start: the price is not the entry ticket.
    u.set_moves( 1 );
    CHECK( mp_plan_ready( 0, err ) );
    CHECK( err.empty() );
    mp_plan_cancel( 0 );
    REQUIRE( mp_plan_submit( 0, p, err ) );

    // Zero and below are the recovery states: the seat cannot start anything.
    u.set_moves( 0 );
    CHECK_FALSE( mp_plan_ready( 0, err ) );
    CHECK_FALSE( err.empty() );
    CHECK_FALSE( mp_plan_is_ready( 0 ) );
    CHECK( u.get_moves() == 0 );

    u.set_moves( -1 );
    CHECK_FALSE( mp_plan_ready( 0, err ) );
    CHECK_FALSE( err.empty() );
    CHECK_FALSE( mp_plan_is_ready( 0 ) );
    CHECK( u.get_moves() == -1 );
}

TEST_CASE( "a_melee_plan_starts_with_one_move_left", "[mp][ap_debt]" )
{
    restore_active_state restore;
    setup_plan_test();

    avatar &u = g->player_at( 0 );
    spawn_test_monster( "mon_zombie", u.pos_bub() + tripoint::east );
    mp_plan p;
    p.kind = mp_plan_kind::melee;
    p.target = u.pos_bub() + tripoint::east;
    std::string err;
    REQUIRE( mp_plan_submit( 0, p, err ) );
    REQUIRE( mp_plan_cost( 0, mp_plan_of( 0 ) ) > 1 );

    u.set_moves( 1 );
    CHECK( mp_plan_ready( 0, err ) );
    CHECK( err.empty() );
    CHECK( mp_plan_is_ready( 0 ) );
    CHECK( u.get_moves() == 1 );
}

TEST_CASE( "a_ready_plan_does_not_run_once_the_ap_is_gone", "[mp][ap_debt]" )
{
    restore_active_state restore;
    setup_plan_test();

    avatar &u = g->player_at( 0 );
    const tripoint_bub_ms start = u.pos_bub();
    u.set_moves( 100 );
    mp_plan p;
    p.kind = mp_plan_kind::move;
    p.target = start + tripoint::east;
    std::string err;
    REQUIRE( mp_plan_submit( 0, p, err ) );
    REQUIRE( mp_plan_ready( 0, err ) );

    // Something else spent the pool between the commit and the resolution.
    u.set_moves( 0 );
    mp_apply_plan( u, mp_plan_of( 0 ) );

    CHECK( u.pos_bub() == start );
    CHECK( u.get_moves() == 0 );
    CHECK_FALSE( mp_plan_error( 0 ).empty() );
}

TEST_CASE( "mp_plan_pass_spends_the_positive_remainder_only", "[mp][ap_debt]" )
{
    restore_active_state restore;
    setup_plan_test();

    avatar &u = g->player_at( 0 );
    mp_plan pass;
    pass.kind = mp_plan_kind::pass;
    CHECK( mp_plan_cost( 0, pass ) == 0 );

    SECTION( "a positive pool" ) {
        u.set_moves( 120 );
        mp_plan_pass( 0 );
        CHECK( u.get_moves() == 0 );
    }
    SECTION( "an empty pool" ) {
        u.set_moves( 0 );
        mp_plan_pass( 0 );
        CHECK( u.get_moves() == 0 );
    }
    SECTION( "a debt" ) {
        u.set_moves( -50 );
        mp_plan_pass( 0 );
        // The pass ends the tick's actions; it does not pay off the debt.
        CHECK( u.get_moves() == -50 );
    }
    SECTION( "a repeated pass" ) {
        u.set_moves( 120 );
        mp_plan_pass( 0 );
        mp_plan_pass( 0 );
        CHECK( u.get_moves() == 0 );
    }

    CHECK( mp_plan_is_passed( 0 ) );
    CHECK_FALSE( mp_plan_is_ready( 0 ) );
    CHECK( mp_plan_of( 0 ).kind == mp_plan_kind::none );
}

TEST_CASE( "a_cancel_keeps_the_pass_and_its_price", "[mp][ap_debt]" )
{
    restore_active_state restore;
    setup_plan_test();

    avatar &u = g->player_at( 0 );
    u.set_moves( 120 );
    mp_plan_pass( 0 );
    REQUIRE( mp_plan_is_passed( 0 ) );
    REQUIRE( u.get_moves() == 0 );

    // The pass already ended the seat's part of the tick and spent the
    // remainder; cancelling an intent does not refund it.
    mp_plan_cancel( 0 );
    CHECK( mp_plan_is_passed( 0 ) );
    CHECK( u.get_moves() == 0 );

    // A new intent after a pass is refused until the next tick.
    mp_plan p;
    p.kind = mp_plan_kind::move;
    p.target = u.pos_bub() + tripoint::east;
    std::string err;
    CHECK_FALSE( mp_plan_submit( 0, p, err ) );
    CHECK_FALSE( err.empty() );
    CHECK( mp_plan_is_passed( 0 ) );

    // The next tick hands the moves back and forgets the pass.
    u.set_moves( 100 );
    mp_plan_reset_tick();
    CHECK_FALSE( mp_plan_is_passed( 0 ) );
    CHECK( mp_plan_submit( 0, p, err ) );
}

TEST_CASE( "mp_plan_round_completes_only_for_eligible_seats", "[mp]" )
{
    restore_active_state restore;
    setup_plan_test();

    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();
    mp_plan_reset_all();

    std::string err;
    avatar &a = g->player_at( 0 );
    a.set_moves( 100 );
    mp_plan p;
    p.kind = mp_plan_kind::move;
    p.target = a.pos_bub() + tripoint::east;
    REQUIRE( mp_plan_submit( 0, p, err ) );
    REQUIRE( mp_plan_ready( 0, err ) );

    b.set_moves( 100 );
    mp_plan_pass( 1 );
    REQUIRE( mp_plan_is_passed( 1 ) );

    const std::vector<bool> both{ true, true };
    CHECK( mp_plan_round_complete( both ) );
    // Seat 1 has finished; leaving it out of the round changes nothing.
    const std::vector<bool> only_first{ true, false };
    CHECK( mp_plan_round_complete( only_first ) );
    // Seat 0 has neither readied nor passed, so the round stays open whether
    // or not its neighbour is ignored.
    mp_plan_cancel( 0 );
    CHECK_FALSE( mp_plan_round_complete( both ) );
    CHECK_FALSE( mp_plan_round_complete( only_first ) );
    // Nobody to wait for: vacuously complete.
    CHECK( mp_plan_round_complete( std::vector<bool>{} ) );
    const std::vector<bool> nobody{ false, false };
    CHECK( mp_plan_round_complete( nobody ) );
}

TEST_CASE( "mp_plan_submit_validates_and_cancel_clears_ready", "[mp]" )
{
    restore_active_state restore;
    setup_plan_test();

    avatar &u = g->player_at( 0 );
    u.set_moves( 100 );
    std::string err;

    mp_plan p;
    CHECK_FALSE( mp_plan_submit( 0, p, err ) );
    CHECK( err == "no plan" );

    p.kind = mp_plan_kind::move;
    p.target = u.pos_bub() + tripoint( 2, 0, 0 );
    CHECK_FALSE( mp_plan_submit( 0, p, err ) );
    CHECK( err == "not adjacent" );
    CHECK( mp_plan_of( 0 ).kind == mp_plan_kind::none );

    p.kind = mp_plan_kind::melee;
    p.target = u.pos_bub() + tripoint::east;
    CHECK_FALSE( mp_plan_submit( 0, p, err ) );
    CHECK( err == "nothing there" );

    // A creature on the tile makes the same melee plan acceptable, and the
    // cost is the server's: exactly what the swing spends, the weapon's attack
    // speed.
    spawn_test_monster( "mon_zombie", u.pos_bub() + tripoint::east );
    REQUIRE( mp_plan_submit( 0, p, err ) );
    const item &weapon = u.is_armed() ? *u.get_wielded_item() : null_item_reference();
    CHECK( mp_plan_cost( 0, mp_plan_of( 0 ) ) == u.attack_speed( weapon ) );
    mp_plan_cancel( 0 );

    p = mp_plan();
    p.kind = mp_plan_kind::move;
    p.target = u.pos_bub() + tripoint::east;
    p.cost = 1; // the client's estimate; the server ignores it
    p.seq = 7;
    p.label = "pavement";
    REQUIRE( mp_plan_submit( 0, p, err ) );
    CHECK( err.empty() );
    CHECK( mp_plan_of( 0 ).kind == mp_plan_kind::move );
    CHECK( mp_plan_of( 0 ).seq == 7 );
    CHECK( mp_plan_of( 0 ).label == "pavement" );
    CHECK( mp_plan_cost( 0, mp_plan_of( 0 ) ) == 100 );

    REQUIRE( mp_plan_ready( 0, err ) );
    CHECK( mp_plan_is_ready( 0 ) );
    mp_plan_cancel( 0 );
    CHECK_FALSE( mp_plan_is_ready( 0 ) );
    CHECK_FALSE( mp_plan_is_passed( 0 ) );
    CHECK( mp_plan_of( 0 ).kind == mp_plan_kind::none );

    // With no plan there is nothing to charge for, and ready refuses.
    CHECK( mp_plan_cost( 0, mp_plan{} ) == -1 );
    CHECK_FALSE( mp_plan_ready( 0, err ) );
    CHECK( err == "invalid plan" );
}

TEST_CASE( "a_plan_into_a_closed_door_is_priced_at_the_opening", "[mp]" )
{
    restore_active_state restore;
    setup_plan_test();

    avatar &u = g->player_at( 0 );
    const tripoint_bub_ms target = u.pos_bub() + tripoint_rel_ms::east;
    get_map().ter_set( target, ter_id( "t_door_c" ) );

    mp_plan p;
    p.kind = mp_plan_kind::move;
    p.target = target;
    std::string err;
    REQUIRE( mp_plan_submit( 0, p, err ) );

    // A closed door's terrain move cost is zero, so the plain step estimate is
    // half a pavement step; walking into it opens it in place and spends the
    // character's whole speed. The plan must be priced at that, or the round
    // gate lets through a step the engine cannot pay for.
    CHECK( mp_plan_cost( 0, mp_plan_of( 0 ) ) == u.get_speed() );

    u.set_moves( 50 );
    // Fifty is enough to start; the opening's full charge lands as debt.
    REQUIRE( mp_plan_ready( 0, err ) );
    CHECK( err.empty() );

    mp_apply_plan( u, mp_plan_of( 0 ) );

    // Opened in place: the opening is the action, the avatar stays put and pays
    // the character's whole speed.
    CHECK( get_map().ter( target ) == ter_id( "t_door_o" ) );
    CHECK( u.pos_bub() != target );
    CHECK( u.get_moves() == 50 - u.get_speed() );
}

TEST_CASE( "mp_plan_of_reads_are_copied_to_outlive_the_slot_table", "[mp]" )
{
    restore_active_state restore;
    setup_plan_test();

    avatar &a = g->player_at( 0 );
    a.set_moves( 100 );
    mp_plan p;
    p.kind = mp_plan_kind::move;
    p.target = a.pos_bub() + tripoint::east;
    p.seq = 1;
    std::string err;
    REQUIRE( mp_plan_submit( 0, p, err ) );

    // The supported pattern: copy the plan, then hold the copy.
    const mp_plan first = mp_plan_of( 0 );

    // A seat joining mid-round grows the slot table on its first submit. The
    // copy cannot be invalidated by that, and a fresh read sees both seats.
    avatar &b = g->add_player_slot();
    mp_sync_context_count();
    mp_plan q;
    q.kind = mp_plan_kind::move;
    q.target = b.pos_bub() + tripoint::east;
    q.seq = 2;
    REQUIRE( mp_plan_submit( 1, q, err ) );

    CHECK( first.seq == 1 );
    CHECK( first.target == p.target );
    CHECK( mp_plan_of( 0 ).seq == 1 );
    CHECK( mp_plan_of( 1 ).seq == 2 );
    CHECK( mp_plan_of( 1 ).target == q.target );

    // Cancelling one seat changes only its own entry; the copy taken earlier
    // still carries the plan it was made from.
    mp_plan_cancel( 0 );
    CHECK( mp_plan_of( 0 ).kind == mp_plan_kind::none );
    CHECK( mp_plan_of( 1 ).seq == 2 );
    CHECK( first.seq == 1 );
}

TEST_CASE( "mp_plan_resets_keep_round_tick_and_seat_scopes_apart", "[mp][ap_debt]" )
{
    restore_active_state restore;
    setup_plan_test();

    avatar &u = g->player_at( 0 );
    u.set_moves( 100 );
    mp_plan p;
    p.kind = mp_plan_kind::move;
    p.target = u.pos_bub() + tripoint::east;
    std::string err;
    REQUIRE( mp_plan_submit( 0, p, err ) );
    REQUIRE( mp_plan_ready( 0, err ) );
    mp_plan_set_error( 0, "refused: test" );
    mp_plan_set_phase( mp_round_phase::resolving );

    mp_plan_reset_round();
    CHECK( mp_plan_of( 0 ).kind == mp_plan_kind::none );
    CHECK_FALSE( mp_plan_is_ready( 0 ) );
    // The refusal outlives the reset by one broadcast: that is how the client
    // learns why a committed plan did not run.
    CHECK( mp_plan_error( 0 ) == "refused: test" );
    // A round reset keeps the phase; the resolver decides when the round moves
    // on, and reset_all is the one that starts a fresh session.
    CHECK( mp_plan_phase() == mp_round_phase::resolving );

    // A pass spans the rest of the tick, so an intermediate round keeps it.
    mp_plan_pass( 0 );
    REQUIRE( mp_plan_is_passed( 0 ) );
    mp_plan_reset_round();
    CHECK( mp_plan_is_passed( 0 ) );

    // The next tick opens: the pass is over, the refusal text is kept until
    // the seat's own next intent clears it.
    mp_plan_set_error( 0, "refused: test" );
    mp_plan_reset_tick();
    CHECK_FALSE( mp_plan_is_passed( 0 ) );
    CHECK( mp_plan_error( 0 ) == "refused: test" );
    CHECK( mp_plan_of( 0 ).kind == mp_plan_kind::none );

    // A reused seat for a new identity starts completely clean.
    mp_plan_reset_seat( 0 );
    CHECK( mp_plan_error( 0 ).empty() );
    CHECK_FALSE( mp_plan_is_passed( 0 ) );
    CHECK_FALSE( mp_plan_is_ready( 0 ) );

    mp_plan_set_phase( mp_round_phase::done );
    mp_plan_reset_all();
    CHECK( mp_plan_phase() == mp_round_phase::planning );
    CHECK( mp_plan_of( 0 ).kind == mp_plan_kind::none );
}

#endif // MULTIPLAYER
