# Several characters in one world: what actually breaks

Audit for phase 3b, task 8. The authority is the code in this tree at
`000066ec4e`, plus the three fixes that landed on top of it: `f244c4cee2`,
`51183bcad2` and `295ad265e7` (a bare player slot is never resolved as a live
character). Every owner below was checked by reading both the writer and the
reader of the state, not by grepping for the name once.

## 1. Where the warning came from

The warning is:

```
Many game features will not work correctly with multiple characters in the same world.
```

It exists in two places: `src/main_menu.cpp:1000` (template path) and
`src/main_menu.cpp:1045` (normal path), both guarded by
`!world->world_saves.empty()`.

- Commit `82ce883a389e67a567a111e9cabe93d07e687798`, Kevin Granade
  (`kevingranade`), "Warn about problems related to creating a new character in
  an existing world.", authored 2024-08-22, committed 2024-08-23, merged
  2024-08-27 via upstream PR #75908 ("Warn about problems related to creating a
  new character in an existing world for 0.I"). The patch is +13 lines in
  `src/main_menu.cpp` only: two `query_yn` prompts and nothing else.
- The PR body says it "Fixes #67750 but for 0.I" and states the position
  plainly: "Supporting multiple characters independently in the same world is
  not supported per-se, so we pop up a warning that things might break." The
  issue, #67750 by Karol1223 (2023-08-18), is titled "Killing one character in
  a world with multiple characters breaks followers for the other character":
  dying as one character printed another character's faction epilogues,
  offered to continue as that character's follower, and left the surviving
  character's follower stuck as neutral, "performing the last task assigned to
  her". Its expected behaviour is the sentence this audit is built on: "NPCs
  should not be shared across multiple characters. Neither should faction
  knowledge."
- The PR also tried, and abandoned, a `YES_NO_STOP_ASKING` prompt backed by a
  `WARN_ABOUT_MULTI_PLAYER_WORLD` external option ("This patch attempts to do
  so, but as I said I couldn't get it to work"). That option was never merged;
  the warning has no dismissal path in this tree.
- Nobody wrote the list of what breaks, upstream included. That list is below.

## 2. Findings

Verdict vocabulary is the brief's: **works**, **breaks cosmetically** (state is
shared, but nothing a character can do or own is lost), **breaks materially**
(a character loses or gains something another character did). "Owner" is where
the state is saved or, for run-time singletons, where it lives.

| Mechanic | State owner (file:line) | Verdict |
|---|---|---|
| Missions | world map `world_missions` (`src/mission.cpp:103`); assignee `mission::player_id` (`src/mission.h:325`) | **breaks materially** — completion, failure and rewards fixed; kill-count and recruit-class goals plus nemesis still world-wide |
| Faction reputation | one `faction` object per faction: `likes_u`/`respects_u`/`trusts_u` (`src/faction.h:148-150`), saved in the master save (`src/savegame_json.cpp:4213`, `src/game_io.cpp:628`); every avatar is `your_followers` (`src/avatar.cpp:1167-1170`) | **breaks materially** — deferred |
| Camps | world/overmap object owned by a faction (`basecamp::owner`, `src/basecamp.h:491`; `overmap.cpp:3914`); known-camp list is per character (`Character::camps`, `src/character.h:2933-2934`, upstream FIXME) | **breaks materially** — deferred |
| Map memory | per character: `avatar::player_map_memory` (`src/avatar.h:439`), `<world>/<base64(save_id)>.mm1/` (`src/map_memory.cpp:42-44`, `src/game_io.cpp:1077-1080`) | **works** |
| Diary | per character: `avatar::a_diary` (`src/avatar.h:471`), `<world>/<base64(save_id+"_diary")>.json` (`src/diary.cpp:619-629`, `:673-683`) | **works** |
| Zones | per character `<base>.zones.json` (`src/clzones.cpp:1896-1918`), filled from `zone_manager::get_manager()`; personal and `your_followers` zones are both in that file (`:1941-1975`) | **breaks materially** — fixed: one manager per seat |
| Memorial log | per character `<base>.log`, written from `memorial()` (`src/game_io.cpp:677-706`) and loaded from it (`:505-516`); the object came from one process-wide `get_memorial()` | **breaks materially** — fixed: one log per seat |
| Auto notes | per character `<base>.ano.json` (`src/auto_note.cpp:33-46`), filled from `get_auto_notes_settings()`; the object was one process-wide static | **breaks materially** — fixed: one set per seat |
| Auto pickup | per character `<base>.apu.json` (`src/auto_pickup.cpp:890-903`), filled from `get_auto_pickup()`; the object was one process-wide static | **breaks materially** — fixed: one set per seat |
| Safemode | per character `<base>.sfm.json` (`src/safemode_ui.cpp:778-790`), filled from `get_safemode()`; the object was one process-wide static | **breaks materially** — fixed: one set per seat |
| NPC followers | `avatar::follower_ids` per avatar (`src/avatar.h:424`, saved at `src/savegame_json.cpp:1633`/`:1724`), ally test is faction-wide (`src/npc.cpp:2692`), follow target is whoever is nearest (`src/npcmove.cpp:912`, `:2365-2370`) | **breaks materially** — deferred |
| Vehicles | `vpart::passenger_id` is a `character_id` (`src/vehicle.h:537`), saved with the part (`src/savegame_json.cpp:3712`, `:3778`), resolved by `vehicle::get_passenger()` (`src/vehicle.cpp:3682-3688`) | **breaks materially** — fixed here |
| Achievements & statistics | world singletons on `game` (`src/game.h:1363-1364`, subscribed `src/game.cpp:484-489`), serialized into every character `.sav` (`src/savegame.cpp:144-146`, `:350-351`) | **breaks materially** — deferred |
| `uistate` | one file per world (`src/game_io.cpp:373-377`, `:797-801`); UI-only fields (`src/inventory_ui.cpp:363-464`) | **breaks cosmetically** |

## 3. Per-mechanic detail

### Missions — breaks materially; completion fixed, kill counts still world-shared

`world_missions` is a file-static map in `src/mission.cpp:103`; `mission::player_id`
(`src/mission.h:325`) names the owner. `mission::process_all()` is called once
per world turn from `src/do_turn.cpp:561`, and the world half runs with slot 0
back in `u` (`mp_activate_world()`, `src/mp/mp_context.cpp:236-253`). Before the
fix, `mission::is_complete()`, `wrap_up()` and `fail()` all took
`get_avatar()`: a mission of slot 1 was judged using slot 0's position and
inventory, and `wrap_up()` paid its reward to slot 0. `fail()` notified nobody
for a slot-1 mission. The `end`/`fail` effect lambdas built by
`mission_type::parse_funcs()` used `get_talker_for( get_avatar() )`
(`src/mission_util.cpp:520-536`), so even a correctly judged mission paid
`u_add_morale`, item rewards and faction changes to the wrong character.

Fixed in `51183bcad2`:

- `mission_holder()` in `src/mission.cpp:47-69` resolves `player_id` through
  `game::get_players()`, falling back to the active avatar for an unassigned
  mission or one whose character is gone.
- `is_complete()`, `wrap_up()` and `fail()` use it (`src/mission.cpp:366`,
  `:409`, `:512`).
- `src/mission_util.cpp:520-536` resolves the same owner for the effect talker.

Residual gaps, still material:

- Kill-count goals are still world-shared. `MGOAL_KILL_MONSTER_TYPE` and
  `MGOAL_KILL_MONSTER_SPEC` compare against
  `g->get_kill_tracker().kill_count(...)` (`src/mission.cpp:644-649`); that is
  one world map (`src/kill_tracker.cpp:24-31`), filled by `notify()` for the
  active avatar and its followers (`:109-119`), and `mission::assign()`
  baselines the goal against the same world count
  (`src/mission.cpp:345-350`). Slot 0's zombie kills therefore complete slot
  1's "kill N zombies" mission and vice versa — materially broken by this
  audit's definition. Fix: attribute the tracker by killer (the event already
  carries `character`, `src/event.h:183-190`) and keep per-character counts, or
  count per character on the avatar and baseline the mission against its
  owner. One or two files plus a persisted per-character count (savegame
  field); estimate ~1 day, phase 5. Not patched here: there is no unit test
  without a live two-character kill and `game::unserialize` is forbidden.
- `MGOAL_RECRUIT_NPC_CLASS` searches
  `overmap_buffer.get_npcs_near_player( 100 )` (`src/mission.cpp:628`), which
  centres on the active avatar, not on the mission's owner. A follower
  recruited by slot 0 completes slot 1's recruit-class mission. Fix: search
  around the owner's `pos_abs_omt()` (one file, an overmap query that takes a
  position) and add a two-slot test with a spawned follower NPC; estimate ~2-4
  hours, phase 5. Deferred because this round is documentation for mission
  residuals; it is cheap but not free.
- `mission::on_creature_death()` looks for a nemesis mission by comparing
  `get_avatar().getID()` (`src/mission.cpp:197`). A nemesis belonging to a
  non-active slot is never completed by anyone; the kill is simply lost for
  that player. Cheap to fold into the same helper, but there is no way to test
  it without spawning a nemesis overmap horde, so it is left documented.
- The avatar-death branch (`src/mission.cpp:239`) clears `get_avatar()`'s
  active missions. That is correct while the dying avatar is the active one,
  which is the only way an avatar dies today; it becomes wrong the day damage
  over time kills a non-acting avatar. Phase 5 owns death.

Cost of finishing the nemesis gap: ~1 hour, one file, plus a nemesis fixture.

### Faction reputation — breaks materially, deferred

`likes_u`, `respects_u`, `trusts_u` are fields of the single world-level
`faction` object (`src/faction.h:148-150`), serialized into the master save
(`faction::serialize`, `src/savegame_json.cpp:4213`; called from
`game::save_factions_missions_npcs()`, `src/game_io.cpp:628`). Every avatar
returns the same faction: `avatar::get_faction()` always fetches
`your_followers` (`src/avatar.cpp:1167-1170`). Consequences: one player's theft,
murder, camp takeover or mission result moves every player's standing with that
faction, and `npc::is_ally()` treats any avatar as an ally when the NPC is in
`your_followers` (`src/npc.cpp:2686-2696`).

A real fix is a per-character opinion map (`std::map<character_id, opinion>` on
`faction`), migration of the three legacy ints, and every reader updated. That
is a savegame-format change and more than two files; phase 5. Estimate: 2-3
days including save migration and an NPC-dialogue pass. Note this is upstream's
own stated bug (#67750): "Neither should faction knowledge" be shared.

### Camps — breaks materially, deferred

The camp itself is world state: `basecamp` lives in the overmap
(`overmap_buffer.add_camp`, `src/overmap.cpp:3910-3914`; saved with the overmap
at `src/savegame.cpp:1540-1546`) and is owned by a faction
(`basecamp::owner`, `src/basecamp.h:491`). "Which camps are mine" is
not world state: `Character::camps` (`src/character.h:2933-2934`) is a
per-character set, saved with the character (`src/savegame_json.cpp:1568`,
loaded at `:1339-1346`), with the upstream FIXME admitting it "should only
[be] knowledge of camps (at best)". `map::add_camp()` records the camp in the
*active* avatar's set only (`src/map.cpp:7548-7554`).

Every consumer reads the active avatar's set: the faction/camp list
(`src/faction_ui.cpp:273-286`), camp takeover plunder counting
(`src/basecamp.cpp:909`), the "go to camp" NPC order
(`src/npctalk_funcs.cpp:354`), NPC camp duties (`src/npcmove.cpp:5366`), camp
removal (`src/faction_camp.cpp:1987`), and dialogue conditions
(`src/condition.cpp:1874`). So slot 0 cannot see, order work in, or take over
slot 1's camp, and slot 1's camp is invisible in slot 0's faction UI. The camp
objects and their NPC workers are shared, so the world keeps simulating them;
it is command and knowledge that split.

Cheapest correct fix: a world-level `your_followers` camp set (or an mp-aware
merge of all loaded avatars' sets at the camp UI and takeover paths). That is a
savegame layout change plus 2-3 files and about a day; phase 5. The brief's
candidate `d` (derive the UI list from the overmap) was rejected: it would show
camps the rest of the camp code still refuses to count or command, papering
over the per-character knowledge invariant instead of fixing it.

### Map memory — works

`avatar::player_map_memory` is per avatar (`src/avatar.h:439`);
`avatar::save_map_memory()`/`load_map_memory()` (`src/avatar.cpp:240-248`) use
`PATH_INFO::current_dimension_player_save_path()`
(`src/game_io.cpp:1077-1080`), which is `<world>/<base64(get_avatar().get_save_id())>`,
and `find_mm_dir()` appends `.mm1` (`src/map_memory.cpp:42-44`). The save loop
activates each slot before calling `save_character_data()`
(`src/game_io.cpp:848-856`), and `load_character()` loads into the slot that was
activated (`src/game_io.cpp:467-473`, `:492-494`). Verified: one directory per
character id, no sharing. No test is needed; the code path is per-character by
construction.

### Diary — works

`avatar::a_diary` (`src/avatar.h:471`) is per avatar, and both store and load
name the file `<world>/<base64(save_id + "_diary")>.json`
(`src/diary.cpp:619-629`, `:673-683`). As with map memory, the save loop puts
the right avatar in `u` before saving, and load activates the slot before
`u.get_avatar_diary()->load()` (`src/game_io.cpp:496-500`). No sharing.

### Memorial, auto notes, auto pickup, safemode — breaks materially, fixed (per seat)

All four are per-character files: `<base>.log`, `<base>.ano.json`,
`<base>.apu.json`, `<base>.sfm.json`. What filled them was not per character:
`get_memorial()`, `get_auto_notes_settings()`, `get_auto_pickup()` and
`get_safemode()` all returned one process-wide object, alongside the single
`zone_manager`. The load loop made the consequence concrete. Every continued
character is loaded by `load_character()` (`src/game_io.cpp:471-580`), which
runs `memorial().load()` (`:505-516`, and `load()` clears the log first),
`get_auto_pickup().load_character()` (`:533`), `get_auto_notes_settings().load( true )`
(`:534`, which also clears), `get_safemode().load_character()` (`:535`) and
`zone_manager::get_manager().load_zones()` (`:536`, which replaces the zone
vector). After two loads the process holds only the second character's state,
in game and in every file written afterwards: `save_character_data()`
(`:823-845`) wrote the surviving object into each slot's own file without
re-reading it, so on the first save of a two-character session each
character's `.log`, `.zones.json`, `.ano.json`, `.apu.json` and `.sfm.json` became
the last-loaded character's. Between saves the consequence was not only on disk:
each player picked up, ignored and safe-moded by somebody else's rules, saw
another character's personal zones, and had their memorial entries recorded in
another character's log.

Fixed by owning one of each in `player_context` (`src/mp/mp_context.h`) and
pointing the five accessors at the active seat
(`memorial_state()`, `pickup_rules_state()`, `auto_notes_state()`,
`safemode_state()`, `zones_state()`, `src/mp/mp_context.cpp`). A seat's objects
are created on first use, so a process that never leaves the world phase pays
for none of them, and the drain of a dropped seat unsubscribes its memorial
from the event bus (`event_subscriber`'s destructor). Each seat's memorial is
subscribed when it is created, because a log that is not subscribed records
nothing. Cost: three files for the mechanism plus the five owners, an hour
each; the playtime baseline (`.pt`, below) had to move with it.

The test is `tests/mp_save_split_test.cpp`,
`each_character_keeps_its_own_per_character_files`: two characters, distinct
memorial lines, zones, notes, pickup rules and safemode rules, saved through
`game::save()`, then read back from the world folder file by file (`.sav`,
`.sav.log`, `.zones.json`, `.ano.json`, `.apu.json`, `.sfm.json`, `.pt`) and
loaded again with the production loader.

### Zones — breaks materially, fixed (per seat)

`zone_manager` used to be a process-wide singleton. On load, `load_zones()`
(`src/clzones.cpp:1925-1945`, called for **every** character at
`src/game_io.cpp:536`) replaces the whole vector (`jv.read( zones )` clears,
`src/clzones.cpp:1786`) with that character's `.zones.json`, then appends the
world file's non-`your_followers` zones. After loading two characters the
manager held only the last-loaded character's personal and `your_followers`
zones. On save, `save_character_data()` called `save_zones()`
(`src/game_io.cpp:827`) for each slot, but nothing reloaded that slot first, so
**the same manager contents were written into every character's
`.zones.json`**: slot 0's zone file was overwritten with slot 1's zones (and
vice versa) on the first save. Personal zones were also re-anchored to
`get_avatar().pos_abs()` whenever the cache was rebuilt
(`zone_manager::cache_data()`), so they applied at the wrong place for the
other character even before saving.

The brief's candidate `c` (move non-personal `your_followers` zones to the
world file) does not fix this. It changes where one class of zone is stored
(a savegame layout change) while the per-slot save still wrote the shared
manager into each character's file, and it needs legacy migration for old saves
that hold those zones in the character file. Rejected for this task.

Fixed by the same per-seat move as the four sidecars above:
`zone_manager::get_manager()` returns `player_context::zones_state()`
(`src/clzones.cpp:105-115`), one manager per seat. `save_zones()` now writes the
active seat's zones because the save loop activates the seat first, and
`load_zones()` fills only that seat. The world file is unchanged, so old saves
need no migration, and the "split world-owned faction zones into their own
file" question is a separate, cosmetic storage choice again. Cost: two files
(the accessor moved out of line) and the test above, which checks that each
character's `.zones.json` holds its own personal zone and not its partner's.

### NPC followers — breaks materially, deferred

`avatar::follower_ids` is per avatar (`src/avatar.h:424`) and saved with the
avatar (`src/savegame_json.cpp:1633`, `:1724`), but the run-time model is not:
an NPC in `your_followers` is an ally of *any* avatar
(`npc::is_ally()`, `src/npc.cpp:2686-2696`), and every follow/combat decision
resolves `g->nearest_player()` (`src/npcmove.cpp:912`, `:2365-2370`,
`game::nearest_player()` at `src/game.cpp:4719-4737`). So a follower recruited
by slot 1 walks after slot 0 when slot 0 is closer, fights for whoever is near,
and on load `game::validate_npc_followers()` (`src/game.cpp:1851-1872`) adds
every visible ally to the *active* avatar's `follower_ids`; it runs only for
the first character (`settle_game_state()`, `src/game_io.cpp:426-434`, guarded
by `with_game_state`). The surviving character in upstream #67750 is exactly
this: the NPC has follower dialogue but no faction/owner the game agrees on.

A real fix needs an owner on the NPC (`character_id owner` in `npc`, set on
recruit, saved, used by `is_ally`/follow/`validate_npc_followers`): three files
plus a new savegame field. Phase 5, estimate 2-3 days including migration of
existing saves (derive owner from the saved follower lists and faction).

### Vehicles — breaks materially, fixed

`vehicle_part::passenger_id` is a `character_id` (`src/vehicle.h:537`),
serialized with the part (`src/savegame_json.cpp:3712`, `:3778`) and written on
boarding (`src/map.cpp:1641-1673`). It survives a slot swap, but
`vehicle::get_passenger()` resolved it through `game::critter_by_id()`, which
knew only `u` and the NPC tracker. Every consumer therefore saw an empty seat
for a partner: vehicle mass and muscle power (`src/vehicle.cpp:1146`), driver
detection (`get_driver()`, `src/vehicle.cpp:3696-3703`), passenger checks
(`vehicle::is_passenger()`, `src/vehicle.cpp:3668-3679`),
NPC boarding (`src/npcmove.cpp:2415`), unboarding (`src/map.cpp:711`,
`:1712`) and vehicle movement (`src/vehicle_move.cpp:2104`).

Fixed in `f244c4cee2`: `game::critter_by_id()` scans `player_slots` after the
`u` fast path and before `find_npc()`. One source file. Test:
`tests/coop_vehicle_test.cpp` seats slot 1 in a bicycle by writing the same two
fields `map::board_vehicle()` writes and checks both the lookup and
`get_passenger()`. It cannot use `map::board_vehicle()` itself because
`game::update_map()` always recentres on `u` (`src/game.cpp:10478-10555`), a
separate co-op limitation: boarding with a non-active avatar would move the
active one.

### Achievements and statistics — breaks materially, deferred

`stats_tracker`, `achievements_tracker` and `kill_tracker` are single objects
owned by `game` (`src/game.h:1363-1365`), created once and subscribed to the
event bus (`src/game.cpp:484-489`). They are serialized into **every**
character `.sav` (`src/savegame.cpp:144-146`; loaded back at `:350-351`; the
per-slot save loop is `src/game_io.cpp:848-856`). Three concrete consequences:

1. Every character file carries a copy of the same tracker. Those copies are
   not a last-writer-wins load: the co-op loader brings the game state in with
   the **first** continued character (`with_game_state = !have_game_state`,
   `src/mp/mp_server_main.cpp:270-277`), and every later character is read with
   `character_only=true`, which returns after the view offsets and the player
   (`src/savegame.cpp:253-263`). The authoritative tracker is therefore the
   first loaded character's copy; the copies in later characters' `.sav` files
   are never read — dead weight that inflates those saves and misleads anyone
   reading them, not a last-file-wins conflict.
2. Statistics decide "the avatar" from the `avatar_id` statistic, defined as
   the *last value* of the `game_avatar_new` event
   (`data/json/statistics.json:1-8`); kill/achievement statistics filter on it
   (`:17-35`, `:87-93`). That event is emitted when a character starts
   (`src/game.cpp:1109-1110`) or when a player takes over an NPC
   (`src/avatar.cpp:187-188`), never on load. In a co-op world the id is
   whichever character was created last, so the kill counts, scores and
   achievements of one character (and the conduct failures of another) are
   attributed to that one id for the whole world.
3. The human-readable achievement export is written from
   `save_world_data()` before the per-slot loop (`src/game_io.cpp:791-801`,
   `game::save_achievements()` `:701-752`), so it is always stamped with slot
   0's name and character id (`:746-751`). Other characters get no export.

The scout's note that events are "unattributed" is wrong: the generic
character events carry the acting `character` (`event_spec_character`,
`src/event.h:183-190`), the kill events carry `killer` explicitly
(`src/event.h:370-380`), and transformations filter on those fields. The break
is the shared tracker plus the single `avatar_id` statistic.

A real fix means a tracker per character (or per-character filtering with a
current-avatar-relative `avatar_id`) plus a savegame change: phase 5, estimate
2 days. Achievements are deliberately global achievements in the upstream
sense, so the first decision for phase 5 is product-level: shared achievements
with per-character scores, or per-character everything.

### `uistate` — breaks cosmetically

One file per world: loaded from `<world>/uistate.json` in the world half
(`src/game_io.cpp:373-377`), written in `save_world_data()`
(`src/game_io.cpp:797-801`). The fields are UI preferences and history:
sort modes, filters, hidden/favorite recipes, overmap toggles, tileset zoom,
input history (`src/inventory_ui.cpp:363-464`). `read_recipes`/`read_items` are
marks used only by the crafting UI for unread highlighting
(`src/crafting_gui.cpp:2363`, `:2379`, `:2521`), not crafting permission.
Consequence: two clients pick the same filter, sort order, sidebar state and
"unread" markers, and the last writer wins on save. Nothing is owned or lost;
no fix warranted.

## 4. Why the other cheap fixes did not land

| Candidate | Verdict |
|---|---|
| a) missions resolve the assignee | **landed** (`51183bcad2`), plus effect attribution in `mission_util.cpp` |
| b) vehicle lookup sees every slot | **landed** (`f244c4cee2`) |
| c) non-personal `your_followers` zones move to the world file | **not needed for correctness**: the per-slot clobbering was the real break and is fixed by the per-seat manager (`295ad265e7` wave); moving the storage is now a separate layout choice, phase 5 if ever |
| d) camp UI derives known camps from the overmap | **not landed**: the UI would list camps the rest of the camp code (takeover counting, NPC orders, camp conditions) still treats as foreign — a cosmetic patch over a per-character knowledge invariant; phase 5 |

## 5. Tests: what is covered and what needs a human

Landed and covered (each test was run against a build with the fix reverted to
confirm it fails there):

- `tests/coop_vehicle_test.cpp`, tag `[coop][vehicle]`: slot 1's id resolves
  through `critter_by_id<Character>()` and through a real vehicle part's
  `passenger_id`/`passenger_flag`. Without the fix both return `nullptr`.
- `tests/mp_save_split_test.cpp`, tag `[coop]`:
  `each_character_keeps_its_own_per_character_files` saves two characters
  through `game::save()` and reads each character's own `.sav`, `.log`,
  `.zones.json` and `.ano.json` back from the world folder, then loads both
  again through `game::load_character()`. `.apu.json` and `.sfm.json` cannot be
  checked on disk -- their writers refuse to write unless both `<base>.sav` and
  `<base>.sav.zzip` exist (`src/auto_pickup.cpp:903`,
  `src/safemode_ui.cpp:787`), so the per-seat pickup and safemode property is
  asserted in memory.
- `tests/mp_join_test.cpp`, tag `[mp]` (server build): a client that drops
  inside the character creator, one that quits there, and one that drops after
  choosing all cost only their own seat, and the party still starts.
- `tests/mp_character_card_test.cpp`, tag `[mp]`: a card written in another
  world names nobody here.
- `tests/coop_mission_test.cpp`, tag `[coop][mission]`: a `MGOAL_CONDITION`
  mission assigned to slot 1 completes (condition, completed-mission list and
  the `u_add_morale` end effect) when slot 1 carries the item, and stays open
  when only slot 0 carries it. With the owner resolution reverted the test
  fails (completes for the wrong character); with only the effect attribution
  reverted it fails on the morale (slot 0 gets 10, slot 1 gets 0).

Not testable under the current rules, and why:

- Camps, NPC followers, faction reputation and achievements still need a live
  two-character world: the state is world-level, so the failure is behaviour
  rather than a file. A human gate for phase 5 must: check that the faction UI
  lists both camps and both can be ordered; that a follower recruited by slot 1
  does not abandon it for slot 0; that faction standing changes only for the
  character who caused it; and that achievements/scores name the right
  character.
- Zones, memorial, auto notes, auto pickup, safemode and playtime no longer
  need a human: `tests/mp_save_split_test.cpp`,
  `each_character_keeps_its_own_per_character_files`, saves two characters
  through `game::save()` and reads each character's own files back from the
  world folder, then loads them through `game::load_character()` -- the
  production loader, not the forbidden `game::unserialize`.
- Mission nemesis attribution has no fixture short of spawning an overmap
  nemesis horde; it is documented, not tested.

## 6. Verification

- `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1` — clean.
- `tests/cata_test "[mp],[coop]" --rng-seed 1 --user-dir=<fresh>` — all passed
  (40 cases, 197 assertions). The ordinary-build half of the wave's tests.
- `tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=<fresh>` — all passed
  (165 cases, 4024 assertions), including the three join-phase harness cases
  added with the seat-release fixes (a client that drops inside the character
  creator, one that quits there, and one that drops after choosing).
- `tests/cata_test "[mission]" --rng-seed 1` — all passed (2 cases, 72
  assertions).
- `tests/cata_test "[vehicle]" --rng-seed 1` — all passed (109 cases, 487871
  assertions).
- Negative controls: each landed test was run with its fix removed and failed
  for the expected reason (output recorded in the task report). For the
  per-character round trip, putting `get_auto_notes_settings()` and
  `zone_manager::get_manager()` back to process-wide statics fails exactly the
  four zone/note file assertions (39/43 when the fix is present).
- Finding 12's mandatory replay: shard 03 of the phase gate, replayed with
  `tests/cata_test -f .../fullrun-final/shard-03.txt -d yes --order decl
  --rng-seed 1` on a clean build — exit 0 and zero
  `map::unboard_vehicle: vehicle not found` lines, which is base behaviour for
  the invalid-id lookup the narrowed guard restores.
- Full sharded suite (`python3 tools/run_tests_parallel.py -j $(nproc)
  --seed 1`, fresh outdir) at the time of the first audit: 1877 cases, 2 failed. Both failures
  (`NPC-rules-close-doors` and
  `monster_cant_enter_reality_bubble_because_wall`) are members of the phase's
  known pre-existing order-dependent set (five members, recorded as proven or
  presumed pre-existing in
  `.superpowers/sdd/2026-08-08-mp-phase3a-scheduler/progress.md:287-292`), and
  both pass standalone on this build. The
  shard plan was recomputed because the two new test cases changed the test
  set, which is what re-exposed this pair; no new failure appeared.
