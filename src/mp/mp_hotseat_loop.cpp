#include "mp/mp_hotseat_loop.h"

#include "avatar.h"
#include "game.h"

bool mp_hotseat_do_turn()
{
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
