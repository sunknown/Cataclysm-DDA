#include "cata_catch.h"

#if defined( MULTIPLAYER )

#include <string>

#include "avatar.h"
#include "bodypart.h"
#include "coordinates.h"
#include "creature.h"
#include "creature_tracker.h"
#include "game.h"
#include "game_test_helper.h"
#include "item.h"
#include "line.h"
#include "map.h"
#include "map_helpers.h"
#include "map_helpers_tests.h"
#include "monster.h"
#include "mp/mp_context.h"
#include "mp/mp_plan.h"
#include "mp/mp_resolve.h"
#include "player_helpers.h"
#include "string_formatter.h"
#include "type_id.h"

namespace
{
// Without shoes run_cost adds 16 to a plain step (see move_cost_test.cpp), so
// the "100 AP for a step on pavement" arithmetic only holds for a normally
// shod avatar.
const itype_id itype_sneakers( "sneakers" );

// One seat on plain pavement, the same baseline mp_plan_test.cpp uses. The
// caller owns the restore_active_state that puts the globals back afterwards.
void setup_resolve_test()
{
    mp_ensure_player_context();
    clear_avatar();
    clear_creatures();
    // Earlier tests' blood or fire; build_test_map() rebuilds terrain but not
    // fields, and a field left here would change what these cases stage on.
    clear_fields( 0 );
    build_test_map( ter_id( "t_pavement" ) );
    // Daylight, with the sight and map caches refreshed: a monster's plan()
    // only targets a player on a level it can see, and the whole test suite
    // shares whatever time the previous case left behind.
    set_time_to_day();
    get_avatar().wear_item( item( itype_sneakers ) );
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    g->ensure_gamemode_for_test();
    mp_plan_reset_all();
}

// get_speed() is base plus a bonus other tests can leave on the character, so
// pin the base such that the arbitration scores come out exactly as asked.
void set_speed( avatar &ava, const int speed )
{
    ava.set_speed_base( speed - ava.get_speed_bonus() );
    REQUIRE( ava.get_speed() == speed );
}

// A monster's bite leaves blood on the map as a field, and the tests that run
// after this file rebuild the terrain but not the fields. Undo that on the way
// out, including when a REQUIRE aborts the case.
struct clear_map_fields_guard {
    ~clear_map_fields_guard() {
        clear_fields( 0 );
    }
};

// Adds slot 1 at `where`. The slot table is rebuilt so it sees the new seat,
// and the context list is grown so mp_activate_player() can hand it a turn.
void add_second_seat( const tripoint_bub_ms &where )
{
    avatar &b = g->add_player_slot();
    b.set_body();
    b.wear_item( item( itype_sneakers ) );
    b.setpos( get_map(), where );
    mp_sync_context_count();
    mp_plan_reset_all();
}

// Gives the seat enough AP to afford anything it plans and commits a step.
// `cost` is the client's own estimate: it is the field the resolver orders
// its cost groups by, while the actual charge comes from mp_plan_cost().
void plan_move( const size_t slot, const tripoint_bub_ms &target, const int cost = 100 )
{
    avatar &ava = g->player_at( slot );
    ava.set_moves( 1000 );
    mp_plan plan;
    plan.kind = mp_plan_kind::move;
    plan.target = target;
    plan.cost = cost;
    std::string err;
    REQUIRE( mp_plan_submit( slot, plan, err ) );
    REQUIRE( mp_plan_ready( slot, err ) );
    REQUIRE( mp_plan_is_ready( slot ) );
}

void plan_melee( const size_t slot, const tripoint_bub_ms &target )
{
    avatar &ava = g->player_at( slot );
    ava.set_moves( 1000 );
    mp_plan plan;
    plan.kind = mp_plan_kind::melee;
    plan.target = target;
    // The client's estimate only orders the group; the server recomputes the
    // attack cost when it resolves.
    plan.cost = 100;
    std::string err;
    REQUIRE( mp_plan_submit( slot, plan, err ) );
    REQUIRE( mp_plan_ready( slot, err ) );
    REQUIRE( mp_plan_is_ready( slot ) );
}
} // namespace

TEST_CASE( "the_faster_seat_takes_a_contested_cell", "[mp]" )
{
    restore_active_state restore;
    setup_resolve_test();

    avatar &a = g->player_at( 0 );
    const tripoint_bub_ms a_start = a.pos_bub();
    const tripoint_bub_ms target = a_start + tripoint_rel_ms::east;
    const tripoint_bub_ms b_start = a_start + tripoint_rel_ms::south;
    add_second_seat( b_start );
    set_speed( a, 200 );
    set_speed( g->player_at( 1 ), 100 );

    plan_move( 0, target );
    plan_move( 1, target );
    mp_resolve_round( 2 );

    // Same target, same cost group: the faster claimant walks and the slower
    // one stays where it was.
    CHECK( a.pos_bub() == target );
    CHECK( g->player_at( 1 ).pos_bub() == b_start );
    // A move that lost arbitration still costs the estimate.
    CHECK( g->player_at( 1 ).get_moves() < 1000 );
    CHECK( a.get_moves() < 1000 );
}

TEST_CASE( "equal_speed_contested_cell_blocks_both", "[mp]" )
{
    restore_active_state restore;
    setup_resolve_test();

    avatar &a = g->player_at( 0 );
    const tripoint_bub_ms a_start = a.pos_bub();
    const tripoint_bub_ms target = a_start + tripoint_rel_ms::east;
    const tripoint_bub_ms b_start = a_start + tripoint_rel_ms::south;
    add_second_seat( b_start );
    set_speed( a, 100 );
    set_speed( g->player_at( 1 ), 100 );

    plan_move( 0, target );
    plan_move( 1, target );
    mp_resolve_round( 2 );

    // Neither claimant wins a tie: the cell stays empty and both stay put.
    CHECK( a.pos_bub() == a_start );
    CHECK( g->player_at( 1 ).pos_bub() == b_start );
    CHECK( get_creature_tracker().creature_at<Creature>( target ) == nullptr );
    // Both paid for the step they were not allowed to take.
    CHECK( a.get_moves() < 1000 );
    CHECK( g->player_at( 1 ).get_moves() < 1000 );
}

TEST_CASE( "a_vacated_cell_chain_is_applied_backwards", "[mp]" )
{
    restore_active_state restore;
    setup_resolve_test();

    avatar &a = g->player_at( 0 );
    const tripoint_bub_ms a_start = a.pos_bub();
    // b stands on the cell a is walking into and leaves it first, so the chain
    // can only be applied tail first.
    const tripoint_bub_ms b_start = a_start + tripoint_rel_ms::east;
    add_second_seat( b_start );

    plan_move( 0, b_start );
    plan_move( 1, b_start + tripoint_rel_ms::east );
    mp_resolve_round( 2 );

    CHECK( a.pos_bub() == b_start );
    CHECK( g->player_at( 1 ).pos_bub() == b_start + tripoint_rel_ms::east );
}

TEST_CASE( "a_two_cycle_is_refused_and_both_stay", "[mp]" )
{
    restore_active_state restore;
    setup_resolve_test();

    avatar &a = g->player_at( 0 );
    const tripoint_bub_ms a_start = a.pos_bub();
    const tripoint_bub_ms b_start = a_start + tripoint_rel_ms::east;
    add_second_seat( b_start );

    // A swap: both targets look free only because the other claimant vacates
    // them. v1 refuses true swaps, so both remain in place.
    plan_move( 0, b_start );
    plan_move( 1, a_start );
    mp_resolve_round( 2 );

    CHECK( a.pos_bub() == a_start );
    CHECK( g->player_at( 1 ).pos_bub() == b_start );
    CHECK( a.get_moves() < 1000 );
    CHECK( g->player_at( 1 ).get_moves() < 1000 );
}

TEST_CASE( "a_second_attack_on_a_killed_target_misses", "[mp]" )
{
    restore_active_state restore;
    setup_resolve_test();

    avatar &a = g->player_at( 0 );
    const tripoint_bub_ms a_start = a.pos_bub();
    const tripoint_bub_ms b_start = a_start + tripoint_rel_ms::south;
    add_second_seat( b_start );
    const tripoint_bub_ms zombie_pos = a_start + tripoint_rel_ms::east;
    monster &zombie = spawn_test_monster( "mon_zombie", zombie_pos );

    plan_melee( 0, zombie_pos );
    plan_melee( 1, zombie_pos );
    const int cost_a = mp_plan_cost( 0, mp_plan_of( 0 ) );
    const int cost_b = mp_plan_cost( 1, mp_plan_of( 1 ) );
    REQUIRE( cost_a > 0 );
    REQUIRE( cost_b > 0 );
    const int moves_a = a.get_moves();
    const int moves_b = g->player_at( 1 ).get_moves();

    // The first blow lands before this round resolves. Killing it here keeps
    // the test deterministic: a live melee roll could miss, and the point is
    // the guard for the plan whose target is already a corpse.
    zombie.die( &get_map(), nullptr );
    REQUIRE( zombie.is_dead() );

    mp_resolve_round( 2 );

    // A miss is not a refusal: the plan is spent, nobody moves, no error text.
    CHECK( a.pos_bub() == a_start );
    CHECK( g->player_at( 1 ).pos_bub() == b_start );
    CHECK( a.get_moves() == moves_a - cost_a );
    CHECK( g->player_at( 1 ).get_moves() == moves_b - cost_b );
    CHECK( mp_plan_error( 0 ).empty() );
    CHECK( mp_plan_error( 1 ).empty() );
}

TEST_CASE( "a_dead_actor_does_not_act", "[mp]" )
{
    restore_active_state restore;
    setup_resolve_test();

    avatar &a = g->player_at( 0 );
    const tripoint_bub_ms a_start = a.pos_bub();
    const tripoint_bub_ms b_start = a_start + tripoint_rel_ms::south;
    add_second_seat( b_start );
    const tripoint_bub_ms a_target = a_start + tripoint_rel_ms::east;
    const tripoint_bub_ms b_target = b_start + tripoint_rel_ms::east;

    plan_move( 0, a_target );
    plan_move( 1, b_target );
    const int moves_a = a.get_moves();
    a.set_part_hp_cur( bodypart_id( "head" ), 0 );
    REQUIRE( a.is_dead_state() );

    mp_resolve_round( 2 );

    // The dead seat's plan is dropped, and the plan it never ran costs nothing.
    CHECK( a.pos_bub() == a_start );
    CHECK( a.get_moves() == moves_a );
    // The living seat still gets its round.
    CHECK( g->player_at( 1 ).pos_bub() == b_target );
}

TEST_CASE( "a_plan_the_seat_cannot_pay_for_is_refused", "[mp]" )
{
    restore_active_state restore;
    setup_resolve_test();

    avatar &a = g->player_at( 0 );
    const tripoint_bub_ms a_start = a.pos_bub();
    plan_move( 0, a_start + tripoint_rel_ms::east );
    const int cost = mp_plan_cost( 0, mp_plan_of( 0 ) );
    REQUIRE( cost > 0 );

    // The world can take AP away after a plan was committed. The resolver has
    // to recompute the cost and refuse rather than spend into the negative.
    a.set_moves( cost - 1 );
    mp_resolve_round( 1 );

    CHECK( a.pos_bub() == a_start );
    CHECK( a.get_moves() == cost - 1 );
    CHECK( mp_plan_error( 0 ) == string_format( "needs %d AP, you have %d", cost, cost - 1 ) );
}

TEST_CASE( "an_unsupported_plan_is_refused_with_not_in_this_phase", "[mp]" )
{
    restore_active_state restore;
    setup_resolve_test();

    avatar &a = g->player_at( 0 );
    a.set_moves( 100 );
    mp_plan plan;
    plan.kind = mp_plan_kind::ranged;
    plan.target = a.pos_bub() + tripoint_rel_ms::east;
    std::string err;
    REQUIRE( mp_plan_submit( 0, plan, err ) );
    REQUIRE( mp_plan_ready( 0, err ) );

    mp_resolve_round( 1 );

    // Phase 1 supports move, melee and pass; anything else is refused without
    // charging AP, and the seat is told why.
    CHECK( a.get_moves() == 100 );
    CHECK( mp_plan_error( 0 ) == "not in this phase" );
}

TEST_CASE( "a_monster_takes_one_step_per_round", "[mp]" )
{
    restore_active_state restore;
    setup_resolve_test();

    avatar &u = g->player_at( 0 );
    clear_map_fields_guard fields;
    const tripoint_bub_ms start = u.pos_bub() + tripoint_rel_ms( 2, 0, 0 );
    monster &zombie = spawn_test_monster( "mon_zombie", start );
    // An explicit goal keeps the fixture independent of whether the monster
    // happens to target the avatar while planning this frame; it still takes
    // its own plan() step inside the round.
    zombie.set_dest( u.pos_abs() );
    // Far more AP than one action costs. The single-player loop would spend it
    // on several steps; a round gives the critter one action, so it can end at
    // most one tile from where it started and must still have AP left.
    zombie.set_moves( 300 );

    mon_round_step();

    CHECK( zombie.get_moves() < 300 );
    CHECK( zombie.get_moves() > 0 );
    CHECK( rl_dist( start, zombie.pos_bub() ) <= 1 );
}

TEST_CASE( "a_fast_monster_acts_in_two_rounds", "[mp]" )
{
    restore_active_state restore;
    setup_resolve_test();

    avatar &u = g->player_at( 0 );
    clear_map_fields_guard fields;
    const tripoint_bub_ms start = u.pos_bub() + tripoint_rel_ms( 2, 0, 0 );
    monster &zombie = spawn_test_monster( "mon_zombie", start );
    zombie.set_dest( u.pos_abs() );
    // Enough AP for several actions in one tick.
    zombie.set_moves( 600 );

    mon_round_step();
    const int after_first = zombie.get_moves();
    // One round spends one action's worth, not the whole pool: a monster that
    // could walk six tiles in a tick walks one and keeps the rest.
    CHECK( after_first < 600 );
    CHECK( after_first > 0 );

    mon_round_step();

    // One more action in the second round, and still at most one tile of
    // travel per round -- not six for the tick.
    CHECK( zombie.get_moves() < after_first );
    CHECK( rl_dist( start, zombie.pos_bub() ) <= 2 );
}

#endif // MULTIPLAYER
