#pragma once
#ifndef CATA_SRC_MP_MP_PLACEMENT_H
#define CATA_SRC_MP_MP_PLACEMENT_H

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "coordinates.h"

class avatar;
class scenario;

// Establishes a freshly created character in a world that already exists.
//
// With `near` unset this runs the scenario's own start location and the
// world-building path -- only ever correct for the first character in an empty
// world, because game::start_game()'s world steps (start_calendar, seed,
// init_region_layout, and MAPBUFFER.clear on its retry path) would reset the
// calendar and wipe the map of a loaded world. That path builds the world
// around `u`, so `ava` must be the active slot.
//
// With `near` set every world step is skipped and the character is placed on a
// free tile next to that point.
//
// Returns an empty string on success, otherwise why the character could not be
// placed -- "the world could not be built" and "there is no room next to the
// group" are different refusals and the join menu shows the one that happened.
//
// Also what Phase 5 needs for a replacement character after a death.
std::string place_new_character( avatar &ava, std::optional<tripoint_abs_ms> near );

// A free tile as close to `target` as possible: passable, not occupied by any
// creature or player, and within the tether's soft limit. Returns nullopt when
// the search exhausts that radius, which the caller reports rather than papering
// over.
//
// The occupancy check covers every player slot, not just the creature tracker:
// the tracker sees only the active avatar, and placement puts players down one
// at a time.
std::optional<tripoint_abs_ms> mp_find_free_tile_near( const tripoint_abs_ms &target );

// The two-pass placement of a whole roster, run once after every player has
// chosen (never before: slots are handed out by connection order, so "slot 0"
// is not necessarily anybody's position).
//
// Pass 1 gives every continuing character the position from its own save; the
// first of them becomes the anchor, and continuations beyond the tether's soft
// limit from it are pulled next to it. Pass 2 places every new character next
// to the anchor. With no continuing character the anchor is `world_anchor`:
// where the world's game state put the bubble, when the world has been played
// but nobody here continues in it. With neither, the world is empty and the
// first new character goes down the full start_game() path and becomes the
// anchor.
//
// `is_new[i]` says whether slot i was created rather than continued. Fills
// `moved` with the slots that had to be pulled to the anchor, so the caller can
// tell those players before the session starts.
//
// Returns an empty string on success, otherwise the refusal to show the
// players: which character could not be placed and why. Keeping the reason in
// the return value keeps "the world could not be built" and "no room near the
// group" from reading as the same failure in the join phase's log and bye.
std::string mp_place_roster( const std::vector<bool> &is_new, std::vector<size_t> &moved,
                             std::optional<tripoint_abs_ms> world_anchor );

// The scenario every co-op character starts under: one start location, an evac
// shelter, safe and with floor to spare -- which is what keeps the anchor's spot
// usable for a whole party. It does not put players next to each other; no
// scenario can, and mp_place_roster() does that. Null, after a debugmsg, when
// the JSON no longer defines it.
const scenario *mp_coop_scenario();

#endif // CATA_SRC_MP_MP_PLACEMENT_H
