#include "cata_catch.h"

#if defined( MULTIPLAYER )

#include <string>
#include <vector>

#include "avatar.h"
#include "game.h"
#include "game_test_helper.h"
#include "item.h"
#include "map_helpers_tests.h"
#include "mp/mp_context.h"
#include "mp/mp_plan.h"
#include "mp/mp_turn.h"
#include "player_helpers.h"
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
    build_test_map( ter_id( "t_pavement" ) );
    get_avatar().wear_item( item( itype_sneakers ) );
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_turn_reset_all();
    mp_plan_reset_all();
}
} // namespace

TEST_CASE( "mp_plan_ready_is_gated_by_ap", "[mp]" )
{
    restore_active_state restore;
    setup_plan_test();

    avatar &u = g->player_at( 0 );
    u.set_moves( 64 );
    mp_plan p;
    p.kind = mp_plan_kind::move;
    p.target = u.pos_bub() + tripoint::east;
    std::string err;
    REQUIRE( mp_plan_submit( 0, p, err ) );
    CHECK_FALSE( mp_plan_ready( 0, err ) );
    CHECK_FALSE( err.empty() );
    CHECK( err == "needs 100 AP, you have 64" );
    CHECK_FALSE( mp_plan_is_ready( 0 ) );
    // A refused ready must not touch the seat's AP.
    CHECK( u.get_moves() == 64 );

    u.set_moves( 100 );
    CHECK( mp_plan_ready( 0, err ) );
    CHECK( mp_plan_is_ready( 0 ) );
}

TEST_CASE( "mp_plan_pass_spends_all_moves", "[mp]" )
{
    restore_active_state restore;
    setup_plan_test();

    avatar &u = g->player_at( 0 );
    u.set_moves( 120 );
    mp_plan pass;
    pass.kind = mp_plan_kind::pass;
    CHECK( mp_plan_cost( 0, pass ) == 0 );

    mp_plan_pass( 0 );
    CHECK( mp_plan_is_passed( 0 ) );
    CHECK_FALSE( mp_plan_is_ready( 0 ) );
    CHECK( mp_plan_of( 0 ).kind == mp_plan_kind::none );
    CHECK( u.get_moves() == 0 );
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
    // cost is the server's: the base attack cost plus the weapon's speed.
    spawn_test_monster( "mon_zombie", u.pos_bub() + tripoint::east );
    REQUIRE( mp_plan_submit( 0, p, err ) );
    const item &weapon = u.is_armed() ? *u.get_wielded_item() : null_item_reference();
    CHECK( mp_plan_cost( 0, mp_plan_of( 0 ) ) == 100 + u.attack_speed( weapon ) );
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

TEST_CASE( "mp_plan_resets_keep_round_and_session_scopes_apart", "[mp]" )
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

    mp_plan_set_phase( mp_round_phase::resolving );
    CHECK( mp_plan_phase() == mp_round_phase::resolving );

    mp_plan_reset_round();
    CHECK( mp_plan_of( 0 ).kind == mp_plan_kind::none );
    CHECK_FALSE( mp_plan_is_ready( 0 ) );
    CHECK_FALSE( mp_plan_is_passed( 0 ) );
    // A round reset keeps the phase; the resolver decides when the round moves
    // on, and reset_all is the one that starts a fresh session.
    CHECK( mp_plan_phase() == mp_round_phase::resolving );

    mp_plan_set_phase( mp_round_phase::done );
    mp_plan_reset_all();
    CHECK( mp_plan_phase() == mp_round_phase::planning );
    CHECK( mp_plan_of( 0 ).kind == mp_plan_kind::none );
}

#endif // MULTIPLAYER
