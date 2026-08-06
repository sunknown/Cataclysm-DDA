#include "mp/mp_hotseat_loop.h"

#include "avatar.h"
#include "color.h"
#include "game.h"
#include "map.h"
#include "messages.h"
#include "mp/mp_context.h"
#include "mp/mp_tether.h"
#include "translations.h"

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
        // mp_activate_player() is the single alias-swap point: it swaps the
        // acting slot's data into `u` for the duration of its turn (a no-op
        // for slot 0, which already lives there), re-points the context
        // aliases (stdscr, windows, metrics, ui stack, active context) and
        // sets the active slot. The next iteration's activation swaps the
        // previous slot back out.
        mp_activate_player( i );
        avatar &who = g->player_at( i );
        while( who.get_moves() > 0 && who.activity ) {
            who.activity.do_turn( who );
        }
        // Deliberately no "now controlling" message here. Control alternates
        // every single turn, so any per-turn message buries the log within
        // seconds of play. The active slot is shown in the sidebar instead --
        // see display::hotseat_player_text() and the hotseat_player_desc
        // widget.
        bool warned = false;
        while( who.get_moves() > 0 ) {
            const tripoint_abs_ms pos_before = who.pos_abs();
            // handle_action() returning false means no game time was spent
            // (a menu, looking around, keybindings, etc.) -- section 5.5's
            // reference (do_turn.cpp's single-player loop) does not end the
            // turn on that, it just asks for another action. Breaking here
            // ended the slot's turn on the first free action taken, leaving
            // its moves unspent and letting them accumulate turn after turn.
            // Quitting/saving still terminates this loop: both explicitly
            // zero the active avatar's moves before returning.
            if( !g->handle_action() ) {
                continue;
            }
            who.action_taken();
            // Tether (section 5.7 of the design spec): an outward step past the
            // hard limit is undone and ends the slot's turn; the warning band is
            // reported once per turn. Intercepted here rather than in
            // avatar_action::move so the single-player path is untouched.
            const tether_state tether = mp_tether_check( who.pos_abs() );
            if( tether == tether_state::blocked ) {
                who.setpos( get_map(), get_map().get_bub( pos_before ) );
                add_msg( m_bad, _( "You cannot go that far from your partner." ) );
                who.set_moves( 0 );
                break;
            }
            if( tether == tether_state::warning && !warned ) {
                add_msg( m_warning, _( "You are getting far from your partner." ) );
                warned = true;
            }
        }
        // No swap back here: the next iteration's mp_activate_player() (or the
        // mp_activate_player( 0 ) below) takes this slot out of `u`.
    }
    mp_activate_player( 0 );

    // World phase: run the single-player turn machinery. Every slot was
    // drained above, so do_turn()'s input loops are no-ops and its interactive
    // wait branch is inert (no slot is left with a pending activity or
    // destination). This advances the calendar, simulates monsters/NPCs/fields,
    // and replenishes slot 0's moves exactly as in single player.
    const bool result = g->do_turn();

    // do_turn() only replenishes the active slot (slot 0). Every other slot
    // needs the same per-character turn processing (move regeneration,
    // suffering, speed-bonus recalculation) or it permanently runs out of
    // moves after its first turn and becomes an unplayable spectator.
    for( size_t i = 1; i < g->player_slot_count(); ++i ) {
        g->player_at( i ).process_turn();
    }

    return result;
}
