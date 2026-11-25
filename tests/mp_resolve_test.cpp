#include "cata_catch.h"

#if defined( MULTIPLAYER )

#include <string>

#include "avatar.h"
#include "bodypart.h"
#include "calendar.h"
#include "cata_scope_helpers.h"
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
#include "mp/mp_interactive.h"
#include "mp/mp_plan.h"
#include "mp/mp_resolve.h"
#include "mp/mp_turn.h"
#include "player_helpers.h"
#include "string_formatter.h"
#include "trap.h"
#include "type_id.h"

namespace
{
// Without shoes run_cost adds 16 to a plain step (see move_cost_test.cpp), so
// the "100 AP for a step on pavement" arithmetic only holds for a normally
// shod avatar.
const itype_id itype_sneakers( "sneakers" );
static const efftype_id effect_blind( "blind" );
// A weapon makes a one-hit kill independent of unarmed damage; the melee
// fixture only needs the blow to land and do more than a point.
const itype_id itype_katana( "katana" );

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
// The avatar goes through the same normalization the first seat got: a raw
// slot avatar differs in stamina and limb state, and the server prices steps
// from those, so two seats that should pay the same step would otherwise land
// in different cost groups.
void add_second_seat( const tripoint_bub_ms &where )
{
    avatar &b = g->add_player_slot();
    clear_character( b );
    b.wear_item( item( itype_sneakers ) );
    b.setpos( get_map(), where );
    mp_sync_context_count();
    mp_plan_reset_all();
}

// Gives the seat enough AP to afford anything it plans and commits a step.
// `client_cost` is what a wire plan would carry in its `cost` field; it is set
// only so a case can prove the resolver ignores it.
void plan_move( const size_t slot, const tripoint_bub_ms &target, const int client_cost = 0 )
{
    avatar &ava = g->player_at( slot );
    ava.set_moves( 1000 );
    mp_plan plan;
    plan.kind = mp_plan_kind::move;
    plan.target = target;
    plan.cost = client_cost;
    std::string err;
    REQUIRE( mp_plan_submit( slot, plan, err ) );
    REQUIRE( mp_plan_ready( slot, err ) );
    REQUIRE( mp_plan_is_ready( slot ) );
}

void plan_melee( const size_t slot, const tripoint_bub_ms &target, const int client_cost = 0 )
{
    avatar &ava = g->player_at( slot );
    ava.set_moves( 1000 );
    mp_plan plan;
    plan.kind = mp_plan_kind::melee;
    plan.target = target;
    plan.cost = client_cost;
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
    // The other seat stands on the far side of the same cell, so both plans
    // are one straight step and cost the same: they land in one cost group and
    // the speed decides, exactly as the server prices them.
    const tripoint_bub_ms b_start = a_start + tripoint_rel_ms( 2, 0, 0 );
    add_second_seat( b_start );
    set_speed( a, 200 );
    set_speed( g->player_at( 1 ), 100 );

    plan_move( 0, target );
    plan_move( 1, target );
    REQUIRE( mp_plan_cost( 0, mp_plan_of( 0 ) ) ==
             mp_plan_cost( 1, mp_plan_of( 1 ) ) );
    mp_resolve_round( 2 );

    // Same target, same cost group: the faster claimant walks and the slower
    // one stays where it was.
    CHECK( a.pos_bub() == target );
    CHECK( g->player_at( 1 ).pos_bub() == b_start );
    // The resolver finished its own phase transition; resetting the round is
    // the tick loop's job, not the resolver's.
    CHECK( mp_plan_phase() == mp_round_phase::done );
    // A move that lost arbitration still costs the server's price.
    CHECK( g->player_at( 1 ).get_moves() < 1000 );
    CHECK( a.get_moves() < 1000 );
}

TEST_CASE( "a_contested_move_is_paid_once_and_can_create_debt", "[mp][ap_debt]" )
{
    restore_active_state restore;
    setup_resolve_test();

    avatar &a = g->player_at( 0 );
    const tripoint_bub_ms a_start = a.pos_bub();
    const tripoint_bub_ms target = a_start + tripoint_rel_ms::east;
    const tripoint_bub_ms b_start = a_start + tripoint_rel_ms( 2, 0, 0 );
    add_second_seat( b_start );
    set_speed( a, 200 );
    set_speed( g->player_at( 1 ), 100 );

    plan_move( 0, target );
    plan_move( 1, target );
    const int cost = mp_plan_cost( 0, mp_plan_of( 0 ) );
    REQUIRE( cost == mp_plan_cost( 1, mp_plan_of( 1 ) ) );

    // Both seats can start (one point each) but neither can cover the step.
    a.set_moves( 1 );
    g->player_at( 1 ).set_moves( 1 );
    mp_resolve_round( 2 );

    // The winner walks and pays; the loser stays and pays the same price for
    // the step it was not allowed to take. Neither is charged twice.
    CHECK( a.pos_bub() == target );
    CHECK( g->player_at( 1 ).pos_bub() == b_start );
    CHECK( a.get_moves() == 1 - cost );
    CHECK( g->player_at( 1 ).get_moves() == 1 - cost );
}

TEST_CASE( "equal_speed_contested_cell_blocks_both", "[mp]" )
{
    restore_active_state restore;
    setup_resolve_test();

    avatar &a = g->player_at( 0 );
    const tripoint_bub_ms a_start = a.pos_bub();
    const tripoint_bub_ms target = a_start + tripoint_rel_ms::east;
    const tripoint_bub_ms b_start = a_start + tripoint_rel_ms( 2, 0, 0 );
    add_second_seat( b_start );
    set_speed( a, 100 );
    set_speed( g->player_at( 1 ), 100 );

    plan_move( 0, target );
    plan_move( 1, target );
    REQUIRE( mp_plan_cost( 0, mp_plan_of( 0 ) ) ==
             mp_plan_cost( 1, mp_plan_of( 1 ) ) );
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

TEST_CASE( "a_move_onto_a_seat_that_is_not_moving_is_refused", "[mp]" )
{
    restore_active_state restore;
    setup_resolve_test();

    avatar &a = g->player_at( 0 );
    const tripoint_bub_ms a_start = a.pos_bub();
    const tripoint_bub_ms b_start = a_start + tripoint_rel_ms::east;
    add_second_seat( b_start );

    // Seat 1 has no plan, so nothing will vacate its cell. creature_tracker
    // only knows the active avatar, so a batch trusting it would stack the two
    // seats; the seat-position probe has to refuse this step.
    plan_move( 0, b_start );
    mp_resolve_round( 2 );

    CHECK( a.pos_bub() == a_start );
    CHECK( g->player_at( 1 ).pos_bub() == b_start );
    CHECK( a.get_moves() < 1000 );
    CHECK( mp_plan_error( 0 ).empty() );
}

TEST_CASE( "a_chain_whose_tail_cannot_move_keeps_everyone_home", "[mp]" )
{
    restore_active_state restore;
    setup_resolve_test();

    avatar &a = g->player_at( 0 );
    const tripoint_bub_ms a_start = a.pos_bub();
    const tripoint_bub_ms b_start = a_start + tripoint_rel_ms::east;
    const tripoint_bub_ms wall = b_start + tripoint_rel_ms::east;
    add_second_seat( b_start );
    get_map().ter_set( wall, ter_id( "t_wall" ) );

    // The tail's step fails against the wall, so it never vacates its cell.
    // The head must not be applied after that failure: an implementation that
    // only tracks the batch's captured "from" cells, or that trusts the
    // creature tracker for seats, would land the head on the tail.
    plan_move( 0, b_start );
    plan_move( 1, wall );
    mp_resolve_round( 2 );

    CHECK( a.pos_bub() == a_start );
    CHECK( g->player_at( 1 ).pos_bub() == b_start );
    CHECK( a.get_moves() < 1000 );
    CHECK( g->player_at( 1 ).get_moves() < 1000 );
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
    zombie.set_hp( 1 );

    // Seat 0's cheaper group resolves first and kills; seat 1's later group
    // then has only the corpse to attack. A melee hit is a normal roll, but a
    // monster's dodge roll is deterministic (dodge * 5): raise the dex cap for
    // this case so the roll cannot land in the miss tail.
    const int saved_max_dex = character_max_dex;
    character_max_dex = 1000;
    a.set_dex_base( 1000 );
    a.set_wielded_item( item( itype_katana ) );
    on_out_of_scope restore_dex( [&]() {
        character_max_dex = saved_max_dex;
        a.set_dex_base( 8 );
    } );

    // The swing order is the server's: seat 0's attack is cheaper (its rigged
    // dexterity floors attack_speed) and resolves first. The client `cost` the
    // two plans carry is deliberately the opposite way round -- 1000 for seat 0
    // and 1 for seat 1 -- and must change nothing, or a client could choose its
    // own priority.
    plan_melee( 0, zombie_pos, 1000 );
    plan_melee( 1, zombie_pos, 1 );
    const int cost_a = mp_plan_cost( 0, mp_plan_of( 0 ) );
    const int cost_b = mp_plan_cost( 1, mp_plan_of( 1 ) );
    REQUIRE( cost_a > 0 );
    REQUIRE( cost_b > 0 );
    REQUIRE( cost_a < cost_b );
    const int moves_b = g->player_at( 1 ).get_moves();

    mp_resolve_round( 2 );

    // The first attack killed the target and the group cleanup removed the
    // corpse; the second plan is a miss that spends its own cost and is not
    // reported as a refusal.
    CHECK( get_creature_tracker().creature_at<Creature>( zombie_pos ) == nullptr );
    CHECK( g->player_at( 1 ).pos_bub() == b_start );
    CHECK( g->player_at( 1 ).get_moves() == moves_b - cost_b );
    CHECK( mp_plan_error( 1 ).empty() );
    CHECK( mp_plan_phase() == mp_round_phase::done );
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

TEST_CASE( "a_plan_below_its_price_still_runs_and_pays_in_full", "[mp][ap_debt]" )
{
    restore_active_state restore;
    setup_resolve_test();

    avatar &a = g->player_at( 0 );
    const tripoint_bub_ms a_start = a.pos_bub();
    plan_move( 0, a_start + tripoint_rel_ms::east );
    const int cost = mp_plan_cost( 0, mp_plan_of( 0 ) );
    REQUIRE( cost > 0 );

    // The world can take AP away after a plan was committed. The step still
    // starts -- one point is enough -- and the engine charges the whole price;
    // the remainder is the seat's debt, not a reason to cancel the action.
    a.set_moves( cost - 1 );
    mp_resolve_round( 1 );

    CHECK( a.pos_bub() == a_start + tripoint_rel_ms::east );
    CHECK( a.get_moves() == -1 );
    CHECK( mp_plan_error( 0 ).empty() );
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

TEST_CASE( "a_planned_step_onto_a_known_trap_runs_without_asking", "[mp]" )
{
    restore_active_state restore;
    setup_resolve_test();

    avatar &a = g->player_at( 0 );
    const tripoint_bub_ms start = a.pos_bub();
    const tripoint_bub_ms target = start + tripoint_rel_ms::east;
    map &here = get_map();
    here.trap_set( target, trap_id( "tr_light_snare" ) );
    a.add_known_trap( target, here.tr_at( target ) );
    REQUIRE_FALSE( g->get_dangerous_tile( target, 10 ).empty() );

    plan_move( 0, target );
    mp_resolve_round( 1 );

    // A dangerous tile asks a confirmation question. The committed plan is the
    // answer: the step happens, and the world thread never waits for a player
    // who cannot be asked.
    CHECK( a.pos_bub() == target );
    CHECK( mp_plan_error( 0 ).empty() );
    // The non-interactive scope is the resolver's alone and cannot leak.
    CHECK( mp_interactive::allowed() );
}

TEST_CASE( "a_planned_step_onto_a_ledge_fizzles_without_asking", "[mp]" )
{
    restore_active_state restore;
    setup_resolve_test();

    avatar &a = g->player_at( 0 );
    const tripoint_bub_ms start = a.pos_bub();
    const tripoint_bub_ms target = start + tripoint_rel_ms::east;
    get_map().ter_set( target, ter_id( "t_open_air" ) );

    plan_move( 0, target );
    const int moves = a.get_moves();
    mp_resolve_round( 1 );

    // What the move needs is the ledge menu. A menu cannot open while the
    // world thread resolves, so the plan fizzles: no step, no AP spent, and a
    // reason the party snapshot can carry.
    CHECK( a.pos_bub() == start );
    CHECK( a.get_moves() == moves );
    CHECK( mp_plan_error( 0 ) == "needs an interactive choice" );
    CHECK( mp_interactive::allowed() );
}

TEST_CASE( "a_planned_door_step_pays_the_opening_and_opens_it", "[mp]" )
{
    restore_active_state restore;
    setup_resolve_test();

    avatar &a = g->player_at( 0 );
    const tripoint_bub_ms start = a.pos_bub();
    const tripoint_bub_ms target = start + tripoint_rel_ms::east;
    map &here = get_map();
    here.ter_set( target, ter_id( "t_door_c" ) );

    plan_move( 0, target );
    // A closed door's terrain move cost is zero, so the plan has to be priced
    // at the character's speed: exactly what avatar_action::move() charges to
    // open it in place.
    REQUIRE( mp_plan_cost( 0, mp_plan_of( 0 ) ) == a.get_speed() );
    const int moves = a.get_moves();

    mp_resolve_round( 1 );

    CHECK( here.ter( target ) == ter_id( "t_door_o" ) );
    CHECK( a.pos_bub() == start );
    CHECK( a.get_moves() == moves - a.get_speed() );
}

TEST_CASE( "an_engine_charge_beyond_the_plan_s_price_stands_as_debt", "[mp][ap_debt]" )
{
    restore_active_state restore;
    setup_resolve_test();

    avatar &a = g->player_at( 0 );
    const tripoint_bub_ms start = a.pos_bub();
    const tripoint_bub_ms wall = start + tripoint_rel_ms::east;
    get_map().ter_set( wall, ter_id( "t_wall" ) );

    // A blind bump spends the character's whole speed, which no terrain price
    // can see. The wall's own move cost is zero, so the plan is priced well
    // below that charge; the engine spends it anyway and the full cost stands.
    plan_move( 0, wall );
    const int cost = mp_plan_cost( 0, mp_plan_of( 0 ) );
    REQUIRE( cost > 0 );
    REQUIRE( cost < a.get_speed() );
    a.set_moves( cost );
    a.add_effect( effect_blind, 1_turns, true );

    mp_resolve_round( 1 );

    CHECK( a.pos_bub() == start );
    CHECK( a.get_moves() == cost - a.get_speed() );
    CHECK( a.get_moves() < 0 );
    // Not a refusal: the engine's own invalid move is still the round's move.
    CHECK( mp_plan_error( 0 ).empty() );
}

TEST_CASE( "a_ready_plan_is_dropped_when_the_seat_can_no_longer_start", "[mp][ap_debt]" )
{
    restore_active_state restore;
    setup_resolve_test();

    avatar &a = g->player_at( 0 );
    const tripoint_bub_ms start = a.pos_bub();
    plan_move( 0, start + tripoint_rel_ms::east );
    REQUIRE( mp_plan_is_ready( 0 ) );

    // The pool emptied between the commit and the resolution: the seat is in
    // recovery, so the round does not run its plan and spends nothing.
    a.set_moves( 0 );
    mp_resolve_round( 1 );

    CHECK( a.pos_bub() == start );
    CHECK( a.get_moves() == 0 );
    CHECK_FALSE( mp_plan_error( 0 ).empty() );
}

TEST_CASE( "a_disconnected_seat_s_plan_is_not_executed", "[mp]" )
{
    restore_active_state restore;
    setup_resolve_test();

    avatar &a = g->player_at( 0 );
    const tripoint_bub_ms a_start = a.pos_bub();
    const tripoint_bub_ms b_start = a_start + tripoint_rel_ms::south;
    add_second_seat( b_start );

    plan_move( 0, a_start + tripoint_rel_ms::east );
    plan_move( 1, b_start + tripoint_rel_ms::east );
    avatar &b = g->player_at( 1 );
    const int moves_b = b.get_moves();

    mp_set_disconnected( 1, true );
    on_out_of_scope restore_connected( []() {
        mp_set_disconnected( 1, false );
    } );

    mp_resolve_round( 2 );

    // Defense in depth: mark_disconnected() cancels the plan, and the resolver
    // also skips a seat whose client is gone. Nobody is at that terminal to
    // see the action, so it must not run.
    CHECK( b.pos_bub() == b_start );
    CHECK( b.get_moves() == moves_b );
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
