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
