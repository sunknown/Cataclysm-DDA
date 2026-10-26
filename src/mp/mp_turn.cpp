#include "mp/mp_turn.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <vector>

#include "avatar.h"
#include "game.h"
#include "map.h"
#include "monster.h"
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

// When each countdown started, or nullopt when it is not running. A time point
// rather than a remaining count: the orchestrator polls at whatever rate the
// session runs at, and a decrementing counter would drift with it.
//
// One per trigger, so that arming one can never swallow the other. They share
// one flag once, and whichever fired first silently turned the other trigger
// into a no-op (see mp_turn.h for the semantics).
using hurry_time_point = std::chrono::steady_clock::time_point;
std::optional<hurry_time_point> g_hurry_manual_started;
std::optional<hurry_time_point> g_hurry_combat_started;

std::optional<hurry_time_point> &started_of( const mp_hurry_source source )
{
    return source == mp_hurry_source::manual ? g_hurry_manual_started : g_hurry_combat_started;
}

int length_ms_of( const mp_hurry_source source )
{
    return source == mp_hurry_source::manual ? MP_HURRY_MS : MP_HURRY_COMBAT_MS;
}

// Milliseconds left on one countdown, or -1 when it is not running.
int remaining_ms_of( const mp_hurry_source source )
{
    const std::optional<hurry_time_point> &started = started_of( source );
    if( !started ) {
        return -1;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - *started ).count();
    const int left = length_ms_of( source ) - static_cast<int>( elapsed );
    return left > 0 ? left : 0;
}

void clear_hurry_countdowns()
{
    g_hurry_manual_started.reset();
    g_hurry_combat_started.reset();
}

// True while a hostile creature inside the bubble can see any player.
bool anyone_is_seen_by_a_hostile()
{
    for( Creature &critter : g->all_creatures() ) {
        monster *const mon = dynamic_cast<monster *>( &critter );
        if( mon == nullptr || mon->friendly != 0 ) {
            continue;
        }
        for( size_t i = 0; i < g->player_slot_count(); ++i ) {
            if( mon->sees( get_map(), g->player_at( i ) ) ) {
                return true;
            }
        }
    }
    return false;
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
        // (see the header).
        if( i >= g_waiting_for_input.size() || !g_waiting_for_input[i] ) {
            g_dropped_by_countdown[i] = false;
        }
    }
    clear_hurry_countdowns();
}

void mp_turn_reset_all()
{
    g_waiting_for_input.assign( g->player_slot_count(), false );
    g_dropped_by_countdown.assign( g->player_slot_count(), false );
    g_turn_open = false;
    g_session_end = false;
    clear_hurry_countdowns();
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

void mp_request_hurry()
{
    if( g_hurry_manual_started ) {
        return;
    }
    g_hurry_manual_started = std::chrono::steady_clock::now();
}

int mp_hurry_remaining_ms( const mp_hurry_source source )
{
    return remaining_ms_of( source );
}

int mp_hurry_remaining_ms()
{
    const int manual = remaining_ms_of( mp_hurry_source::manual );
    const int combat = remaining_ms_of( mp_hurry_source::combat );
    if( manual < 0 || combat < 0 ) {
        return std::max( manual, combat );
    }
    return std::min( manual, combat );
}

void mp_advance_hurry_for_test( const int ms, const mp_hurry_source source )
{
    std::optional<hurry_time_point> &started = started_of( source );
    if( started ) {
        *started -= std::chrono::milliseconds( ms );
    }
}

void mp_advance_hurry_for_test( const int ms )
{
    mp_advance_hurry_for_test( ms, mp_hurry_source::manual );
    mp_advance_hurry_for_test( ms, mp_hurry_source::combat );
}

void mp_tick_hurry()
{
    ensure_sized();
    // Nobody spends a keystroke on "hurry up" when something is running at
    // them, so combat starts its own countdown, and a shorter one. It does so
    // whether or not somebody already pressed the key: a press that came first
    // must not make combat urgency wait out the longer countdown.
    if( !g_hurry_combat_started && anyone_is_seen_by_a_hostile() ) {
        g_hurry_combat_started = std::chrono::steady_clock::now();
    }
    for( const mp_hurry_source source : { mp_hurry_source::manual, mp_hurry_source::combat } ) {
        if( remaining_ms_of( source ) != 0 ) {
            continue;
        }
        // Expired. Drop everyone still holding -- for this turn only. Their UI
        // is untouched: their character stands still and skips the turn, their
        // menu stays open, and they confirm their choice when they are ready.
        for( size_t i = 0; i < g->player_slot_count(); ++i ) {
            if( mp_player_state_of( i ) == mp_player_state::holding ) {
                g_dropped_by_countdown[i] = true;
            }
        }
        // Only the one that expired: the other is still somebody's request.
        started_of( source ).reset();
    }
}

void mp_forfeit_dropped_moves()
{
    ensure_sized();
    for( size_t i = 0; i < g->player_slot_count(); ++i ) {
        if( mp_player_state_of( i ) == mp_player_state::not_waited ) {
            // Standing still is what the drop means. Taken at the end of the
            // turn rather than at the moment of the drop, because a drop lasts
            // as long as the player sits in the same menu while every closed
            // turn hands out another turn's worth of moves.
            g->player_at( i ).set_moves( 0 );
        }
    }
}

bool mp_action_is_free( const action_id act )
{
    switch( act ) {
        // Reading and looking: what a player actually wants to do while their
        // partner thinks.
        case ACTION_INVENTORY:
        case ACTION_PL_INFO:
        case ACTION_MAP:
        case ACTION_SKY:
        case ACTION_LOOK:
        case ACTION_MESSAGES:
        case ACTION_MISSIONS:
        case ACTION_FACTIONS:
        case ACTION_MORALE:
        case ACTION_MEDICAL:
        // Client-side and bookkeeping.
        case ACTION_OPTIONS:
        case ACTION_HELP:
        case ACTION_KEYBINDINGS:
        case ACTION_ZOOM_IN:
        case ACTION_ZOOM_OUT:
        case ACTION_TOGGLE_PANEL_ADM:
        case ACTION_TOGGLE_SAFEMODE:
        case ACTION_TOGGLE_AUTOSAFE:
        // The escape hatch itself, and leaving.
        case ACTION_COOP_HURRY:
        case ACTION_SAVE:
            return true;
        default:
            // Everything else costs time, or might. Refusing is the safe
            // direction: a gap here is an annoyance, a gap the other way is a
            // free action outside the turn.
            return false;
    }
}
