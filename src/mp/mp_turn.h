#pragma once

#include <cstddef>
#include <string>

#include "action.h"

// Long enough to finish reading a screen, short enough that a partner does not
// go and make tea. Tuned against the Phase 3a gate; configurable in Phase 4.
constexpr int MP_HURRY_MS = 15000;
// A fight is not the moment to read anything.
constexpr int MP_HURRY_COMBAT_MS = 4000;

// What a player is doing from the turn loop's point of view. Almost all of it
// is derived from game state on every query rather than stored: two pieces of
// state that could disagree is exactly the bug the co-op mode fixed once
// already. The one exception is `away`, which no amount of game state can
// tell apart from a player staring at an open menu.
enum class mp_player_state {
    // Voluntarily left the keyboard. Stored, and checked before activity and
    // moves, so it outranks every other state: a player who is not there
    // cannot be holding anything.
    away,
    // Has moves and is currently executing.
    acting,
    // Out of moves, or occupied by something that will spend the turn without
    // asking: a running activity, or a travel destination already set. This is
    // the cheap form of "planned turns" -- a player who has told the game what
    // to do does not have to be at the keyboard for the world to move on.
    acted,
    // Has moves and is sitting at an input wait. The only state that holds the
    // barrier, and therefore the only state a countdown is aimed at.
    holding,
    // Dropped from the barrier by a hurry countdown. Lasts while the player
    // sits at the input wait they were dropped in (see mp_turn_reset).
    not_waited,
};

mp_player_state mp_player_state_of( size_t slot );

// True while `slot` is one of the players still holding the barrier. Read by
// the sidebar widget so everyone can see who is being waited on.
bool mp_player_holds_barrier( size_t slot );

// True when the world may tick: every living player has either acted, been
// dropped from the barrier, or gone away. The single rule the whole turn model
// rests on.
bool mp_barrier_satisfied();

// True when every living player has left the keyboard on purpose. A satisfied
// barrier would let such a turn close, and closing it moves the world around
// empty chairs, so the orchestrator keeps the turn open on this as well.
// Vacuously true when no living player is left, which is the same "nobody is
// at the table" case.
bool mp_turn_all_away();

// True for a player a hurry countdown dropped who is still sitting at an input
// wait. They hold nothing, but they are still at the keyboard: when a key
// brings them back, the world has to hand them the baton even though it is
// not waiting on them.
bool mp_player_left_behind( size_t slot );

// True while a satisfied barrier must not close the turn yet. Not about the
// barrier: about what paces a turn nobody is playing by hand. A player at the
// keyboard -- acting, or a left-behind one once they act -- ends it. Otherwise
// the turn lasts at least as long as the countdown that would have dropped
// them: MP_HURRY_COMBAT_MS while a hostile can see a player, and MP_HURRY_MS
// while a left-behind player is still reading. An away player is not a
// left-behind one: they asked not to be waited for, so their partner's
// activity pace is not held back on a countdown aimed at nobody. With nobody
// playing by hand and no hostile in sight it ends too: that is the free run
// the phase exists for.
bool mp_turn_paced();

// Marks `slot` as sitting at an input wait, or no longer sitting at one. Called
// by the server backend around the point where it blocks on the wire, because
// that is the only place the game can tell "thinking" from "working".
void mp_set_waiting_for_input( size_t slot, bool waiting );

// Marks `slot` as having left the keyboard on purpose, or brings them back.
// This is the player's own decision, not a drop imposed by a countdown, so it
// stops the barrier waiting for them until they say otherwise. Called by
// whatever handles the away key.
//
// Unlike the hurry drop, this outlives mp_turn_reset(): a player who stepped
// away for one turn is still away on the next one. Only the end of the session
// (mp_turn_reset_all) clears it.
void mp_set_away( size_t slot, bool away );

// True while `slot` has been marked away. Read by the barrier rule and by the
// sidebar widget, which has to show who is not being waited on.
bool mp_player_is_away( size_t slot );

// Marks `slot` as back at the keyboard. A separate entry point from
// mp_set_away so that a caller who only has to bring somebody back does not
// have to reach for the flag itself.
void mp_return_from_away( size_t slot );

// Clears the per-turn barrier bookkeeping that belongs to a fresh turn: the
// hurry countdowns (both), and the countdowns' drops -- but only for players who have
// since left their input wait.
//
// Two deliberate exceptions, both of which were freezes when they were not
// exceptions:
//
// The input-wait flags are never cleared here. They belong to the input backend
// and live as long as the player is actually parked on a wait. Clearing them
// would turn a parked player into "acting", which mp_tick_hurry() cannot drop.
//
// A drop survives while its player has not moved from the same input wait. The
// alternative is one keypress of "hurry up" buying exactly one tick of world:
// a partner reading a recipe list would have to be hurried again every turn,
// which is the phase gate ("an evening of play") made unplayable by its own
// safety valve. Once they press a key and act, the input-wait flag clears and
// they rejoin the barrier on their own.
//
// The away flags are not cleared here either, for the same reason the input-wait
// flags are not: a player who stepped away is still away in the next turn, and
// clearing a voluntary state once per turn would make the key press expire on
// its own. Only mp_turn_reset_all() ends it.
//
// Called once per opened turn by the orchestrator.
void mp_turn_reset();

// Clears everything the module holds, including the input-wait flags, the turn
// flag and the end-of-session request. For test setup and teardown and for
// session start: those are the moments when the state of a previous session is
// not conservative but wrong.
void mp_turn_reset_all();

// Whether a turn is currently open: world_turn_pre has run and the barrier has
// not been satisfied yet. While open, do_turn skips world_turn_pre and only
// polls the barrier, so a waiting turn never re-runs the world half.
bool mp_turn_is_open();

// Opens or closes the turn. Set true right after world_turn_pre runs, set false
// once the barrier is satisfied and world_turn_post has run. Opening also
// starts the clock mp_turn_paced() measures.
void mp_set_turn_open( bool open );

// The session must end: a player quit, died or disconnected. Player threads
// call the setter because player_act's return value has nowhere else to go --
// they run inside a thread body, not inside do_turn -- and the orchestrator
// turns it into do_turn()'s `true` for main(). `reason` is what every client
// is told ("Bo disconnected"); the first request's reason is the one kept.
void mp_request_session_end( const std::string &reason );
bool mp_session_end_requested();
const std::string &mp_session_end_reason();

// The two things that start a hurry countdown. Each has its own countdown, so
// neither can swallow the other.
enum class mp_hurry_source {
    // A player pressed "hurry up". MP_HURRY_MS.
    manual,
    // A hostile creature can see a player; started by mp_tick_hurry() itself.
    // MP_HURRY_COMBAT_MS.
    combat,
};

// Starts the manual hurry countdown, if a manual one is not already running.
// A running combat countdown does not count: the press is a request in its own
// right, and the player gets an answer to it even in the middle of a fight.
//
// Any player who has already acted may call it. The countdown is visible to everyone; when it
// expires the server stops waiting for whoever is still holding -- it does not
// reach into their UI or close anything. Their character stands still and skips
// the turn; their menu stays open and they confirm when they are ready.
void mp_request_hurry();

// Milliseconds left on the countdown that will expire first, or -1 when none is
// running. Read by the sidebar widget and by the tests.
//
// "Expires first" because expiry is the only thing a countdown does -- it is
// what drops a holder -- so the shortest time left is the honest answer to "how
// long until the world stops waiting". When both are armed that is whichever
// has less left, not whichever was armed first or has the shorter length.
//
// Both countdowns end the same way, and they are independent: when one expires
// it drops everyone still holding and disarms itself only; the other keeps
// running. Both are turn-scoped, so mp_turn_reset() and mp_turn_reset_all()
// clear both. A combat countdown is simply re-armed by the next
// mp_tick_hurry() while a hostile can still see someone; a press is not, which
// is right, as it was a request about the turn that just ended.
int mp_hurry_remaining_ms();
// Milliseconds left on the one countdown `source` names, or -1 when it is not
// running. For a caller that has to know which trigger is armed: the overload
// above cannot tell a press from a hostile in view.
int mp_hurry_remaining_ms( mp_hurry_source source );

// Advances the countdown and, when it expires, drops every holding player from
// the barrier for this turn. Called once per orchestrator poll.
//
// Also starts the countdown by itself, on the shorter combat length, while a
// hostile creature in the bubble can see any player: nobody spends a keystroke
// on "hurry up" when something is running at them.
void mp_tick_hurry();

// Spends the turn's moves of every player the world stopped waiting for: the
// ones a countdown dropped, and the ones who went away. Called by the
// orchestrator once per closed turn, immediately before avatar_turn_post hands
// moves back.
//
// Not an optimisation and not cosmetic: moves accumulate (Character::
// process_turn does `moves += get_speed()`), and a dropped player never spends
// theirs, because they are reading a menu. Without this, a partner who read a
// recipe list for a minute comes back holding a minute's worth of moves and
// takes sixty actions inside one tick, which is both an exploit and exactly
// what "their character stands still and skips the turn" says does not happen.
// The same arithmetic hands a player who stepped away for a while a bank of
// moves to spend the moment they sit down again.
void mp_forfeit_dropped_moves();

// Whether `act` may be taken by a player who has already acted this turn.
//
// The default is "no": the list below is a whitelist, not a classification of
// all 153 action ids. Classifying every one of them would be both expensive and
// dangerous -- a mistake in the "free" direction lets a player act outside
// their turn. Inverted, a mistake merely refuses something harmless, which is
// annoying and fixed by one line.
bool mp_action_is_free( action_id act );

// Moves a countdown's start point back by `ms`, so a test can expire a
// fifteen-second countdown without sleeping for fifteen seconds. Does nothing
// to a countdown that is not running. The overload without a source advances
// both.
void mp_advance_hurry_for_test( int ms, mp_hurry_source source );
void mp_advance_hurry_for_test( int ms );
