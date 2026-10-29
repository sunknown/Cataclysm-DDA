#include "mp/mp_tether.h"

#include "avatar.h"
#include "game.h"

tripoint_abs_ms mp_bubble_anchor()
{
    // Integer arithmetic only: floating point midpoints would differ between
    // builds, which section 7.1 of the spec forbids in simulation paths.
    int64_t sx = 0;
    int64_t sy = 0;
    int count = 0;
    for( avatar *who : g->get_players() ) {
        if( who->is_dead_state() ) {
            continue;
        }
        sx += who->pos_abs().x();
        sy += who->pos_abs().y();
        ++count;
    }
    if( count == 0 ) {
        return g->player_at( 0 ).pos_abs();
    }
    // Anchor z follows slot 0, per section 5.7 of the spec.
    return tripoint_abs_ms( static_cast<int>( sx / count ),
                            static_cast<int>( sy / count ),
                            g->player_at( 0 ).pos_abs().z() );
}

// Section 5.7 of the design spec: the bubble is 132x132 tiles, so +/-66 from
// the anchor. Twenty four tiles of margin are reserved because monsters and
// fields at the bubble edge are not simulated correctly, leaving 42.
static constexpr int tether_hard_limit = 42;

tether_state mp_tether_check( const tripoint_abs_ms &proposed )
{
    const int d = rl_dist( mp_bubble_anchor(), proposed );
    if( d > tether_hard_limit ) {
        return tether_state::blocked;
    }
    if( d > MP_TETHER_SOFT_LIMIT ) {
        return tether_state::warning;
    }
    return tether_state::ok;
}
