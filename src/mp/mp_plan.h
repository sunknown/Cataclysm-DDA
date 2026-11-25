#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "coordinates.h"
#include "mp/mp_protocol.h"

// The round state of a co-op session: what each seat intends to do, whether it
// has committed to that intent, and where the round as a whole is. Called only
// by the world thread while it holds the baton -- like the rest of the turn
// machinery, there is no locking here because there is only one writer.
//
// Plans are ephemeral. They live in the process, are never saved, and are
// cleared between rounds, because a plan names a tile and a creature in the
// world as it is now; restoring one into a different world would be a lie.

// Where the round is. `planning` collects plans; `resolving` executes them and
// refuses anything new; `done` is the moment between a finished resolution and
// the next round's planning phase. The orchestrator drives the transitions.
enum class mp_round_phase : uint8_t {
    planning,
    resolving,
    done,
};

// One seat's intent. `cost` is the client's own estimate for its overlay only:
// the server recomputes the cost from the world (see mp_plan_cost) and the
// client never decides what it can afford.
struct mp_plan {
    mp_plan_kind kind = mp_plan_kind::none;
    tripoint_bub_ms target;
    int cost = 0;
    uint32_t seq = 0;
    std::string label;
};

// Forgets every plan, readiness and pass in every seat. A session start (or a
// test) calls this; the phase goes back to planning.
void mp_plan_reset_all();

// Forgets this round's plans and readiness, keeping the phase, any refusal text
// and any pass. A pass is a decision for the whole tick, so an intermediate
// round does not undo it; the text must outlive the reset by one broadcast,
// because that is how the client learns why a committed plan did not run. The
// seat's next submit, cancel or pass clears the text, and mp_plan_reset_tick()
// starts the next tick.
void mp_plan_reset_round();

// Forgets the last tick's plans and passes at the tick's opening, keeping the
// phase and any refusal text: a pass ends with the tick that took it. Called
// once per opened tick, never between the tick's rounds.
void mp_plan_reset_tick();

// Forgets everything about one seat -- plan, readiness, pass and refusal text
// -- for a new identity in a reused seat. A reconnect is not a new identity and
// does not call this.
void mp_plan_reset_seat( size_t slot );

// Stores a seat's plan after validating it against the world. On refusal
// `error` says why in words a player can read, and the previous plan (if any)
// is left alone. On success the seat is no longer ready or passed: a new
// intent means it has to be committed again.
bool mp_plan_submit( size_t slot, const mp_plan &plan, std::string &error );

// The server's estimate of what a plan costs in AP. Negative means the plan
// cannot be costed (an unsupported or empty plan), which mp_plan_ready()
// refuses. The client's `plan.cost` is never trusted.
int mp_plan_cost( size_t slot, const mp_plan &plan );

// Drops a seat's plan and readiness. A pass already taken stays: it spent the
// remainder and ended the seat's part of the tick, and cancelling an intent
// refunds nothing.
void mp_plan_cancel( size_t slot );

// Commits a seat's plan for this round, gated on the seat being able to start
// an action (see mp_turn_player_availability). The plan's price is not part of
// the gate: any positive balance starts the action and the engine charges in
// full, so the commit does not have to cover the estimate. On refusal `error`
// says why; a negative cost is refused as an invalid plan.
bool mp_plan_ready( size_t slot, std::string &error );

// Spends everything the seat has left and marks it done for this round.
void mp_plan_pass( size_t slot );

bool mp_plan_is_ready( size_t slot );
bool mp_plan_is_passed( size_t slot );

// A seat's stored plan, or a default-constructed plan when it has none.
//
// The result refers into the slot table, so it is valid only while that table
// keeps its storage. The table grows when the number of player slots has grown
// since the last call -- that is how a player joining mid-round is first seen,
// and the growth happens inside the next call here, including a submit for the
// new seat -- and mp_plan_reset_all() rebuilds it. The value behind a live
// reference also changes whenever that seat's plan changes (submit, cancel,
// pass, reset_round).
//
// Supported pattern: copy the plan and keep the copy. Any reference obtained
// across a call that can grow or rebuild the table is dangling, and any
// reference meant to keep a *value* across a plan change is stale.
const mp_plan &mp_plan_of( size_t slot );

// True when every eligible seat is ready or passed. Ineligible seats are
// ignored, and an empty eligible set is vacuously complete.
bool mp_plan_round_complete( const std::vector<bool> &eligible );

// The reason this seat's last plan was refused or could not run, in words a
// player can read; empty when there is none. The resolver sets it when it
// refuses a committed action (an unaffordable step, a kind this phase cannot
// run, a menu it had to cancel), and the party broadcast carries it to the
// client. Accepting a new intent for the seat -- submit, cancel, pass -- clears
// it; a round reset deliberately keeps it for the broadcast that follows.
//
// Like mp_plan_of, the result refers into the slot table: copy it to keep the
// text across a call that can grow or rebuild the table.
const std::string &mp_plan_error( size_t slot );
void mp_plan_set_error( size_t slot, std::string error );

mp_round_phase mp_plan_phase();
void mp_plan_set_phase( mp_round_phase phase );
