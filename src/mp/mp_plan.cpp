#if defined( MULTIPLAYER )

#include "mp/mp_plan.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "avatar.h"
#include "creature.h"
#include "creature_tracker.h"
#include "game.h"
#include "item.h"
#include "map.h"
#include "string_formatter.h"
#include "vehicle.h"
#include "vpart_position.h"

namespace
{
struct mp_plan_slot {
    mp_plan plan;
    bool ready = false;
    bool passed = false;
    std::string error;
};

// One entry per player slot, grown lazily because a seat can be added
// mid-session (a client joining) and submit before the next reset. Growth
// reallocates, so references mp_plan_of handed out do not survive it (see the
// lifetime rule there); copies are the way to keep a plan.
std::vector<mp_plan_slot> g_slots;
mp_round_phase g_phase = mp_round_phase::planning;

void ensure_sized()
{
    g_slots.resize( g->player_slot_count() );
}

bool seat_exists( const size_t slot )
{
    return slot < g->player_slot_count();
}

// A step is worth what the seat would actually pay to walk it: the terrain
// cost passed through the character's own speed and encumbrance. Same shape as
// npcmove.cpp's movement, so a plan costs what executing it will.
int move_cost( const size_t slot, const mp_plan &plan )
{
    avatar &ava = g->player_at( slot );
    const tripoint_bub_ms from = ava.pos_bub();
    const bool diagonal = from.x() != plan.target.x() && from.y() != plan.target.y();
    return ava.run_cost( get_map().combined_movecost( from, plan.target ), diagonal );
}

// True when avatar_action::move() would open the target in place and charge
// the character's whole speed instead of a step. A closed door's terrain move
// cost is zero, so the ordinary estimate would leave that charge unpaid and
// the character could end a planned step with negative moves. The engine's own
// check_only open is the predicate: it opens terrain, furniture and a vehicle
// part alike without touching anything.
bool opens_door_in_place( avatar &ava, const tripoint_bub_ms &target )
{
    map &here = get_map();
    if( ava.pos_bub().z() != target.z() ) {
        return false;
    }
    const optional_vpart_position vp = here.veh_at( target );
    const optional_vpart_position from_vp = here.veh_at( ava.pos_bub() );
    const bool outside_vehicle = !vp || !from_vp ||
                                 &vp->vehicle() != &from_vp->vehicle();
    const bool vehicle_door = vp && vp->vehicle().next_part_to_open(
                                  vp->part_index(), outside_vehicle ) >= 0;
    // A walkable terrain door (a wooden fence gate) is walked over rather than
    // opened while running; a vehicle door and a closed terrain or furniture
    // door are opened in place in either mode.
    if( !vehicle_door && here.passable_ter_furn( target ) && !ava.is_walking() ) {
        return false;
    }
    return here.open_door( ava, target, !here.is_outside( ava.pos_bub() ), true );
}
} // namespace

void mp_plan_reset_all()
{
    g_slots.assign( g->player_slot_count(), mp_plan_slot() );
    g_phase = mp_round_phase::planning;
}

void mp_plan_reset_round()
{
    ensure_sized();
    for( mp_plan_slot &slot : g_slots ) {
        slot.plan = mp_plan();
        slot.ready = false;
        slot.passed = false;
        // `error` deliberately survives: a refusal the resolver stored is what
        // the post-resolve party broadcast carries to the client, and the tick
        // loop resets the round before it broadcasts. The seat's next submit,
        // cancel or pass clears it, and mp_plan_reset_all() clears it with the
        // rest of the session.
    }
}

bool mp_plan_submit( const size_t slot, const mp_plan &plan, std::string &error )
{
    error.clear();
    ensure_sized();
    if( !seat_exists( slot ) ) {
        error = "no such seat";
        return false;
    }

    switch( plan.kind ) {
        case mp_plan_kind::none:
            error = "no plan";
            return false;
        case mp_plan_kind::move: {
            const tripoint_bub_ms here = g->player_at( slot ).pos_bub();
            const int dx = std::abs( plan.target.x() - here.x() );
            const int dy = std::abs( plan.target.y() - here.y() );
            if( plan.target.z() != here.z() || std::max( dx, dy ) != 1 ) {
                error = "not adjacent";
                return false;
            }
            break;
        }
        case mp_plan_kind::melee: {
            const Creature *const target =
                get_creature_tracker().creature_at<Creature>( plan.target );
            if( target == nullptr || target->is_dead_state() ) {
                error = "nothing there";
                return false;
            }
            break;
        }
        case mp_plan_kind::ranged:
        case mp_plan_kind::use_item:
        case mp_plan_kind::interact:
        case mp_plan_kind::pass:
            // Accepted here; the resolver is the one that decides whether the
            // kind is meaningful in this phase.
            break;
    }

    mp_plan_slot &entry = g_slots[slot];
    entry.plan = plan;
    entry.ready = false;
    entry.passed = false;
    // A new intent supersedes the last refusal reason.
    entry.error.clear();
    return true;
}

int mp_plan_cost( const size_t slot, const mp_plan &plan )
{
    switch( plan.kind ) {
        case mp_plan_kind::none:
            return -1;
        case mp_plan_kind::pass:
        case mp_plan_kind::ranged:
        case mp_plan_kind::use_item:
        case mp_plan_kind::interact:
            // Nothing charged up front; the resolver refuses the kinds it
            // cannot run in this phase.
            return 0;
        case mp_plan_kind::move:
            if( !seat_exists( slot ) ) {
                return -1;
            }
            if( opens_door_in_place( g->player_at( slot ), plan.target ) ) {
                // avatar_action::move() opens an adjacent closed door or gate
                // where it stands and spends the character's whole speed for
                // it, not the tile's step cost.
                return g->player_at( slot ).get_speed();
            }
            return move_cost( slot, plan );
        case mp_plan_kind::melee: {
            if( !seat_exists( slot ) ) {
                return -1;
            }
            avatar &ava = g->player_at( slot );
            const item &weapon = ava.is_armed() ? *ava.get_wielded_item() : null_item_reference();
            // Exactly what Character::melee_attack_abstract() charges for the
            // swing: its move_cost is attack_speed(weapon), with no base 100.
            return ava.attack_speed( weapon );
        }
    }
    return -1;
}

void mp_plan_cancel( const size_t slot )
{
    ensure_sized();
    if( !seat_exists( slot ) ) {
        return;
    }
    mp_plan_slot &entry = g_slots[slot];
    entry.plan = mp_plan();
    entry.ready = false;
    entry.passed = false;
    entry.error.clear();
}

bool mp_plan_ready( const size_t slot, std::string &error )
{
    error.clear();
    ensure_sized();
    if( !seat_exists( slot ) ) {
        error = "no such seat";
        return false;
    }
    const int cost = mp_plan_cost( slot, g_slots[slot].plan );
    if( cost < 0 ) {
        error = "invalid plan";
        return false;
    }
    avatar &ava = g->player_at( slot );
    if( ava.get_moves() < cost ) {
        error = string_format( "needs %d AP, you have %d", cost, ava.get_moves() );
        return false;
    }
    mp_plan_slot &entry = g_slots[slot];
    entry.ready = true;
    entry.passed = false;
    return true;
}

void mp_plan_pass( const size_t slot )
{
    ensure_sized();
    if( !seat_exists( slot ) ) {
        return;
    }
    mp_plan_slot &entry = g_slots[slot];
    entry.passed = true;
    entry.plan = mp_plan();
    entry.ready = false;
    entry.error.clear();
    g->player_at( slot ).set_moves( 0 );
}

bool mp_plan_is_ready( const size_t slot )
{
    ensure_sized();
    return seat_exists( slot ) && g_slots[slot].ready;
}

bool mp_plan_is_passed( const size_t slot )
{
    ensure_sized();
    return seat_exists( slot ) && g_slots[slot].passed;
}

const mp_plan &mp_plan_of( const size_t slot )
{
    static const mp_plan empty{};
    ensure_sized();
    if( !seat_exists( slot ) || g_slots[slot].plan.kind == mp_plan_kind::none ) {
        return empty;
    }
    return g_slots[slot].plan;
}

bool mp_plan_round_complete( const std::vector<bool> &eligible )
{
    ensure_sized();
    for( size_t i = 0; i < eligible.size(); ++i ) {
        if( !eligible[i] ) {
            continue;
        }
        if( !seat_exists( i ) || ( !g_slots[i].ready && !g_slots[i].passed ) ) {
            return false;
        }
    }
    return true;
}

const std::string &mp_plan_error( const size_t slot )
{
    static const std::string empty;
    ensure_sized();
    if( !seat_exists( slot ) ) {
        return empty;
    }
    return g_slots[slot].error;
}

void mp_plan_set_error( const size_t slot, std::string error )
{
    ensure_sized();
    if( !seat_exists( slot ) ) {
        return;
    }
    g_slots[slot].error = std::move( error );
}

mp_round_phase mp_plan_phase()
{
    return g_phase;
}

void mp_plan_set_phase( const mp_round_phase phase )
{
    g_phase = phase;
}

#endif // MULTIPLAYER
