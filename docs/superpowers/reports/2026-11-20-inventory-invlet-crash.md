# Inventory invlet crash: a moved avatar invalidates the wielded item's location

- Date: 2026-11-20
- Branch: `fix/mp-inventory-invlet-crash` @ `bb9299c620` (the repro test; parent `2df87c503f`
  is the plan-document commit)
- Code under study: `inventory_entry::get_invlet()` and the selector input path
  (`src/inventory_ui.cpp`), `game::swap_slot_into_u()` (`src/game.cpp`), `mp_activate_player()`
  / `mp_activate_world()` (`src/mp/mp_context.cpp`), `safe_reference_anchor`
  (`src/safe_reference.cpp`, `src/safe_reference.h`), the `item`/`Character` move operations.
- Evidence: `.data/e2e-save3/srv-repro3.log` (field), `.data/inv-crash/run6.log` (clean
  binary, current source), `.data/inv-crash/diag1.log` and `run5.log` (temporary instrumented
  build), `.data/inv-crash/run1.log`/`run2.log` (green without a world tick).

## Symptom

The MP server dies with `SIGSEGV` (exit 134) inside an open inventory when a key is pressed
after the partner has taken a turn. Both the field session (world `coop-e2e`, two clients
`Bo`/`Ada`, server built at 16:23) and the harness reproduce the same stack, frame for frame:

```
inventory_entry::get_invlet() const                    src/inventory_ui.cpp:636
  <- inventory_column::find_by_invlet(int) const          src/inventory_ui.cpp:702
  <- inventory_selector::find_entry_by_invlet(int) const  src/inventory_ui.cpp:2428
  <- inventory_selector::process_input(...)               src/inventory_ui.cpp:3528
  <- inventory_selector::get_input()                      src/inventory_ui.cpp:3499
  <- inventory_pick_selector::execute()                   src/inventory_ui.cpp:3887
  <- game_menus::inv::common()                            src/game_inventory.cpp:295
  <- game::do_regular_action(...)                         src/handle_action.cpp:2735
  <- game::handle_action()                                src/handle_action.cpp:3507
  <- game::player_act(avatar&)                            src/do_turn.cpp:663
  <- mp_run_player_thread(mp_sched&, unsigned long)       src/mp/mp_server_main.cpp:1152
```

The instrumented logs (`diag1.log`, `run5.log`) show the frames one line higher
(637/709/2439/3539/3510/3898) because the temporary `TMPDIAG` statements were still in
`src/inventory_ui.cpp`; the stack shape and every call are identical.

## Reproduction

```
tests/mpserver-cata_test "inventory_reopen_cycles_do_not_crash" --rng-seed 1 \
    --user-dir=.data/inv-crash/run6
```

`.data/inv-crash/run6.log`: `SIGSEGV` on `cycle := 0`, exit 134, with the stack above. The
case (`tests/mp_session_test.cpp:1873`, committed at `bb9299c620`) drives two seats:

- both seats get 100 moves; seat 0 wields a `rock` (`set_wielded_item`);
- seat 0 opens the inventory (`mp_menu_inventory.key`), the harness waits for the inventory
  marker;
- seat 1 presses `` ` `` (hurry), the countdown is advanced past `MP_HURRY_MS` with
  `mp_advance_hurry_for_test`, seat 1 presses `.`, and the test waits until
  `calendar::turn` advances — a world tick under seat 0's still-open menu;
- seat 0 presses `a`, then `Escape`; four cycles.

Determinism: SIGSEGV on cycle 0 in every run of the sequence (run4 and run6, plus the field
log), not intermittently. The trigger is the world tick, not the reopen count: the same
two-seat menu cycling with no tick in between is green (`run1.log`, `run2.log`, 9 assertions
each). `run3.log` is a partial: the harness failed its marker wait before any input was sent
(1 assertion), so it says nothing about the crash.

## Entry dump

From the temporary instrumented build (`diag1.log`, `run5.log`; the diagnostics were removed
afterwards and `src/inventory_ui.cpp` is clean at `bb9299c620`):

- the dying entry is `where=1` (`item_location::type::character`), `type=rock`, one
  location (`stack=1`), `custom_invlet` unset (`INT_MIN`);
- the item address is constant across cycles: `0x7fb01fd06988` (`run5`: `0x7ee277d06988`);
- after the first world tick, `get_invlet` reports the entry's location dead on every cycle,
  while `add_entry` logs a fresh (then-live) entry at the same address on every reopen;
- a temporary null guard made the run green while logging the same dead entry every cycle —
  the entry is never removed; only the dereference was skipped.

## Verdict

### Which entry

The wielded rock. `add_character_items()` builds it in `own_gear_column` with category
`item_category_WEAPON_HELD` (`src/inventory_ui.cpp:2186-2188`). On a narrow layout
`prepare_layout()` moves the gear column's entries into `own_inv_column`
(`src/inventory_ui.cpp:2466-2470`), so which column the lookup walks depends on the window
width; the entry itself is unambiguous from the dump (`rock`, `where=1`, one location,
no custom invlet).

- `item_location::where()` = `1` = `type::character` (`src/item_location.h:36-42`).
- The `Character` is alive: the location was built for seat 0, which is `g->u`
  (`game::player_at()` returns `*player_slots[index]`, `src/game.cpp:4738-4742`, and
  `player_slots[0] == &u`, `src/game.cpp:484`, asserted after every activation at
  `src/mp/mp_context.cpp:290`). `item_on_person` keeps `character_id` plus
  `Character *who = &g->u` (`src/item_location.cpp:396-399`), and that pointer is never
  invalidated. The crashing member is not the character but the item.

### What was invalidated

The item's `safe_reference`, not the address and not the `Character*`.

- `Character`'s move constructor and move assignment are defaulted
  (`src/character.cpp:585-586`); `avatar`'s are defaulted too (`src/avatar.cpp:148-152`).
  `item weapon` is a direct member (`src/character.h:2880`), and `item`'s move operations are
  defaulted as well (`src/item.cpp:386-389`).
- `item` holds its anchor lazily (`src/item.h:3536`) and hands out references into it
  (`item::get_safe_reference()`, `src/item.cpp:306-309`; `safe_reference.h:52-58`).
- `safe_reference_anchor`'s move constructor and move assignment install a **fresh control
  block** (`src/safe_reference.cpp:20-36`). This is deliberate: `safe_reference.h`'s header
  comment says a reference must be safe "if that item is moved or destroyed".
- `item_location::impl::item_on_person` holds `safe_reference<item> what`
  (`src/item_location.cpp:164-190`, 377-399); `target()` is `what.get()` and `valid()` is
  `!!what && !!who` (`src/item_location.cpp:543-546`). After the anchor is re-seated, `what`
  is expired, `target()` is `nullptr`, `valid()` is false. `item_location::operator->`
  returns `ptr->target()` (`src/item_location.cpp:953-961`), so `any_item()->invlet` at
  `src/inventory_ui.cpp:636` is a null dereference.

### The invalidation event

`game::swap_slot_into_u()` (`src/game.cpp:4751-4766`), lines 4762-4764:

```cpp
avatar temp = std::move( *other );
*other = std::move( u );
u = std::move( temp );
```

`*other = std::move( u )` move-assigns seat 0's whole avatar out of `u`; that memberwise move
assigns its `weapon`, and the item's move assignment re-seats the item's anchor. The callers
are `mp_activate_player()` (`src/mp/mp_context.cpp:284` and `:287`) and
`mp_activate_world()` (`src/mp/mp_context.cpp:425`). Each seat's turn starts with
`mp_activate_player( slot )` (`src/mp/mp_server_main.cpp:1146`) immediately before
`player_act` (`:1152`) — the frame above the crash. So the first swap into the partner's turn
is enough to kill the location; the swap back is not required.

The item address observed in the dump is consistent with `&u.weapon`: the entry is built from
`get_wielded_item()` while seat 0's data sits in `u`, the avatar's contents leave `u` for the
duration of the partner's (and world's) turn, and they return to `u` at the same address when
seat 0's turn resumes — hence the constant address across cycles. The reference still dies,
because it tracks the control block, not the address.

### Why the entry is not rebuilt

`inventory_pick_selector::execute()` loops `redraw()` / `get_input()` without rebuilding
entries (`src/inventory_ui.cpp:3882-3929`); the entries are built once per
`game_menus::inv::common()` iteration (`clear_items(); add_character_items( you );`,
`src/game_inventory.cpp:284-289`). `inventory_column::find_by_invlet()` filters only on
`is_item()` (`:702`), and `is_item()` is `!locations.empty()` (`src/inventory_ui.h:131-135`)
— a dead location is still "an item". `get_invlet()` (`src/inventory_ui.cpp:621-637`) has no
validity check, so the first key after the tick dies in the lookup.

### Why only remote input and a menu that outlives a turn

In single-player `handle_action()` runs on the only thread and blocks in `ctxt.handle_input()`
inside the menu, so no avatar can be moved while the menu is open. In a session each seat is
its own thread (`src/mp/mp_server_main.cpp:1152`); the partner's turn calls
`mp_activate_player()`, which swaps the avatar out of `u`, while the parked seat's menu keeps
the location it built before the swap. The hurry countdown makes exactly this legal: per the
coop design (§7.3) the server "does not reach into another's UI and closes nothing — the menu
stays open", and the accepted price is that "the choice may have gone stale by then". The
field sequence needed the partner's turns (3-6 s pauses), which is why runs without a world
tick are green.

## Hypotheses

**1. A change of the active context between turns, with the menu open, invalidates
`Character*`-locations — confirmed in substance, corrected in detail.** The `Character*`
(`who`) is not invalidated; the character object is never destroyed. What dies is the item's
`safe_reference`, because the character's move operations move the direct `item weapon`
member and `safe_reference_anchor`'s move installs a fresh block. Evidence: the dump says
`where=1` and the location fails `valid()` (which requires both `what` and `who`) while the
item address and the owner id stay the same; the code lines above. This is the cause.

**2. A world tick destroys a map item that had entered the list — rejected for this
crash.** The dying entry is `where=1` (`type::character`), so it cannot be a map item; the
repro adds nothing to the map, and the inventory `i` path only feeds `add_character_items()`
for this case. No evidence anywhere points at a map/vehicle location in this stack.

**3. The entry is reused between selector openings (static or cached state) — rejected.**
Each opening constructs a fresh `inventory_pick_selector` and every iteration calls
`clear_items()` (`src/game_inventory.cpp:275-289`; `clear_items()` empties every column,
including the three named ones, `src/inventory_ui.cpp:2344-2355`). The crash happens inside a
single opening, on the first key after the tick, so no cross-opening reuse is needed. The
ledger's observation that the same dead entry appears "every cycle" is the same opening
pattern repeated after a reopen, not one entry surviving between selectors.

**4. It is not an MP bug; it reproduces on `mp_session_harness( 1 )` — rejected.** With one
seat, `mp_activate_player( 0 )` and `mp_activate_world()` both take the `current == 0` path
and return without swapping (`src/mp/mp_context.cpp:282-288`, `:423-426`); there is no second
slot to swap in. The avatar's contents therefore never move, and a character-owned location
cannot be invalidated by a world tick. Settled by code reading; the green no-tick runs
(`run1`/`run2`) show the tick alone is not the trigger without the swap. A one-seat probe
exists as an uncommitted experiment (`tests/mp_session_test.cpp:1922`, marked TEMPORARY); it
was not run because it postdates the binary that produced `run6`, and the code path above
already settles the question.

## Consequences for the fix

**Where.** `inventory_selector::get_input()` (`src/inventory_ui.cpp:3494-3499`), immediately
after `ctxt.handle_input()` returns and before `process_input()`. Every selector's execute
loop goes through it (`:3885`, `:4222`, `:4520`, `:4555`, `:4813`, `:4985`, `:5213`, `:5271`),
so one prune covers `i`, wear, drop, use, multiselect and reload/ammo, not just the wielded
item's menu. The post-wait point is the only place where the list can be stale: the wait is
exactly where the world runs.

**What.** Walk every column and, for each entry, drop locations that fail `valid()`; erase an
entry whose `locations` becomes empty; recompute category `chosen_count`s and drop empty
category headers; clamp `inventory_column::highlighted_index` (`src/inventory_ui.h:569`) and
re-highlight; then invalidate paging (`paging_is_valid = false`) / `prepare_layout()` so the
cell rectangles and the highlight are recomputed. Leave `custom_invlet` handling alone:
`get_invlet()` must never again be handed a dead location.

**Why this is the spec's ordinary refusal, not a symptom fix.** The dead location is a legal
state that §7.3 accepts ("the choice may have gone stale"; the game answers with an ordinary
refusal). Consumers already answer that way: `Character::use( item_location & )` returns
"You don't have that item." and refunds the moves for an invalid location
(`src/character.cpp:8060-8064`), and the throw path does the same (`src/avatar_action.cpp:1062`).
What is missing is that the *list* keeps a dead location and dereferences it in a getter.
Pruning at the one post-wait update is where the staleness first enters the list; a null check
in `get_invlet()` would keep the dead entry for the rest of the menu's life — the guarded
diagnostic build shows precisely that (green, entry still listed every cycle) — and the dead
location would still flow into selection, `chosen_count`, drag/drop and `obtain()`.

**One trade-off to record in Task 3.** Pruning removes the entry for an item that is still in
the inventory; it reappears on the next open because `item_location` is a snapshot. If the fix
wants the wielded item to stay visible, it can first re-resolve the location by uid within the
character and only drop what no longer exists; that is a display refinement, not a correctness
requirement, and it must stay inside the selector, not change `item_location` semantics.

**Rejected alternatives.**

- A null check in `get_invlet()`/`any_item()`: hides the symptom and leaves a dead location in
  the list, where selection, drag/drop and obtain consume it. Explicitly forbidden by the plan
  before the verdict; the verdict does not change that.
- Re-resolving character locations by uid inside `item_location`/`item_on_person`: `safe_reference`
  invalidates on move by design (`src/safe_reference.h` header comment); changing that for every
  caller is an `item_location` semantics change, and it would also hide genuine consumption.
- Keeping the avatar in `u` while a menu is open, or not swapping: contradicts §7.3 (the
  partner must be able to move) and defeats the per-seat `avatar`.
- Rebuilding the whole selector on every key (`clear_items(); add_character_items( u )`):
  correct but resets filter state, the multiselector's chosen counts and the cursor on every
  key, and costs a full re-add; pruning touches only entries that are actually dead.

## Notes

- `run4.log`/`run6.log` also end with an assertion failure from the harness teardown:
  `mp_sync_context_count(): Assertion 'dying.ui_stack.empty()' failed`
  (`src/mp/mp_context.cpp:404`). That is the session being torn down after the SIGSEGV with
  the menu's `ui_adaptor` still registered, not a second defect.
- The dump came from a temporary instrumented build; the `TMPDIAG` statements were removed and
  `src/inventory_ui.cpp` is clean at `bb9299c620`. Every line number in the verdict is a
  current-source number.
- At the time of writing, `tests/mp_session_test.cpp` carries an uncommitted one-seat probe
  (marked TEMPORARY). It is not part of the committed repro and was not used as evidence; it
  must be deleted before any commit touches that file.
