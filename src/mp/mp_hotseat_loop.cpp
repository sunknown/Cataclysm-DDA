#include "mp/mp_hotseat_loop.h"

#include "avatar.h"
#include "game.h"
#include "map.h"
#include "mp/mp_tether.h"

bool mp_hotseat_do_turn()
{
    // Centre the reality bubble on the midpoint between the players before
    // anyone acts, so both stay inside the simulated area, and keep it on the
    // anchor's z-level (slot 0's z, per section 5.7 of the spec).
    const tripoint_abs_ms anchor = mp_bubble_anchor();
    g->shift_map_center( anchor );
    if( anchor.z() != get_map().get_abs_sub().z() ) {
        g->shift_map_z( anchor.z() );
    }

    // Player phase: every slot acts in index order, never in speed order, so
    // the sequence is reproducible and matches section 5.5 of the design spec.
    for( size_t i = 0; i < g->player_slot_count(); ++i ) {
        g->set_active_slot( i );
        avatar &who = g->player_at( i );
        while( who.get_moves() > 0 && who.activity ) {
            who.activity.do_turn( who );
        }
        while( who.get_moves() > 0 ) {
            if( !g->handle_action() ) {
                break;
            }
            who.action_taken();
        }
    }
    g->set_active_slot( 0 );

    // World phase: run the single-player turn machinery. Every slot was
    // drained above, so do_turn()'s input loops are no-ops and its interactive
    // wait branch is inert (no slot is left with a pending activity or
    // destination). This advances the calendar, simulates monsters/NPCs/fields,
    // and replenishes slot 0's moves exactly as in single player.
    return g->do_turn();
}
