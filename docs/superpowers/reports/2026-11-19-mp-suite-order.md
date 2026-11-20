# Order-dependent SIGSEGV in the full test suite (F2)

Date: 2026-11-19
Tree: `fix/mp-followups`, base commit `fc5ac01e61`, fix uncommitted at the time of writing.
Scope: diagnosis and production fix for the shard-04 crash. The full parallel suite was **not**
rerun here; Task 7 owns the baseline comparison.

## Summary

`monster_can_navigate_from_overmap_to_reality_bubble_following_sound`
(`tests/monster_test.cpp:853`) alerts an idle horde with a sound. The signalling path
(`horde_map::signal_entities`) extracts the entity from its idle submap and leaves the now-empty
inner submap behind in `idle_monster_map`. `horde_map::iterator::insure_valid()` relies on empty
submaps never staying behind, so a later `overmap::serialize()` starts on that empty inner map and
dereferences its end iterator. The result is a SIGSEGV in `tripoint::serialize` with
`this == 0x8` — a null hash node plus the offset of the pair inside it.

The corrupting state is left by production code, not by test-only state that cleanup could hide:
any real game that saves after a sound wakes an idle horde has the same container state. The fix
is therefore in `src/horde_map.cpp`; no existing test was changed or reordered, and a regression
test was added to `tests/horde_map_test.cpp`.

## Reproducer

The ordinary test binary was built with a distinct prefix so the unrelated `mpserver-` build was
not touched (a plain test binary did not exist in the tree):

```
make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 TILES=0 BUILD_PREFIX=plain-
```

Full shard-04 filter (binary is `tests/plain-cata_test` because of `BUILD_PREFIX=plain-`):

```
tests/plain-cata_test "$(cat .data/shard04-spec.txt)" --rng-seed 1 --user-dir=.data/fu-t6/
```

Before the fix (observed):

```
../tests/mp_save_split_test.cpp:375: FAILED:
  {Unknown expression after the reported line}
due to a fatal error condition:
  SIGSEGV - Segmentation violation signal
test cases:      56 |      55 passed | 1 failed
assertions: 2744763 | 2744762 passed | 1 failed
exit=139
```

(`56` cases because the run aborts at the save-split test; the remaining filtered cases never
execute. Catch2 runs test cases in declaration order, which is why the culprit is not adjacent to
the save test in the filter list.)

Minimal reproduction — the culprit plus the save-split test:

```
tests/plain-cata_test \
  "monster_can_navigate_from_overmap_to_reality_bubble_following_sound" , \
  "each_character_keeps_its_own_per_character_files" \
  --rng-seed 1 --user-dir=.data/fu-t6-A/
```

Before the fix: `assertions: 12 | 11 passed | 1 failed`, `exit=139`.

## Bisection

All runs use `--rng-seed 1`, the shard-04 spec names, and keep
`each_character_keeps_its_own_per_character_files` in every set. Catch2 runs test cases in
declaration order, not in filter order, so the save-split test is not last in the filter list.
Runs are labelled by their `.data/t6-spec-*.txt` file.

| Run | Tests (plus the save-split test) | Result |
| --- | --- | --- |
| full spec | all 107 filter names | exit 139, SIGSEGV |
| C | all except the last four executed tests | exit 0 |
| B | `Incapacited_character_can_not_dodge`, monster-navigate, mouse-view, anchor | exit 139 |
| D | the three of B that are not monster-navigate | exit 0 |
| A | monster-navigate only | exit 139 |
| E | C plus monster-navigate (the add-back) | exit 139 |

The last four executed tests before the crash were `Incapacited_character_can_not_dodge`,
`monster_can_navigate_from_overmap_to_reality_bubble_following_sound`,
`each_players_mouse_view_lives_in_that_players_ui_stack`, and
`the_anchor_is_the_first_continuing_slot_not_slot_zero`. Excluding the mouse-view test alone had
already been shown not to help (`.data/repro-shard04-nolv.log`, 55 cases, exit 139).

Minimal set: `monster_can_navigate_from_overmap_to_reality_bubble_following_sound` +
`each_character_keeps_its_own_per_character_files`. Adding the horde test back to the clean C set
(E) restores the crash, which is the add-back confirmation.

## Evidence

gdb on run A, at the crash:

```
#0  tripoint::serialize (this=this@entry=0x8, jsout=...) at src/savegame_json.cpp:4646
#1  coords::coord_point_base<tripoint>::serialize (this=0x8, ...) at src/coordinates.h:110
#2  overmap::serialize (this=this@entry=0x7fbfaf000000, fout=...) at src/savegame.cpp:1493
#3  overmap::save (this=0x7fbfaf000000) at src/overmap.cpp:3772
```

`src/savegame.cpp:1493` is `monster_entry.first.serialize( json )` in the `hordes` loop.
`0x8` is a null `std::unordered_map` node: the hash node holds its next pointer first, so the
value pair starts at offset 8. In other words the range-for's `begin()` is an iterator into an
empty inner map; `insure_valid()` never checks the inner map after choosing an outer entry.

At that frame, the horde maps of the overmap being saved (`$1..$4` = outer element counts):

```
$1 = 0        # active_monster_map
$2 = 1        # idle_monster_map
$3 = 0        # dormant_monster_map
$4 = 0        # immobile_monster_map
$5 = std::unordered_map with 1 element = {[...] = std::unordered_map with 0 elements}
```

So the overmap itself is valid and still owned by `overmap_buffer`; the damage is the single empty
idle submap.

## Hypotheses

1. **Leftover `overmap_buffer` state (has/moved/removed overmaps): partially supported.** The
   overmap in the buffer is the ordinary `(0,0,0)` overmap and is valid (`this` is a live
   pointer); no moved-from or removed overmap is involved. The leftover that matters is the empty
   idle horde submap inside that overmap. `overmap_buffer.clear()` would mask the bug but not fix
   it, and the same state is reachable in a real game.
2. **`game::world_seed` / overmap cache: ruled out.** The crash is in serialization of an
   existing overmap; saving consults neither the seed nor the cache. Run D uses the same seed and
   passes.
3. **`calendar` / weather globals: ruled out.** Run D contains every other test in the shard,
   including the calendar/weather users, and passes; run A contains only the horde test and the
   save test.
4. **`g->overmap_data` leftovers: ruled out.** No overmap UI is opened in A, and the fault is in
   `overmap::save`, not `overmap_ui`.
5. **Map memory/cache: ruled out.** `game::save_maps()` saves the reality bubble first; the crash
   happens in `overmap_buffer.save()`, before `MAPBUFFER.save()`. Clearing or reloading the map
   would not touch the horde container.

The evidence supports only the horde-map state left by `signal_entities()`.

## Root cause and fix

`horde_map::signal_entities()` (`src/horde_map.cpp`) iterates the idle submaps. For each one,
`signal_sm(..., active=false, ...)` extracts every alert entity into `migrating_hordes`; the
entities are re-inserted into the active map afterwards. Nothing removed the now-empty outer
submap. `horde_map::iterator::insure_valid()` assumes the opposite:

```cpp
// horde_map::erase() insures that we cull empty maps, so if
// outer_map is not empty, outer_map->begin() is valid and so is
// outer_map->begin()->second.begin()
inner_iter = outer_iter->second.begin();
```

`horde_map::erase()` and `horde_map::extract()` do cull, which is why only the signalling path
leaves the poison.

Fix (`src/horde_map.cpp`, idle loop of `signal_entities()`): erase the outer entry when
`signal_sm()` emptied its inner map, using the iterator returned by `erase()`; advance normally
otherwise. The active loop needs no change because `signal_sm(..., active=true, ...)` never
extracts entities.

No invariant check was added to `insure_valid()`: the culling fixes the only producer of empty
submaps, and a second copy of the invariant would be dead code today. `insure_valid()`, the
iterator, and the rest of the container are untouched.

## Regression test

`tests/horde_map_test.cpp:139` adds
`signalling_an_idle_horde_drops_the_emptied_submap` under `[hordes]`, next to
`horde_map_corner_cases`. It sets a location, spawns one `mon_zombie` with
`spawn_entity( entity_pos, mon_zombie )`, checks it landed in the idle map, calls
`signal_entities( entity_pos, 10 )`, and then walks the idle-only view while reading the key:

```cpp
tripoint_abs_ms idle_key = tripoint_abs_ms::zero;
int idle_entities = 0;
for( std::pair<const tripoint_abs_ms, horde_entity> &entity :
     test_horde.get_view( horde_map_flavors::idle ) ) {
    idle_entities++;
    idle_key = entity.first;
}
CHECK( idle_entities == 0 );
CHECK( idle_key == tripoint_abs_ms::zero );
```

A full-view iteration would not fault without the fix: the re-inserted active entity keeps
`active_monster_map` non-empty, and `operator++` skips past the empty idle submap. The idle-only
view starts on it, and reading the key is what dereferences the end of the empty inner map. The
test calls `signal_entities()` directly, so it needs no sounds and no world.

RED, with the cull temporarily removed and the test binary rebuilt:

```
../tests/horde_map_test.cpp:156: FAILED:
  {Unknown expression after the reported line}
due to a fatal error condition:
  SIGSEGV - Segmentation violation signal
test cases: 1 | 1 failed
assertions: 5 | 4 passed | 1 failed
exit=139
```

gdb on the same run stops at `horde_map_test.cpp:163` (`idle_key = entity.first;`) with
`$_siginfo._sifields._sigfault.si_addr = 0x8` and `Cannot access memory at address 0x8` — the
same null hash node shape as the shard-04 crash (`overmap::serialize` at `src/savegame.cpp:1493`).

GREEN, with the cull restored and the binary rebuilt: the test passes with 6 assertions, and
`tests/plain-cata_test "[hordes]" --rng-seed 1` passes 13 test cases / 4467 assertions (12/4461
before the new test).

## Outcomes

Before, with `tests/plain-cata_test` built from unmodified sources:

- full shard-04 filter: exit 139, 56 cases, 1 failed / 2744763 assertions.
- minimal pair A: exit 139, 12 assertions, 1 failed.

After the fix (same build command, incremental rebuild of `src/horde_map.cpp`):

| Command | Result |
| --- | --- |
| `tests/plain-cata_test "$(cat .data/shard04-spec.txt)" --rng-seed 1 --user-dir=.data/t6-green/` | exit 0 — `All tests passed (3407902 assertions in 107 test cases)` |
| minimal pair A (`--user-dir=.data/fu-t6-Afix/`) | exit 0 — `All tests passed (49 assertions in 2 test cases)` |
| `tests/plain-cata_test "monster_can_navigate_from_overmap_to_reality_bubble_following_sound" --rng-seed 1` | exit 0 — 1 case passed |
| all 23 test cases of `tests/monster_test.cpp` (`.data/t6-spec-monsterfile.txt`) | exit 0 — `All tests passed (1517280 assertions in 23 test cases)` |
| `signalling_an_idle_horde_drops_the_emptied_submap` (new) | exit 0 — `All tests passed (6 assertions in 1 test case)` |
| `tests/plain-cata_test "[hordes]" --rng-seed 1` | exit 0 — `All tests passed (4467 assertions in 13 test cases)` |

The shard-04 run takes 91 s, under the two-minute bisection budget; the pre-fix run took 73 s.
The shard plan, the filter file, and `tests/mp_save_split_test.cpp` were not modified; the only
test change is the new case in `tests/horde_map_test.cpp`.

## How to rerun

1. Build: `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 TILES=0 BUILD_PREFIX=plain-`.
2. Reproduce: `tests/plain-cata_test "$(cat .data/shard04-spec.txt)" --rng-seed 1 --user-dir=.data/fu-t6/`.
3. Minimal: `tests/plain-cata_test "monster_can_navigate_from_overmap_to_reality_bubble_following_sound" , "each_character_keeps_its_own_per_character_files" --rng-seed 1 --user-dir=.data/fu-t6-A/`.
4. After the fix, run 2 and the `[hordes]` tag; both exit 0. The full parallel suite is Task 7's;
   it was deliberately not run here.

## Follow-up: Task 7 full-run regressions

Date: 2026-11-20
Tree: `fix/mp-followups` at `021bbf4057` plus this follow-up (uncommitted).
Scope: the failures the full parallel run on the final branch (`.data/fu-final`, `-b
tests/plain-cata_test --seed 1 -j 32`) reported that the older baseline run
(`.data/par-wd-full`) did not. Baseline failures were: shard-04 SIGSEGV (fixed above), shard-06
`monster_test.cpp:962` and `overmap_test.cpp:1023`, shard-09 `snow_depth_test.cpp:140/:173`, plus a
"failure due to error logged" shard (see below). Two of the four new shard exits here were a crash
class of their own, one was an order-dependent NPC test, one was that pre-existing logged ERROR;
`snow_depth_test.cpp` and `overmap_test.cpp:1023` pass on this branch.

### Shards 03 and 11: SIGABRT in `mp_activate_player` — a leaked co-op seat

Both aborted with `src/mp/mp_context.cpp:264 void mp_activate_player(size_t): Assertion 'idx <
g->contexts.size()' failed.` inside `[coop]` tests: `an_activity_spanning_turns_finishes_without_
aborting` (shard-03) and `moves_come_back_only_after_the_world_moved` (shard-11). Reproduced with
the exact shard filters, same assertion, same test both times.

Symbolizing the frames below `abort` (they are the informative ones; the frames Catch prints are
its own signal handler) gives one call chain for both shards:

```
mp_activate_player(unsigned long)  src/mp/mp_context.cpp:274
game::save()                       src/game_io.cpp:871      <- for( i < player_slot_count() ) mp_activate_player( i )
game::quicksave()                  src/game_io.cpp:1031
game::autosave()                   src/game_io.cpp:1068
game::world_turn_pre()             src/do_turn.cpp:617
game::do_turn()  (shard-03)        src/do_turn.cpp:937       <- coop_turn_test.cpp:265
C_A_T_C_H_T_E_S_T_16 (shard-11)    coop_turn_test.cpp:312    <- moves_come_back..., world_turn_pre()
```

So the abort is not in the test's own activation: an autosave inside the world half walks
`player_slot_count()` slots and `contexts` is one shorter. `idx == 1`, `contexts.size() == 1`,
`player_slots.size() == 2`.

Bisection (the failing test kept in every run, halves of the shard's own filter):

| run | filter | result |
| --- | --- | --- |
| shard-03 | full | abort at `coop_turn_test.cpp:223` |
| shard-03 | `nearest_player_picks_the_closer_slot` , `an_activity_spanning_turns_finishes_without_aborting` | abort, same assertion |
| shard-11 | full | abort at `coop_turn_test.cpp:289` |
| shard-11 | `anchor_is_integer_midpoint` , `moves_come_back_only_after_the_world_moved` | abort, same assertion |

`tests/coop_sim_test.cpp` is the corrupting file: five of its cases call `g->add_player_slot()` and
none removes the slot (`get_players_returns_every_slot`, `nearest_player_picks_the_closer_slot`,
`anchor_is_integer_midpoint`, `tether_thresholds_match_the_spec`, `monster_targets_the_closer_
player` — one even comments "no matter how many slots earlier tests leaked"). Leftover state is
`player_slots.size() == 2`, `active_slot() == 0`, `contexts.size() == 1`. Slots and contexts are
parallel by design; production keeps them in step with `mp_sync_context_count()` wherever the seat
count changes (`debug_menu::toggle_coop_slot`, `mp_server_start_player_threads`), and every other
multi-slot test calls it by hand (and `~mp_session_harness()` ends with
`reset_player_slots(); mp_sync_context_count();`). Nothing did that per test case:
`tests/test_main.cpp::testCaseEnded()` only reset calendar/weather. The leak is harmless until the
next test drives `do_turn()`/`world_turn_pre()` (an autosave then hits it) — which is exactly the
new shard plan's pairing, not a code path change.

Fix (`tests/test_main.cpp`, `testCaseEnded()`): normalise the seat state after every case.

```cpp
if( g->player_slot_count() != 1 || g->contexts.size() != 1 ) {
    g->remove_extra_player_slots();
    mp_sync_context_count();
}
```

`remove_extra_player_slots()` rather than `game_test_helper::reset_player_slots()`: it undoes an
in-progress avatar swap before freeing, so it is correct with any active slot, while
`reset_player_slots()` captures `player_at( 0 )` as the survivor and leaves
`player_slots[0]` dangling if slot 1 is the active one. The guard keeps the other ~2900 cases
byte-identical (single slot, single context). This is the same repair the `[mp]` harness destructor
already performs, applied to the cases that do not use that harness.

Before/after: both minimal pairs exit 139 → exit 0 (`All tests passed (5 assertions in 2 test
cases)`); shard-03 filter exit 134 → exit 0, `All tests passed (6321564 assertions in 168 test
cases)`; shard-11 filter exit 134 → exit 0, `All tests passed (1678072 assertions in 167 test
cases)`.

### Shard 07: `NPC-rules-close-doors` — the arena was generated where the last test left the bubble

`npc_behavior_rules_test.cpp:157/:158` (`turns_taken 101 < 100`, `(67,34,0) == (71,16,0)`; `:173/
:174` with `(68,58,0) == (71,40,0)` after the edits below). Order-dependent: the case passes alone
and fails in the shard. Instrumenting the walk showed the NPC moving six tiles, then stalling for
80+ turns at `moves == 0`, `path.size() == 18`, choosing `Go to position` every turn with
`BT=order(goto_ordered_position)`, no danger, no npc activity — and `path[0]` is a
`t_concrete_wall`.

Cause, in `setup_generic_rules_test()` (two stacked problems, both in the test's preconditions):

1. The avatar, and therefore the NPC, and therefore the OMT the arena is generated at, stood
   wherever the previous test left them. The `[mp]` session harness shifts the reality bubble to
   its players' midpoint and how far its turn loop runs before teardown is thread-timing
   dependent: two runs of the same test set left the avatar at `(36,24,0)` and at `(-120,-120,0)`.
   From a bubble-edge origin the `update_mapgen` lands on a non-main map ("loading non-main map at
   (-10,-12,0) which overlaps with main map") and from an off-axis origin inside the OMT the route
   to the chair cuts the concrete perimeter — the NPC's pathfinding settings allow bashing
   (`max_bash_distance` 10), so `npc_goto_to_this_pos` re-routes through the wall and the NPC bashes
   concrete, which outlasts the whole 100-turn budget.
2. The NPC was inserted into the overmap before it had any position, so `g->load_npcs()` logged
   `src/npc.cpp:1264 Failed to place NPC in a valid location near (20,34,-500)`.

Problem 2 is the shard-09 failure (see below); problem 1 is what made shard-07's NPC test fail.

Fix (tests only, `tests/npc_behavior_rules_test.cpp`):

- pin the avatar to the middle of the main map before generating, so the arena is always inside it;
- `guy->spawn_at_precise( here.get_abs( next_to ) )` before `insert_npc()`: `place_on_map` now
  succeeds and no `on_move()` bookkeeping runs against an NPC the buffer does not hold yet
  (`setpos` there logs "could not find npc on its old overmap");
- after `manual_mapgen`, stand the NPC on the arena's own southern approach — column 11 of its last
  row, the door frame the arena opens with (`data/mods/TEST_DATA/npc_behavior_arenas/
  arena_maps.json`) — so the route is the intended door column instead of the perimeter wall.

Before/after: `(68,58,0) == (71,40,0)`, `101 < 100`, 4 s of bashing → the case passes; shard-07 goes
from 2 failed to 1 failed, the remaining one being the baseline `monster_test.cpp:962`.
`NPC-rules-avoid-doors` and `NPC-rules-avoid-locks` share the helper and still pass.

### Shard 09: green tests, "failure due to error logged"

The runner failed the shard on `Treating result as failure due to error logged during tests` with
`All tests passed (53633 assertions in 167 test cases)`. The logged line was
`src/npc.cpp:1264 [void npc::place_on_map(map*)] Failed to place NPC in a valid location near
(20,34,-500)`.

That is pre-existing, not this branch: the identical message from the identical line appears in the
baseline run's shard (`grep -l "Failed to place NPC" .data/par-wd-full/*.log` →
`.data/par-wd-full/shard-05.log`, which also printed `All tests passed (1268513 assertions in 107
test cases)` followed by `Treating result as failure due to error logged during tests`). The new
shard plan only moved the test that triggers it into another shard. It is produced by
`setup_generic_rules_test()` inserting a position-less `npc` into the overmap (see above); the fix
above removes it. After the fix: shard-09 exit 0, no ERROR line.

### `monster_test.cpp:962` — same defect as the baseline

`REQUIRE( g->num_creatures() == 1 )` → `2 == 1` in `monster_cant_enter_reality_bubble_because_wall`
in both runs; the only difference is the `CAPTURE`d `step_attempts` (60 in the baseline, 44 here),
an attempt count, not an assertion. So it is the same behaviour, unchanged by this branch — the new
shard plan only changed which attempts got made. Not addressed here (it needs the horde/monster
path inspected; the shard-04 work above did not touch `move_hordes`).

### Outcomes

| run | before | after |
| --- | --- | --- |
| shard-03 filter | exit 134, SIGABRT in `mp_activate_player` | exit 0 — 168 cases, 6321564 assertions |
| shard-11 filter | exit 134, SIGABRT in `mp_activate_player` | exit 0 — 167 cases, 1678072 assertions |
| shard-07 filter | exit 3, 2 failed (`monster_test.cpp:962`, `npc_behavior_rules_test.cpp:157/:158`) | exit 1, 1 failed (`monster_test.cpp:962`, baseline) — 167 cases |
| shard-09 filter | exit 1, error logged (`npc.cpp:1264`) | exit 0 — 167 cases, 53614 assertions |
| minimal pair A (`nearest_player_picks_the_closer_slot` + activity test) | exit 134 | exit 0 — 2 cases |
| minimal pair B (`anchor_is_integer_midpoint` + moves test) | exit 134 | exit 0 — 2 cases |
| `[coop]` tag | — | exit 0 — 33 cases, 175 assertions |
| `[mp]` tag | — | green to case 136 of the run (`137 | 136 passed`); the run was stopped there by SIGTERM, and the only `FAILED` block is `SIGTERM` in the case that was executing (`mp_turn_test.cpp:702`) |

Changed files: `tests/test_main.cpp` (per-test seat/context teardown), `tests/npc_behavior_rules_
test.cpp` (arena setup preconditions). No production code, no shard plan, no filter change. The
full parallel suite was not rerun; the four affected shard filters were.
