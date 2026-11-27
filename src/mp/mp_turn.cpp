#include "mp/mp_turn.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "avatar.h"
#include "debug.h"
#include "debug_capture.h"
#include "game.h"
#include "map.h"
#include "monster.h"
#include "mp/mp_context.h"
#include "mp/mp_plan.h"
#include "mp/mp_resolve.h"
#include "mp/mp_tether.h"
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
    // A committed plan belongs to a player who is still at the table. The pool
    // is left alone here: the positive part is burned at the tick's tail, on
    // the world thread, and a debt stays. Plans exist only in co-op, so the
    // single-player build has nothing to cancel.
#if defined( MULTIPLAYER )
    if( away ) {
        mp_plan_cancel( slot );
    }
#endif
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
            // Taken at the end of the tick rather than at the moment of the
            // drop, because a drop lasts as long as the player sits in the
            // same menu while every closed tick hands out another tick's worth
            // of moves. Only the positive part burns: a debt is budget that was
            // already spent, and the ticks pay it back.
            avatar &ava = g->player_at( i );
            ava.set_moves( std::min( 0, ava.get_moves() ) );
        }
    }
}

void mp_seed_session_moves( const size_t slot, const bool is_new )
{
    if( slot >= g->player_slot_count() ) {
        return;
    }
    avatar &ava = g->player_at( slot );
    // A character created just now has not played a turn, so it owes nothing. A
    // loaded one keeps its debt -- the pool is budget already spent -- and
    // loses only the positive bank, which the load policy does not carry.
    ava.set_moves( is_new ? 0 : std::min( 0, ava.get_moves() ) );
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
        // Menus whose key only opens the menu: the spec makes opening,
        // closing, tabs, filters, sorting and examining free in every phase,
        // and what a menu does with a confirmed choice is not this question.
        // The whitelist is inverted, so each of these has to be named.
        case ACTION_DROP:
        case ACTION_COMPARE:
        case ACTION_EXAMINE:
        case ACTION_CRAFT:
        case ACTION_CONSTRUCT:
        case ACTION_WIELD:
        case ACTION_WEAR:
        case ACTION_TAKE_OFF:
        case ACTION_UNLOAD:
        case ACTION_ITEMACTION:
        // AIM's key only opens the inventory-management menu; the moves it
        // confirms inside are the same menu-action class as drop/craft above.
        case ACTION_ADVANCEDINV:
        // The read menu opens first and the reading activity starts only on its
        // confirmed choice, and phase 1 keeps a menu's confirmed action
        // immediate, so the key itself spends nothing.
        case ACTION_READ:
        // ACTION_EAT is deliberately absent: it is not menu-only. A
        // RUMINANT/GRAZER on an edible tile eats from the map and mutates
        // terrain with no menu at all. A phase-1 limitation, restored when menu
        // actions become plans.
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
            // Everything else spends time or starts a targeting flow of its
            // own (movement, pickup, fire, throw, pause, autotravel). Refusing
            // is the safe direction: a gap here is an annoyance, a gap the
            // other way is a free action outside the turn.
            return false;
    }
}

#if defined( MULTIPLAYER )
// --- A planning tick -------------------------------------------------------

mp_action_availability mp_turn_player_availability( const size_t slot )
{
    if( slot >= g->player_slot_count() ) {
        return mp_action_availability::unavailable;
    }
    const avatar &ava = g->player_at( slot );
    // Dead first: nothing else about a corpse matters, and the resolver must
    // never run its plan.
    if( ava.is_dead_state() ) {
        return mp_action_availability::dead;
    }
    // Then the flags with no keyboard behind them -- away before disconnected,
    // as everywhere else: stepping away is the player's own choice.
    if( mp_player_is_away( slot ) ) {
        return mp_action_availability::away;
    }
    if( mp_player_is_disconnected( slot ) ) {
        return mp_action_availability::disconnected;
    }
    if( mp_plan_is_passed( slot ) ) {
        return mp_action_availability::passed;
    }
    // Occupied: an activity or a destination will spend the tick without the
    // player touching a key.
    if( ava.activity || ava.has_destination() ) {
        return mp_action_availability::busy;
    }
    // An empty or negative pool: the debt is paid back by ticks, not actions.
    if( ava.get_moves() <= 0 ) {
        return mp_action_availability::recovering;
    }
    return mp_action_availability::available;
}

const char *mp_action_availability_name( const mp_action_availability availability )
{
    switch( availability ) {
        case mp_action_availability::available:
            return "available";
        case mp_action_availability::recovering:
            return "recovering";
        case mp_action_availability::passed:
            return "passed";
        case mp_action_availability::busy:
            return "busy";
        case mp_action_availability::away:
            return "away";
        case mp_action_availability::disconnected:
            return "disconnected";
        case mp_action_availability::dead:
            return "dead";
        case mp_action_availability::unavailable:
            return "unavailable";
    }
    return "unavailable";
}

bool mp_action_availability_from_string( const std::string &text,
        mp_action_availability &availability )
{
    for( const mp_action_availability state :
         { mp_action_availability::available, mp_action_availability::recovering,
           mp_action_availability::passed, mp_action_availability::busy,
           mp_action_availability::away, mp_action_availability::disconnected,
           mp_action_availability::dead, mp_action_availability::unavailable } ) {
        if( text == mp_action_availability_name( state ) ) {
            availability = state;
            return true;
        }
    }
    return false;
}

const char *mp_action_availability_refusal( const mp_action_availability availability )
{
    switch( availability ) {
        case mp_action_availability::available:
            return "";
        case mp_action_availability::recovering:
            return "no AP left";
        case mp_action_availability::passed:
            return "already passed";
        case mp_action_availability::busy:
            return "busy";
        case mp_action_availability::away:
            return "away";
        case mp_action_availability::disconnected:
            return "disconnected";
        case mp_action_availability::dead:
            return "dead";
        case mp_action_availability::unavailable:
            return "no such seat";
    }
    return "";
}

bool mp_turn_player_can_act( const size_t slot )
{
    // Every state but `available` is a way of being done for the tick; only
    // what is left holds the round open.
    return mp_turn_player_availability( slot ) == mp_action_availability::available;
}

bool mp_turn_round_complete()
{
    std::vector<bool> eligible( g->player_slot_count(), false );
    for( size_t i = 0; i < eligible.size(); ++i ) {
        eligible[i] = mp_turn_player_can_act( i );
    }
    return mp_plan_round_complete( eligible );
}

void mp_turn_open_tick()
{
    mp_turn_reset(); // once per opened tick, not once per poll
    // The new tick forgets the last tick's passes; the round phase is planned
    // below, and any refusal text stays for the party broadcast that follows.
    mp_plan_reset_tick();
    g->world_turn_pre();
    // The session's whole-tick monster work. Single player's world_turn_post()
    // keeps interleaving it with the move loop; a tick takes it up front and
    // lets each round take one step (mp_resolve.h).
    mon_tick_pre();
    // Puts every seat's view back on its character before the preamble reads
    // the body, the vehicle and any running activity. A look loop's input wait
    // releases the baton, so the world can open a tick while one is parked; the
    // recentre skips that seat, whose own draw restores its centre on exit.
    mp_recenter_context_views();
    // Each player's turn preamble runs under that player's own context: it
    // touches the body, the vehicle and any running activity, and a shared
    // world context would run it all against slot 0's avatar.
    for( size_t i = 0; i < g->player_slot_count(); ++i ) {
        mp_activate_player( i );
        g->avatar_turn_pre( g->player_at( i ) );
    }
    mp_activate_world();
    mp_set_turn_open( true );
    // The first round's phase. A tick that opens with nobody able to act closes
    // in the same poll, and the resolver's `done` is then what a client sees
    // until the next tick opens.
    mp_plan_set_phase( mp_round_phase::planning );
}

void mp_turn_close_tick()
{
    mp_activate_world();
    // The session's tick tail: the despawn, the NPC body updates and the world
    // work that used to follow monmove(), never monmove() itself -- the rounds
    // already took their steps.
    world_turn_post_tail();
    // Nobody is at the keyboard of an away or disconnected seat, and a player
    // a countdown dropped is done for the turn: the positive part of their pool
    // is burned here, before the tick's own grant. A debt stays a debt.
    mp_forfeit_dropped_moves();
    // Moves come back after the world's tail, exactly as in single player, in
    // each player's own context: avatar_turn_post() may open the progress popup
    // and reads the whole avatar.
    for( size_t i = 0; i < g->player_slot_count(); ++i ) {
        mp_activate_player( i );
        g->avatar_turn_post( g->player_at( i ) );
    }
    mp_activate_player( 0 );
    // The bubble follows the group. Kept identical to the barrier loop's tail:
    // one seat means one fixed viewpoint and no re-centring.
    if( g->player_slot_count() > 1 ) {
        const tripoint_abs_ms anchor = mp_bubble_anchor();
        g->shift_map_center( anchor );
        if( anchor.z() != get_map().get_abs_sub().z() ) {
            g->shift_map_z( anchor.z() );
        }
    }
    mp_set_turn_open( false );
    debug_menu::debug_capture::tick_if_active();
}
#endif // MULTIPLAYER
