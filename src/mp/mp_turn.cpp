#include "mp/mp_turn.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "avatar.h"
#include "debug.h"
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
// Players who left the keyboard on purpose (see mp_set_away in the header).
// Stored, unlike the derived states, because nothing in game state separates
// them from a player sitting at a menu.
std::vector<bool> g_away;
// Players whose client is gone (see mp_set_disconnected in the header). Also
// stored because no game state separates a lost client from a player at a
// menu.
std::vector<bool> g_disconnected;
// The seat the one process-wide `uquit` currently belongs to, or nullopt when
// no quit verdict is in flight. `uquit` is a single global, but a session has
// several players whose turns are interleaved around input waits: without an
// owner, one player's "save and quit" or death is read as every player's (see
// mp_claim_quit_latch in the header).
std::optional<size_t> g_quit_latch_owner;
// Slots whose corpse has already been placed. The orchestrator's death scan
// and game::is_game_over() both notice the same death; this is the per-seat
// fact that keeps the second observer from duplicating the corpse.
std::vector<bool> g_corpse_placed;
// Whether a turn is currently open (see mp_turn_is_open in the header), and
// since when (see mp_turn_paced).
bool g_turn_open = false;
std::chrono::steady_clock::time_point g_turn_opened_at;
// Whether a player asked for the session to end, and why (see
// mp_request_session_end).
bool g_session_end = false;
std::string g_session_end_reason;

void ensure_sized()
{
    const size_t n = g->player_slot_count();
    g_waiting_for_input.resize( n, false );
    g_dropped_by_countdown.resize( n, false );
    g_away.resize( n, false );
    g_disconnected.resize( n, false );
    g_corpse_placed.resize( n, false );
}

// When each countdown started, or nullopt when it is not running. A time point
// rather than a remaining count: the orchestrator polls at whatever rate the
// session runs at, and a decrementing counter would drift with it.
//
// One per trigger, so that arming one can never swallow the other. They share
// one flag once, and whichever fired first silently turned the other trigger
// into a no-op (see mp_turn.h for the semantics).
using hurry_time_point = std::chrono::steady_clock::time_point;

// The range a configured countdown must fall in, in milliseconds. Inclusive.
// Outside it a value is refused rather than clamped: silently substituting a
// length for the operator's is a decision they did not make.
constexpr int MP_HURRY_MIN_MS = 1000;
constexpr int MP_HURRY_MAX_MS = 600000;
std::optional<hurry_time_point> g_hurry_manual_started;
std::optional<hurry_time_point> g_hurry_combat_started;

// How long each countdown runs. The constants above are their defaults; a
// server operator overrides them for one session through
// mp_set_hurry_lengths(), and mp_turn_reset_all() puts the defaults back.
int g_hurry_ms = MP_HURRY_MS;
int g_hurry_combat_ms = MP_HURRY_COMBAT_MS;

std::optional<hurry_time_point> &started_of( const mp_hurry_source source )
{
    return source == mp_hurry_source::manual ? g_hurry_manual_started : g_hurry_combat_started;
}

int length_ms_of( const mp_hurry_source source )
{
    return source == mp_hurry_source::manual ? g_hurry_ms : g_hurry_combat_ms;
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
    // Checked first: a player who left the keyboard is not holding and not
    // acting, whatever their moves and activity say.
    if( slot < g_away.size() && g_away[slot] ) {
        return mp_player_state::away;
    }
    // Checked after away: stepping away is the player's own choice and
    // outranks a server-side disconnect. A disconnected client holds nothing
    // either.
    if( slot < g_disconnected.size() && g_disconnected[slot] ) {
        return mp_player_state::disconnected;
    }
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
        if( state != mp_player_state::acted && state != mp_player_state::not_waited &&
            state != mp_player_state::away && state != mp_player_state::disconnected ) {
            return false;
        }
    }
    return true;
}

bool mp_turn_all_away()
{
    ensure_sized();
    for( size_t i = 0; i < g->player_slot_count(); ++i ) {
        if( g->player_at( i ).is_dead_state() ) {
            continue;
        }
        if( !mp_player_is_away( i ) && !mp_player_is_disconnected( i ) ) {
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

void mp_set_away( const size_t slot, const bool away )
{
    ensure_sized();
    if( slot < g_away.size() ) {
        g_away[slot] = away;
    }
}

bool mp_player_is_away( const size_t slot )
{
    ensure_sized();
    return slot < g_away.size() && g_away[slot];
}

void mp_return_from_away( const size_t slot )
{
    mp_set_away( slot, false );
}

void mp_set_disconnected( const size_t slot, const bool disconnected )
{
    ensure_sized();
    if( slot < g_disconnected.size() ) {
        g_disconnected[slot] = disconnected;
    }
}

bool mp_player_is_disconnected( const size_t slot )
{
    ensure_sized();
    return slot < g_disconnected.size() && g_disconnected[slot];
}

void mp_claim_quit_latch( const size_t slot )
{
    if( slot >= g->player_slot_count() || g_quit_latch_owner ) {
        return;
    }
    g_quit_latch_owner = slot;
}

bool mp_player_owns_quit_latch( const size_t slot )
{
    return g_quit_latch_owner && *g_quit_latch_owner == slot;
}

void mp_clear_quit_latch( const size_t slot )
{
    // Only the owner can release the latch. A non-owner calling this is a
    // no-op rather than a way to steal somebody else's quit verdict.
    if( mp_player_owns_quit_latch( slot ) ) {
        g_quit_latch_owner.reset();
    }
}

bool mp_player_corpse_placed( const size_t slot )
{
    ensure_sized();
    return slot < g_corpse_placed.size() && g_corpse_placed[slot];
}

void mp_note_corpse_placed( const size_t slot )
{
    ensure_sized();
    if( slot < g_corpse_placed.size() ) {
        g_corpse_placed[slot] = true;
    }
}

void mp_clear_corpse_placed( const size_t slot )
{
    ensure_sized();
    if( slot < g_corpse_placed.size() ) {
        g_corpse_placed[slot] = false;
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
    // g_away is deliberately untouched: leaving the keyboard is the player's
    // own decision and lasts until they come back (see the header). So is
    // g_disconnected: a lost client is still lost on the next turn.
    clear_hurry_countdowns();
}

void mp_turn_reset_all()
{
    g_waiting_for_input.assign( g->player_slot_count(), false );
    g_dropped_by_countdown.assign( g->player_slot_count(), false );
    g_away.assign( g->player_slot_count(), false );
    g_disconnected.assign( g->player_slot_count(), false );
    g_quit_latch_owner.reset();
    g_corpse_placed.assign( g->player_slot_count(), false );
    g_turn_open = false;
    g_session_end = false;
    g_session_end_reason.clear();
    clear_hurry_countdowns();
    // The countdown lengths are the operator's, so this is where a configured
    // session stops and the next one starts from the shipped values.
    g_hurry_ms = MP_HURRY_MS;
    g_hurry_combat_ms = MP_HURRY_COMBAT_MS;
}

bool mp_set_hurry_lengths( const int manual_ms, const int combat_ms )
{
    // Both are checked before either is written: a half-applied pair would be
    // a session nobody asked for.
    if( manual_ms < MP_HURRY_MIN_MS || manual_ms > MP_HURRY_MAX_MS ||
        combat_ms < MP_HURRY_MIN_MS || combat_ms > MP_HURRY_MAX_MS ) {
        DebugLog( D_ERROR, D_MAIN ) << "mp hurry lengths refused: manual "
                                    << manual_ms << " ms, combat " << combat_ms
                                    << " ms; both must be within ["
                                    << MP_HURRY_MIN_MS << ", " << MP_HURRY_MAX_MS << "]";
        return false;
    }
    g_hurry_ms = manual_ms;
    g_hurry_combat_ms = combat_ms;
    return true;
}

bool mp_turn_is_open()
{
    return g_turn_open;
}

void mp_set_turn_open( const bool open )
{
    g_turn_open = open;
    if( open ) {
        g_turn_opened_at = std::chrono::steady_clock::now();
    }
}

bool mp_player_left_behind( const size_t slot )
{
    ensure_sized();
    return slot < g_dropped_by_countdown.size() && g_dropped_by_countdown[slot] &&
           g_waiting_for_input[slot];
}

bool mp_player_is_away_at_input_wait( const size_t slot )
{
    ensure_sized();
    return g_away[slot] && g_waiting_for_input[slot];
}

bool mp_turn_paced()
{
    ensure_sized();
    // A player at the keyboard is the clock: their actions pace the world, and
    // nothing here needs a floor.
    bool anyone_left_behind = false;
    for( size_t i = 0; i < g->player_slot_count(); ++i ) {
        avatar &ava = g->player_at( i );
        if( ava.is_dead_state() ) {
            continue;
        }
        // Away first, before the left-behind rule: an away player asked not to
        // be waited on, and a drop they collected before leaving would
        // otherwise floor every turn at MP_HURRY_MS for a partner who is busy
        // -- the wait this phase exists to remove. A disconnected seat is the
        // same absence from the other side: nobody is at that keyboard, and
        // counting it as a player at the keyboard switched the floor off for a
        // partner who was still reading after a hurry drop -- the free run.
        if( mp_player_is_away( i ) || mp_player_is_disconnected( i ) ) {
            continue;
        }
        if( mp_player_left_behind( i ) ) {
            anyone_left_behind = true;
            continue;
        }
        if( !ava.activity && !ava.has_distant_destination() ) {
            return false;
        }
    }
    const auto age = std::chrono::steady_clock::now() - g_turn_opened_at;
    // A fight is not a fast-forward, whoever is busy.
    if( anyone_is_seen_by_a_hostile() ) {
        return age < std::chrono::milliseconds( g_hurry_combat_ms );
    }
    // A left-behind player is still reading; the floor lasts as long as the
    // countdown that dropped them would have.
    if( anyone_left_behind ) {
        return age < std::chrono::milliseconds( g_hurry_ms );
    }
    return false;
}

void mp_request_session_end( const std::string &reason )
{
    if( !g_session_end ) {
        g_session_end_reason = reason;
    }
    g_session_end = true;
}

const std::string &mp_session_end_reason()
{
    return g_session_end_reason;
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
        // Expired. Drop everyone still holding, for as long as they sit at the
        // input wait they are in (mp_turn_reset). Their UI is untouched: their
        // character stands still and skips turns, their menu stays open, and
        // they confirm their choice when they are ready.
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
        const mp_player_state state = mp_player_state_of( i );
        if( state == mp_player_state::not_waited || state == mp_player_state::away ||
            state == mp_player_state::disconnected ) {
            // Standing still is what the drop means -- and what being away or
            // disconnected means, since nobody is there to spend the moves.
            // Taken at the end of the turn rather than at the moment of the
            // drop, because a drop lasts as long as the player sits in the
            // same menu while every closed turn hands out another turn's worth
            // of moves.
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
        case ACTION_COOP_AWAY:
        case ACTION_SAVE:
            return true;
        default:
            // Everything else costs time, or might. Refusing is the safe
            // direction: a gap here is an annoyance, a gap the other way is a
            // free action outside the turn.
            return false;
    }
}
