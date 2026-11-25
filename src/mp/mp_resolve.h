#pragma once

#include <cstddef>

class avatar;
struct mp_plan;

// The co-op round: what every committed plan does, in a world that only moves
// once all of them are known.
//
// The header declares these in every build so shared translation units can
// include it; the definitions live in MULTIPLAYER-only translation units and
// the scheduler's call sites are guarded the same way. Single player never
// enters the resolver: its turn runs inline in game::do_turn().

// Executes one round of the seats 0 .. player_count - 1: every ready, non-pass,
// non-dead, connected seat's plan in cost order (the server's mp_plan_cost of
// the plan, then speed descending, then slot), moves as a batch per cost group,
// then the non-move actions. Cleans up between groups and runs mon_round_step()
// after the player batch. The phase becomes `resolving` before anything is
// applied and `done` once the monster step has finished; the round state itself
// is left for the caller (Task 4's tick loop) to reset.
//
// Nothing it runs may wait for input: the world thread holds the baton and
// every player is parked on it. A confirmation prompt answers itself, a menu
// that would open is cancelled, and the plan that needed one fizzles with
// "needs an interactive choice" in the seat's error instead of half-executing.
//
// Returns whether the round's creature step made progress (see
// mon_round_step()). The tick loop pairs it with mp_resolve_any_creature_can_act()
// to decide whether a monster-only round follows.
bool mp_resolve_round( size_t player_count );

// Runs one seat's plan under the seat's own context: `move`, `melee` and
// `pass` are supported in this phase; `ranged`, `use_item` and `interact` are
// refused with "not in this phase" and spend nothing. The cost is recomputed
// from the world immediately before the action and the action is refused (with
// "needs N AP, you have M") when the seat cannot pay it. A move or attack that
// the world has invalidated -- the target died, somebody took the tile -- is
// still charged as a miss; a dead seat acts not at all. An action that reaches
// a menu gets no interactive answer: the plan fizzles with "needs an
// interactive choice" and spends nothing. The engine's own charge is clamped
// at zero, so a priced move the character actually spends more on cannot leave
// a player with negative AP.
void mp_apply_plan( avatar &ava, const mp_plan &plan );

// The three pieces game::world_turn_post() was decomposed into, so a co-op
// round can put the players' batch between the tick's opening world work and
// its close.
//
// mon_tick_pre(): everything that must happen before the first creature moves
// this tick -- scent, floor caches, falling, vehicles, fields, items,
// explosions, the per-avatar field pass, sound processing, the map cache, then
// every monster's and NPC's own turn-processing preamble. Runs once per tick.
void mon_tick_pre();

// One step for every living monster that has the moves for it (ordered by
// position then name, because monsters have no stable id), then one iteration
// of every NPC's move loop. The round's share of the monster phase: a creature
// that could take several steps this tick takes one per round. Cleans up the
// dead afterwards.
//
// Returns whether any creature made progress: some monster's step ran, or some
// NPC's move loop spent moves. A round where nothing progresses is stuck --
// mon_step() zeroes a controlled monster's moves, and an NPC that never spends
// moves makes none -- and the tick must close instead of running it again.
bool mon_round_step();

// Whether any creature could still act this tick: a living, in-bounds,
// non-ridden monster with moves left, or an NPC its move loop would run. The
// continuation rule is this AND mon_round_step()'s progress, so a monster-only
// round keeps the tick open exactly as long as the creature's own AP lasts and
// a stuck creature closes it instead of hanging the server.
bool mp_resolve_any_creature_can_act();

// The rest of the tick's world work: despawning monsters left outside the
// bubble, overmap NPC travel, NPC morale, furniture/terrain field emission, the
// monster-visibility refresh, weather, and the visibility-cache invalidation.
void world_turn_post_tail();
