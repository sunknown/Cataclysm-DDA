#if defined( MULTIPLAYER )

#include "mp/mp_resolve.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

#include "avatar.h"
#include "avatar_action.h"
#include "coordinates.h"
#include "creature.h"
#include "creature_tracker.h"
#include "explosion.h"
#include "game.h"
#include "line.h"
#include "map.h"
#include "mp/mp_context.h"
#include "mp/mp_interactive.h"
#include "mp/mp_plan.h"
#include "mp/mp_turn.h"
#include "sounds.h"
#include "string_formatter.h"

namespace
{
constexpr size_t no_slot = static_cast<size_t>( -1 );

// The plan did not run because the engine needed a choice from a player and
// none can be asked while the world thread resolves.
constexpr const char *interactive_choice_error = "needs an interactive choice";

// The one place a player's AP is allowed to stop being negative: the engine
// charges some special moves (a swim, a mount, a blind bump) more than any
// plan could price, so after the call the seat is put back at zero. Single
// player never runs the resolver and so never reaches this.
void clamp_moves( avatar &ava )
{
    if( ava.get_moves() < 0 ) {
        ava.set_moves( 0 );
    }
}

// The seat whose avatar this is, or no_slot for an avatar the game does not
// own as a player (mp_apply_plan is usable with one, the error storage is not).
size_t slot_of( const avatar &ava )
{
    for( size_t i = 0; i < g->player_slot_count(); ++i ) {
        if( &g->player_at( i ) == &ava ) {
            return i;
        }
    }
    return no_slot;
}

// True when another seat stands on `p`. creature_tracker reports only the
// active avatar, so the slots have to be checked by position as well -- the
// world phase deactivates every seat but slot 0.
bool seat_at( const tripoint_bub_ms &p, const size_t except )
{
    for( size_t i = 0; i < g->player_slot_count(); ++i ) {
        if( i != except && g->player_at( i ).pos_bub() == p ) {
            return true;
        }
    }
    return false;
}

// Recomputes the plan's cost against the world as it is right now and refuses
// the action when the seat cannot pay it. A refused action spends nothing and
// leaves the reason for the party snapshot. `cost` comes back filled either
// way, so the caller charges what the gate checked and never subtracts AP
// twice.
bool affordable( const size_t slot, const avatar &ava, const mp_plan &plan, int &cost )
{
    cost = slot == no_slot ? 0 : std::max( 0, mp_plan_cost( slot, plan ) );
    if( slot == no_slot || ava.get_moves() >= cost ) {
        return true;
    }
    mp_plan_set_error( slot, string_format( "needs %d AP, you have %d", cost, ava.get_moves() ) );
    return false;
}

// Steps the avatar one tile. A step the map refuses (a creature in the way, an
// effect that stopped the move) still costs the round's move when nothing was
// spent on it -- an arbitrated move is a commitment, not a retry. A step whose
// engine call stopped at a menu (a ledge, a locked vehicle door) fizzles
// instead: it spends nothing and leaves the reason for the party snapshot.
// Returns true when the avatar changed cells.
bool step( avatar &ava, const tripoint_bub_ms &target, const int cost )
{
    const tripoint_bub_ms from = ava.pos_bub();
    const int moves_before = ava.get_moves();
    mp_interactive::clear_menu_cancelled();
    avatar_action::move( ava, get_map(), target - from );
    if( mp_interactive::menu_cancelled() ) {
        mp_interactive::clear_menu_cancelled();
        const size_t slot = slot_of( ava );
        if( slot != no_slot ) {
            mp_plan_set_error( slot, interactive_choice_error );
        }
        // The engine got no answer, so nothing was decided: undo whatever AP
        // it spent and leave the avatar where it was. A move it did make
        // (which no current path does after a cancelled menu) stands.
        if( ava.pos_bub() == from ) {
            ava.set_moves( moves_before );
        }
        clamp_moves( ava );
        return ava.pos_bub() != from;
    }
    clamp_moves( ava );
    if( ava.pos_bub() != from ) {
        return true;
    }
    if( ava.get_moves() == moves_before ) {
        ava.mod_moves( -cost );
    }
    clamp_moves( ava );
    return false;
}

// One melee plan. The target has to still be there, alive and within reach;
// anything else is a miss, which spends the attack's AP like the attack would
// have and is not reported as a refusal.
void attack( avatar &ava, const mp_plan &plan, const int cost )
{
    mp_interactive::clear_menu_cancelled();
    Creature *const target = get_creature_tracker().creature_at<Creature>( plan.target );
    if( target == nullptr || target->is_dead_state() || !ava.is_adjacent( target, false ) ) {
        ava.mod_moves( -cost );
        clamp_moves( ava );
        return;
    }
    ava.melee_attack( *target, true );
    if( mp_interactive::menu_cancelled() ) {
        mp_interactive::clear_menu_cancelled();
        const size_t slot = slot_of( ava );
        if( slot != no_slot ) {
            mp_plan_set_error( slot, interactive_choice_error );
        }
    }
    clamp_moves( ava );
}

// One plan in a round's cost group, with the order the round sorted it by.
// `cost` is the server's own figure, recomputed when the round is collected;
// the client's estimate never reaches this struct.
struct resolution_entry {
    size_t slot = 0;
    mp_plan plan;
    int cost = 0;
    int speed = 0;
};

// A player's step that is part of a group's move batch. `from` is captured
// before anything moves, because the batch evaluates every target against the
// cells the group started with.
struct move_claim {
    size_t slot = 0;
    tripoint_bub_ms from;
    tripoint_bub_ms target;
    int speed = 0;
    int cost = 0;
    bool okay = true;
    bool applied = false;
};

// Drops every claim that cannot win the pass. A cell goes to the fastest
// claimant, a speed tie lets nobody in, and a cell is enterable only when
// whoever holds it is itself leaving. Computed against a snapshot of the
// claims that stand at the start of the pass, so two claimants that tie both
// fail in the same pass -- evaluating them one after the other would let the
// second survive the first one's failure. Returns true when anything was
// dropped, which keeps the caller's fixpoint going.
bool fail_unwinnable_claims( std::vector<move_claim> &claims )
{
    std::vector<bool> fails( claims.size(), false );
    for( size_t i = 0; i < claims.size(); ++i ) {
        if( !claims[i].okay ) {
            continue;
        }
        const move_claim &claim = claims[i];
        bool vacated = false;
        for( const move_claim &other : claims ) {
            if( other.okay && other.from == claim.target ) {
                vacated = true;
                break;
            }
        }
        if( !vacated &&
            ( get_creature_tracker().creature_at<Creature>( claim.target ) != nullptr ||
              seat_at( claim.target, claim.slot ) ) ) {
            fails[i] = true;
        }
        int best_speed = std::numeric_limits<int>::min();
        int at_best = 0;
        for( const move_claim &other : claims ) {
            if( !other.okay || other.target != claim.target ) {
                continue;
            }
            if( other.speed > best_speed ) {
                best_speed = other.speed;
                at_best = 1;
            } else if( other.speed == best_speed ) {
                ++at_best;
            }
        }
        if( at_best != 1 || best_speed != claim.speed ) {
            fails[i] = true;
        }
    }
    bool changed = false;
    for( size_t i = 0; i < claims.size(); ++i ) {
        if( fails[i] && claims[i].okay ) {
            claims[i].okay = false;
            changed = true;
        }
    }
    return changed;
}

// Settles the group's player steps before any of them is applied: the fixpoint
// of "a cell is free once its occupant's step is itself going through", with
// contention between claimants. Losing a claim is not a refusal -- the move is
// still charged -- and a chain applies from its tail, so the head walks into a
// cell its owner has already left.
void resolve_moves( const std::vector<resolution_entry> &entries, const size_t begin,
                    const size_t end )
{
    std::vector<move_claim> claims;
    for( size_t i = begin; i < end; ++i ) {
        if( entries[i].plan.kind != mp_plan_kind::move ) {
            continue;
        }
        const size_t slot = entries[i].slot;
        avatar &ava = g->player_at( slot );
        if( ava.is_dead_state() ) {
            continue;
        }
        move_claim claim;
        claim.slot = slot;
        claim.from = ava.pos_bub();
        claim.target = entries[i].plan.target;
        claim.speed = entries[i].speed;
        if( claim.from.z() != claim.target.z() ||
            rl_dist( claim.from, claim.target ) != 1 ) {
            // The tile is no longer one step away: the world moved under the
            // plan. It costs what it would have cost, unless the seat cannot
            // pay that either -- then it is refused like any other action.
            int cost = 0;
            if( affordable( slot, ava, entries[i].plan, cost ) ) {
                ava.mod_moves( -cost );
            }
            continue;
        }
        if( !affordable( slot, ava, entries[i].plan, claim.cost ) ) {
            // Not a move this round: no AP spent, and no claim to a cell.
            continue;
        }
        claims.push_back( claim );
    }

    while( fail_unwinnable_claims( claims ) ) {
        // Each pass drops at least one claim, so this terminates.
    }
    size_t remaining = 0;
    for( const move_claim &claim : claims ) {
        remaining += claim.okay ? 1 : 0;
    }
    while( remaining > 0 ) {
        bool progress = false;
        for( move_claim &claim : claims ) {
            if( !claim.okay || claim.applied ) {
                continue;
            }
            bool blocked = false;
            for( const move_claim &other : claims ) {
                if( &other == &claim || !other.okay || other.applied ) {
                    continue;
                }
                if( other.from == claim.target ) {
                    blocked = true;
                    break;
                }
            }
            if( blocked ||
                get_creature_tracker().creature_at<Creature>( claim.target ) != nullptr ||
                seat_at( claim.target, claim.slot ) ) {
                continue;
            }
            mp_activate_player( claim.slot );
            step( g->player_at( claim.slot ), claim.target, claim.cost );
            mp_activate_world();
            claim.applied = true;
            --remaining;
            progress = true;
        }
        if( !progress ) {
            // Whatever is left depends on a cell that never frees: that is the
            // cycle v1 refuses, a two-seat swap included.
            break;
        }
    }
    for( const move_claim &claim : claims ) {
        if( !claim.applied ) {
            // Lost arbitration, or left in a cycle: the round's step is spent
            // even though it was never taken.
            g->player_at( claim.slot ).mod_moves( -claim.cost );
        }
    }
}

// The passes single player runs between actions: the dead are removed, people
// fall, explosions land, and the seats that acted get to hear what happened.
void post_group_cleanup( const std::vector<resolution_entry> &entries, const size_t begin,
                         const size_t end )
{
    g->cleanup_dead();
    get_map().process_falling();
    explosion_handler::process_explosions();
    for( size_t i = begin; i < end; ++i ) {
        const size_t slot = entries[i].slot;
        if( g->player_at( slot ).is_dead_state() ) {
            continue;
        }
        mp_activate_player( slot );
        sounds::process_sound_markers( &g->player_at( slot ) );
        mp_activate_world();
    }
}
} // namespace

void mp_apply_plan( avatar &ava, const mp_plan &plan )
{
    // Direct callers (a test, or a future single-plan path) get the same rule
    // as the round resolver: no engine call here may wait for a human.
    const mp_interactive::scope no_prompts;
    if( ava.is_dead_state() ) {
        return;
    }
    const size_t slot = slot_of( ava );
    int cost = 0;
    switch( plan.kind ) {
        case mp_plan_kind::none:
        case mp_plan_kind::pass:
            // A pass already spent everything when it was made.
            return;
        case mp_plan_kind::move:
            if( !affordable( slot, ava, plan, cost ) ) {
                return;
            }
            step( ava, plan.target, cost );
            return;
        case mp_plan_kind::melee:
            if( !affordable( slot, ava, plan, cost ) ) {
                return;
            }
            attack( ava, plan, cost );
            return;
        case mp_plan_kind::ranged:
        case mp_plan_kind::use_item:
        case mp_plan_kind::interact:
            // Phase 1 cannot run these. They spend nothing.
            if( slot != no_slot ) {
                mp_plan_set_error( slot, "not in this phase" );
            }
            return;
    }
}

bool mp_resolve_round( const size_t player_count )
{
    // No engine call in the resolver may wait for a human: the world thread
    // holds the baton, and every player is parked on it. Prompt answers and a
    // cancelled menu (recorded per action) take the place of the questions.
    const mp_interactive::scope no_prompts;

    mp_plan_set_phase( mp_round_phase::resolving );

    std::vector<resolution_entry> entries;
    const size_t slots = std::min( player_count, g->player_slot_count() );
    for( size_t slot = 0; slot < slots; ++slot ) {
        if( !mp_plan_is_ready( slot ) ) {
            continue;
        }
        avatar &ava = g->player_at( slot );
        if( ava.is_dead_state() ) {
            // A dead seat's plan is dropped and costs nothing.
            continue;
        }
        if( mp_player_is_disconnected( slot ) ) {
            // Defense in depth: mp_server_mark_disconnected() cancels the plan
            // when the wire dies, and a seat with nobody behind it must not act
            // even if one slipped through.
            continue;
        }
        // Copy: the plan table can grow between reads, and this round works
        // from one snapshot anyway.
        mp_plan plan = mp_plan_of( slot );
        if( plan.kind == mp_plan_kind::none || plan.kind == mp_plan_kind::pass ) {
            continue;
        }
        resolution_entry entry;
        entry.slot = slot;
        entry.plan = std::move( plan );
        // The same server figure the party publishes as `pcost`, and the same
        // one the affordability gate re-checks at apply time. The client's
        // plan.cost is display-only and never orders or groups a round.
        entry.cost = std::max( 0, mp_plan_cost( slot, entry.plan ) );
        entry.speed = ava.get_speed();
        entries.push_back( std::move( entry ) );
    }

    std::sort( entries.begin(), entries.end(),
    []( const resolution_entry & l, const resolution_entry & r ) {
        if( l.cost != r.cost ) {
            return l.cost < r.cost;
        }
        if( l.speed != r.speed ) {
            return l.speed > r.speed;
        }
        return l.slot < r.slot;
    } );

    for( size_t begin = 0; begin < entries.size(); ) {
        size_t end = begin + 1;
        while( end < entries.size() && entries[end].cost == entries[begin].cost ) {
            ++end;
        }
        // Movement first, as one batch: the players' steps contend for cells
        // and have to be settled together.
        resolve_moves( entries, begin, end );
        for( size_t i = begin; i < end; ++i ) {
            if( entries[i].plan.kind == mp_plan_kind::move ) {
                continue;
            }
            const size_t slot = entries[i].slot;
            if( g->player_at( slot ).is_dead_state() ) {
                continue;
            }
            mp_activate_player( slot );
            mp_apply_plan( g->player_at( slot ), entries[i].plan );
            mp_activate_world();
        }
        post_group_cleanup( entries, begin, end );
        begin = end;
    }
    mp_activate_world();

    // Every seat has had its round's worth of actions; now the monsters and
    // NPCs take theirs before the next round opens. Whether that step made
    // progress is the caller's continuation input: a creature-only round
    // follows only while the creatures are still getting somewhere.
    const bool creature_progress = mon_round_step();
    mp_plan_set_phase( mp_round_phase::done );
    return creature_progress;
}

#endif // MULTIPLAYER
