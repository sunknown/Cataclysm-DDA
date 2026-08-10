#pragma once

#include <cstddef>

// What a player is doing from the turn loop's point of view. Derived from game
// state on every query rather than stored: two pieces of state that could
// disagree is exactly the bug the co-op mode fixed once already.
enum class mp_player_state {
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
    // Dropped from the barrier for this turn by a hurry countdown.
    not_waited,
};

mp_player_state mp_player_state_of( size_t slot );

// True while `slot` is one of the players still holding the barrier. Read by
// the sidebar widget so everyone can see who is being waited on.
bool mp_player_holds_barrier( size_t slot );

// True when the world may tick: every living player has either acted or been
// dropped from the barrier. The single rule the whole turn model rests on.
bool mp_barrier_satisfied();

// Marks `slot` as sitting at an input wait, or no longer sitting at one. Called
// by the server backend around the point where it blocks on the wire, because
// that is the only place the game can tell "thinking" from "working".
void mp_set_waiting_for_input( size_t slot, bool waiting );

// Clears the per-turn barrier bookkeeping that belongs to a fresh turn: the
// hurry countdown, and the countdown's drops -- but only for players who have
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
// once the barrier is satisfied and world_turn_post has run.
void mp_set_turn_open( bool open );

// The session must end: a player quit or died out of the game. Player threads
// call the setter because player_act's return value has nowhere else to go --
// they run inside a thread body, not inside do_turn -- and the orchestrator
// turns it into do_turn()'s `true` for main().
void mp_request_session_end();
bool mp_session_end_requested();
