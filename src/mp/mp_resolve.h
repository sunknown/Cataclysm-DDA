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
// non-dead seat's plan in cost order (Cost from the plan, then speed
// descending, then slot), moves as a batch per cost group, then the non-move
// actions. Cleans up between groups and runs mon_round_step() after the player
// batch. The phase becomes `resolving` before anything is applied and `done`
// once the monster step has finished; the round state itself is left for the
// caller (Task 4's tick loop) to reset.
void mp_resolve_round( size_t player_count );

// Runs one seat's plan under the seat's own context: `move`, `melee` and
// `pass` are supported in this phase; `ranged`, `use_item` and `interact` are
// refused with "not in this phase" and spend nothing. The cost is recomputed
// from the world immediately before the action and the action is refused (with
// "needs N AP, you have M") when the seat cannot pay it. A move or attack that
// the world has invalidated -- the target died, somebody took the tile -- is
// still charged as a miss; a dead seat acts not at all.
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
void mon_round_step();

// The rest of the tick's world work: despawning monsters left outside the
// bubble, overmap NPC travel, NPC morale, furniture/terrain field emission, the
// monster-visibility refresh, weather, and the visibility-cache invalidation.
void world_turn_post_tail();
