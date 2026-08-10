#include "mp/mp_turn.h"

#include <vector>

#include "avatar.h"
#include "game.h"
#include "player_activity.h"

namespace
{
// Barrier bookkeeping, indexed by slot. Not part of player_context: it is state
// about the turn, not about the player's display. What belongs to one turn and
// what outlives it is spelled out at mp_turn_reset in the header.
std::vector<bool> g_waiting_for_input;
std::vector<bool> g_dropped_by_countdown;
// Whether a turn is currently open (see mp_turn_is_open in the header).
bool g_turn_open = false;
// Whether a player asked for the session to end (see mp_request_session_end).
bool g_session_end = false;

void ensure_sized()
{
    const size_t n = g->player_slot_count();
    g_waiting_for_input.resize( n, false );
    g_dropped_by_countdown.resize( n, false );
}
} // namespace

mp_player_state mp_player_state_of( const size_t slot )
{
    ensure_sized();
    if( slot < g_dropped_by_countdown.size() && g_dropped_by_countdown[slot] ) {
        return mp_player_state::not_waited;
    }
    avatar &ava = g->player_at( slot );
    // "Occupied" outranks "has moves": an activity or a set destination will
    // spend this turn without the player touching a key, so making the barrier
    // wait on them would freeze everybody for no reason.
    if( ava.activity || ava.has_distant_destination() ) {
        return mp_player_state::acted;
    }
    if( ava.get_moves() <= 0 ) {
        return mp_player_state::acted;
    }
    if( slot < g_waiting_for_input.size() && g_waiting_for_input[slot] ) {
        return mp_player_state::holding;
    }
    return mp_player_state::acting;
}

bool mp_player_holds_barrier( const size_t slot )
{
    return mp_player_state_of( slot ) == mp_player_state::holding;
}

bool mp_barrier_satisfied()
{
    ensure_sized();
    for( size_t i = 0; i < g->player_slot_count(); ++i ) {
        if( g->player_at( i ).is_dead_state() ) {
            continue;
        }
        const mp_player_state state = mp_player_state_of( i );
        if( state != mp_player_state::acted && state != mp_player_state::not_waited ) {
            return false;
        }
    }
    return true;
}

void mp_set_waiting_for_input( const size_t slot, const bool waiting )
{
    ensure_sized();
    if( slot < g_waiting_for_input.size() ) {
        g_waiting_for_input[slot] = waiting;
    }
}

void mp_turn_reset()
{
    ensure_sized();
    for( size_t i = 0; i < g_dropped_by_countdown.size(); ++i ) {
        // A drop is released only by its own player moving on. Somebody who is
        // still sitting at the same input wait stays dropped, or one press of
        // "hurry up" would buy exactly one tick and the next turn would stop on
        // them again. The input-wait flags themselves are never cleared here
        // (see the header). The hurry countdown is reset in task 8.
        if( i >= g_waiting_for_input.size() || !g_waiting_for_input[i] ) {
            g_dropped_by_countdown[i] = false;
        }
    }
}

void mp_turn_reset_all()
{
    g_waiting_for_input.assign( g->player_slot_count(), false );
    g_dropped_by_countdown.assign( g->player_slot_count(), false );
    g_turn_open = false;
    g_session_end = false;
}

bool mp_turn_is_open()
{
    return g_turn_open;
}

void mp_set_turn_open( const bool open )
{
    g_turn_open = open;
}

void mp_request_session_end()
{
    g_session_end = true;
}

bool mp_session_end_requested()
{
    return g_session_end;
}
