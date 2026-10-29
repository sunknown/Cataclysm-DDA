#include "mp/mp_placement.h"

#include <algorithm>
#include <cstdlib>

#include "avatar.h"
#include "cata_assert.h"
#include "creature_tracker.h"
#include "debug.h"
#include "game.h"
#include "item.h"
#include "line.h"
#include "map.h"
#include "mp/mp_context.h"
#include "mp/mp_tether.h"
#include "scenario.h"
#include "string_id.h"
#include "visitable.h"

// The tiles at exactly Chebyshev distance `radius` from `centre`, on its
// z-level. Radius zero is the centre itself.
static std::vector<tripoint_abs_ms> ring_around( const tripoint_abs_ms &centre, const int radius )
{
    std::vector<tripoint_abs_ms> ring;
    if( radius == 0 ) {
        ring.push_back( centre );
        return ring;
    }
    for( int dx = -radius; dx <= radius; ++dx ) {
        for( int dy = -radius; dy <= radius; ++dy ) {
            if( std::max( std::abs( dx ), std::abs( dy ) ) == radius ) {
                ring.push_back( centre + tripoint( dx, dy, 0 ) );
            }
        }
    }
    return ring;
}

static bool a_player_stands_on( const tripoint_abs_ms &p )
{
    for( size_t i = 0; i < g->player_slot_count(); ++i ) {
        if( g->player_at( i ).pos_abs() == p ) {
            return true;
        }
    }
    return false;
}

std::optional<tripoint_abs_ms> mp_find_free_tile_near( const tripoint_abs_ms &target )
{
    map &here = get_map();
    creature_tracker &creatures = get_creature_tracker();
    // Radius zero first: the target itself is a legal answer when nobody is on it.
    for( int radius = 0; radius <= MP_TETHER_SOFT_LIMIT; ++radius ) {
        for( const tripoint_abs_ms &candidate : ring_around( target, radius ) ) {
            const tripoint_bub_ms bub = here.get_bub( candidate );
            if( !here.inbounds( bub ) || !here.passable( bub ) ) {
                continue;
            }
            if( creatures.creature_at( candidate ) != nullptr || a_player_stands_on( candidate ) ) {
                continue;
            }
            return candidate;
        }
    }
    // Exhausted the tether's soft limit. Placing further out would just make the
    // tether complain on the first turn.
    return std::nullopt;
}

bool place_new_character( avatar &ava, const std::optional<tripoint_abs_ms> near )
{
    if( !near ) {
        // Empty world: this character builds it. start_game() works on `u`.
        cata_assert( &ava == &get_avatar() );
        return g->start_game();
    }

    g->prepare_new_character( ava );
    const std::optional<tripoint_abs_ms> spot = mp_find_free_tile_near( *near );
    if( !spot ) {
        return false;
    }
    ava.setpos( *spot );
    // Starting items remember where they spawned; the map needs that to read them.
    const tripoint_abs_ms pos = ava.pos_abs();
    ava.visit_items( [&pos]( item * it, item * ) {
        it->preserve_location( pos );
        return VisitResponse::NEXT;
    } );
    g->finish_new_character( ava );
    return true;
}

bool mp_place_roster( const std::vector<bool> &is_new, std::vector<size_t> &moved,
                      const std::optional<tripoint_abs_ms> world_anchor )
{
    cata_assert( is_new.size() == g->player_slot_count() );
    moved.clear();

    // Pass 1: continuing characters keep the position from their own save. The
    // first of them anchors everybody else.
    std::optional<size_t> anchor;
    for( size_t i = 0; i < is_new.size() && !anchor; ++i ) {
        if( !is_new[i] ) {
            anchor = i;
        }
    }

    // Continuations played solo far from the anchor. One bubble is 132x132
    // tiles, so they are brought together, or the tether objects before the
    // first turn.
    if( anchor ) {
        const tripoint_abs_ms anchor_pos = g->player_at( *anchor ).pos_abs();
        for( size_t i = 0; i < is_new.size(); ++i ) {
            if( is_new[i] || i == *anchor ||
                rl_dist( g->player_at( i ).pos_abs(), anchor_pos ) <= MP_TETHER_SOFT_LIMIT ) {
                continue;
            }
            const std::optional<tripoint_abs_ms> spot = mp_find_free_tile_near( anchor_pos );
            if( !spot ) {
                return false;
            }
            g->player_at( i ).setpos( *spot );
            moved.push_back( i );
        }
    }

    // Pass 2: new characters, each under its own context -- the empty-world
    // path builds the world around `u`. Without any anchor the first of them
    // builds the world and becomes one.
    const bool restore_world = mp_in_world_phase();
    const size_t restore_slot = g->active_slot();
    bool placed = true;
    for( size_t i = 0; i < is_new.size() && placed; ++i ) {
        if( !is_new[i] ) {
            continue;
        }
        mp_activate_player( i );
        std::optional<tripoint_abs_ms> near = world_anchor;
        if( anchor ) {
            near = g->player_at( *anchor ).pos_abs();
        }
        placed = place_new_character( g->player_at( i ), near );
        if( placed && !anchor ) {
            anchor = i;
        }
    }
    if( restore_world ) {
        mp_activate_world();
    } else {
        mp_activate_player( restore_slot );
    }
    return placed;
}

const scenario *mp_coop_scenario()
{
    static const string_id<scenario> coop_id( "coop_survivors" );
    if( !coop_id.is_valid() ) {
        // Content and code disagree -- the JSON was renamed or dropped. Loud,
        // because the alternative is characters starting under whatever
        // scenario was selected and landing across the map from each other.
        debugmsg( "the co-op scenario \"%s\" is missing from the JSON", coop_id.str() );
        return nullptr;
    }
    return &coop_id.obj();
}
