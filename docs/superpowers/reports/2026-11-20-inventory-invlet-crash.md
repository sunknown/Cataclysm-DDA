# Inventory invlet crash: a dead `item_location` left in the open selector

- Date: 2026-11-20
- Branch: `fix/mp-inventory-invlet-crash` (from `mp` @ 2df87c503f)
- Code under study: `inventory_entry::get_invlet()` (`src/inventory_ui.cpp:636`), the avatar swap
  in `mp_activate_player()` (`src/mp/mp_context.cpp:283-289`) and `game::swap_slot_into_u()`
  (`src/game.cpp:4751-4766`), `safe_reference_anchor` (`src/safe_reference.cpp:32-36`).

## Symptom

The server dies with SIGSEGV inside `inventory_entry::get_invlet()` while a player presses a key
in an inventory menu that has been open across a turn boundary. It is not a key-specific crash:
any keypress reaches `inventory_selector::process_input()` and, through
`find_entry_by_invlet()`, calls `get_invlet()` on every entry of every column. A menu that has
one dead entry cannot be closed — the probe below crashed on `Escape`.

The entry is not a dangling pointer: `is_item()` is `!locations.empty()`
(`src/inventory_ui.h:133`) and the location list is non-empty. The location's `item` reference is
dead, so `any_item()->invlet` (`src/inventory_ui.cpp:636`) reads through a null `item *`.

## Reproduction

`tests/mp_session_test.cpp:1873`, `inventory_reopen_cycles_do_not_crash`, on the server build
(`make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver- RELEASE=0`):

```
tests/mpserver-cata_test "inventory_reopen_cycles_do_not_crash" --rng-seed 1 \
    --user-dir=.data/inv-crash/run6
```

Crash of record: `.data/inv-crash/run6.log`, test start 19:54:02.29, `cycle := 0`,
exit 134. The sequence per cycle is `i` (open), partner presses the hurry key, the countdown is
expired, the partner ends the turn, then a key is sent to the seat whose menu is open.

Green controls, same test case before the barrier tick was added:
`.data/inv-crash/run1.log` (17:20:49, all tests passed, 9 assertions) and
`.data/inv-crash/run2.log` (17:26:02, same). The key sequence alone is harmless; the turn
boundary is what kills the entry.

Determinism: `run4.log` and `run6.log` both die on `cycle := 0` with the same stack, so it is
not intermittent. `run3.log` is a partial (the harness failed a marker wait before any input),
so it says nothing either way.

## Stack

```
inventory_entry::get_invlet() const           src/inventory_ui.cpp:636
inventory_column::find_by_invlet(int) const   src/inventory_ui.cpp:702
inventory_selector::find_entry_by_invlet      src/inventory_ui.cpp:2428
inventory_selector::process_input             src/inventory_ui.cpp:3528
inventory_selector::get_input                 src/inventory_ui.cpp:3499
inventory_pick_selector::execute              src/inventory_ui.cpp:3887
game_menus::inv::common                       src/game_inventory.cpp:295
```

The same stack, at `src/inventory_ui.cpp:636`, was reproduced a second time by the temporary
location probe (`.data/inv-crash/locprobe2.log`, run at 20:08:44) when it pressed `Escape`
after the turn tick.

## Dump

From `.data/inv-crash/diag1.log` (temporary instrumentation, since removed from the source; the
line numbers in its stack are shifted by the instrumentation itself):

```
17:31:47.176 TMPDIAG add_entry: where=1 type=rock addr=0x7fb01fd06988
17:31:47.416 (dead-location backtrace: get_invlet <- find_by_invlet <- process_input)
17:32:11.825 TMPDIAG add_entry: where=1 type=rock addr=0x7fb01fd06988
17:32:11.983 TMPDIAG get_invlet dead location: where=1 stack=1 custom_invlet=-2147483648
```

| Property | Value | Source |
|---|---|---|
| column | `own_gear_column`, merged into `own_inv_column` | `inventory_ui.cpp:2183-2187` |
| category | `item_category_WEAPON_HELD` ("Weapon held") | `inventory_ui.cpp:2186-2187` |
| item | wielded `rock` | `diag1.log:21` |
| stack | one location (`stack=1`) | `diag1.log:77-91` |
| `where()` | `character` (`where=1`) | `diag1.log:21`, `item_location.cpp:377` |
| `custom_invlet` | `INT_MIN` (unset, the item's own invlet is read) | `diag1.log:77` |
| item address | `0x7fb01fd06988`, same on all four `add_entry` lines | `diag1.log:21,80,84,88` |

The dump records no slot index and no `character_id`, so the owner is an inference from the test
setup rather than a reading: the case wields the rock on `g->player_at( 0 )`
(`tests/mp_session_test.cpp:1884`), slot 0 is `&g->u` whenever it is active
(`src/game.cpp:484`), and every activation asserts that (`src/mp/mp_context.cpp:291`).

The location does not hold a `Character *`. `item_on_person` keeps a `character_id` plus a
lazily resolved `who` (`src/item_location.cpp:377-400`) and tracks the item itself through
`safe_reference<item> what` (`src/item_location.cpp:179`). The item side is what died, not the
owner: `item_on_person::valid()` needs both (`src/item_location.cpp:543-547`), and resolving
`who` logs `Failed to find item_location owner with character_id` when the character is gone
(`:388`). `diag1.log` carries no such line, so `who` resolved and `what` did not.

## Verdict

**The turn boundary's avatar swap replaces the wielded item's `safe_reference_anchor`; the
selector's entry keeps the old, expired reference.** Confirmed by code and by a direct
experiment.

The chain, every link in the tree:

1. `mp_activate_player()` swaps the whole avatar once per baton handoff
   (`src/mp/mp_context.cpp:283-289`).
2. `game::swap_slot_into_u()` moves avatars through `avatar temp = std::move(*other);
   *other = std::move(u); u = std::move(temp);` (`src/game.cpp:4762-4764`) — three
   move-assignments into avatar objects that keep their addresses.
3. `Character`'s move assignment is defaulted (`src/character.h:4072`), and `item weapon` is a
   direct member (`src/character.h:2880`), so the item is move-assigned in place.
4. `item`'s move assignment is defaulted (`src/item.cpp:389`) and carries
   `lazy<safe_reference_anchor> anchor` (`src/item.h:3536`) through
   `std::optional<safe_reference_anchor>` (`src/cata_lazy.h:77`).
5. `safe_reference_anchor::operator=( safe_reference_anchor && )` installs a **fresh** control
   block (`src/safe_reference.cpp:32-36`), so every pre-existing `safe_reference` — a
   `weak_ptr` (`src/safe_reference.h:53`) — expires although the item object is still alive: it
   was move-assigned, not destroyed.
6. `item_location::impl::item_on_person::valid()` is `!!what && !!who`
   (`src/item_location.cpp:543-547`) and `operator bool()` forwards to it
   (`src/item_location.cpp:938-941`); `get_invlet()` never asks, it calls `any_item()->invlet`
   (`src/inventory_ui.cpp:636`).

Only the weapon dies because inventory and worn items live in lists whose nodes are stolen by
the move (`invstack = std::list<std::list<item>>`, `src/inventory.h:38,293`; `std::list<item>
worn`, `src/character_attire.h:84`); their item objects never move, so their anchors are never
replaced. Direct item members are.

The empirical confirmation (temporary case in `tests/mp_session_test.cpp`, deleted before this
report was committed): with two seats and the same barrier sequence, an `item_location` taken
from `g->player_at(0).get_wielded_item()` before the tick is valid, and after the tick it
reports `valid=0` while a fresh lookup still resolves and names `rock`
(`.data/inv-crash/locprobe2.log`, run started 20:08:44). It is not the same `item` object at the
same address: the first run of that probe asserted the address identity and failed,
`0x7ebc2430c988` against `0x7ebc71106988` (`.data/inv-crash/locprobe.log:36-40`), because during
the partner's turn the seat's data lives in the slot object rather than in `u`
(`src/game.cpp:4762-4764`). The name surviving while the address does not is why the entry's
snapshot cannot be trusted. The second run then crashed in `get_invlet` on `Escape`.

### Hypotheses

| # | Hypothesis | Verdict | Evidence |
|---|---|---|---|
| 1 | the avatar swap expires the anchor | **confirmed** | chain above; `locprobe2.log` |
| 2 | a tick destroys a map item in the list | rejected | `where=1`; `game_inventory.cpp:289` |
| 3 | the entry is reused between openings | rejected | per-`i` selector, `game_inventory.cpp:274` |
| 4 | the bug is not MP-specific | rejected | no swap for one seat, `game.cpp:4752` |

Hypothesis 2 deserves a sentence of its own: a world tick under an open menu *can* invalidate a
`map` location whose item another player removed, and that is a real risk of the same family. It
is not this crash: the crashing selector is `inventory_pick_selector` in
`game_menus::inv::common()`, which is built from `add_character_items( you )` alone
(`src/game_inventory.cpp:289-292`), and the dump says `where=1`.

Hypothesis 4, in detail. With one slot, `mp_activate_player( 0 )` performs no swap at all:
`swap_slot_into_u()` returns immediately for index 0 (`src/game.cpp:4752-4754`) and its callers
in `mp_activate_player()` are gated on a non-zero index (`src/mp/mp_context.cpp:283-289`). The
other swap sites are gated the same way: `mp_activate_world()` swaps only when the current slot
is non-zero (`src/mp/mp_context.cpp:423-426`), and `remove_extra_player_slots()` swaps only to
bring a non-zero slot's data back (`src/game.cpp:4777-4783`). With one slot `active_slot()` is
always 0, so the swap path is never entered and a one-seat session has no anchor replacement to
suffer from, whatever its barrier does.

That is the mechanism; the probe is the observation. A temporary `mp_session_harness( 1 )` case
opened the inventory four times, started the manual hurry and let its countdown expire with the
menu open, then pressed `a` and `Escape`. It passed with 13 assertions
(`.data/inv-crash/oneseat2.log`, 20:03:53-20:04:15). `.data/inv-crash/oneseat2.log` printed
`after advance: state=5 hurry=-1 barrier=1 turn=0 before=0` for all four cycles, and
`before advance: state=4 hurry=15000` for the first (`state=5` for the rest); both it and
`.data/inv-crash/oneseat.log` (20:01:36-20:01:58) printed
`world ticked under the open one-seat menu: 0` for every cycle. The calendar did not advance
under the open menu in any of the four cycles (eight world polls each), so the lone seat was
never even in the situation that kills the location; and the drop alone does not move an avatar
either: with two seats it takes the swap in `mp_activate_player()` to expire the anchor. The
single-player path has no scheduler at all (`src/do_turn.cpp:933-936`, "Single player: no
scheduler and no thread").

### The teardown assert is not a second defect

`cata_assert( dying.ui_stack.empty() )` (`src/mp/mp_context.cpp:404`) inside
`mp_sync_context_count()` (:345) fires at the end of `run6.log` and of `locprobe2.log`, always
*after* the fatal SIGSEGV, and never in a run that did not crash (`run1.log`, `run2.log`,
`oneseat2.log`); `diag1.log`, an instrumented run that only logged errors and completed, has no
such line either. The comment above the assert names exactly this state: an adaptor "on a thread
that did not unwind" (:391-393). The thread that owned `game_menus::inv::common()`'s
`ui_adaptor` died inside `get_invlet()`, so it never unwound and the context still had the menu
on its stack. Verdict: teardown-after-crash. It should disappear with the crash; the Task 3
GREEN run is the check.

## Rejected alternatives

- **A null check in `get_invlet()`.** Hides the symptom and leaves a dead `item_location` in the
  list: it stays selectable, keeps being drawn, and `execute()` can hand it back to callers
  through `any_item()` (`src/inventory_ui.cpp:3910,3914`). The plan forbids it before a verdict
  for the same reason.
- **Re-resolving `item_on_person` by item uid.** Would change `item_location` semantics
  game-wide; the class's contract is that it "is invalidated by many operations (including
  copying of the item)" (`src/item_location.h:31`).
- **Rebuilding the whole list after the wait.** Loses the filter and the highlight that
  `common()` restores between rounds (`src/game_inventory.cpp:292-294`) and buys nothing: the
  list is rebuilt per opening already.

## Consequences

- **Fix direction (Task 3):** drop entries whose `item_location` is dead at the top of
  `inventory_selector::get_input()` (`src/inventory_ui.cpp:3494`). That covers the eight call
  sites that go through it (:3885, :4222, :4520, :4555, :4813, :4985, :5213, :5271) and the
  selectors built on them — `i`, `wear`, `drop`, `use`, multiselect, ammo. It does **not** cover
  every selector: `trade_selector::execute()` (`:5353`) reads keys through
  `_ctxt_trade.handle_input()` (`:5363`) and calls
  `process_input( ctxt.input_to_action( iev ), iev.get_first_input() )` itself (`:5381`), with
  character `item_location`s in its entries. Task 3 must prune there as a second call site (or
  share one prune helper between the two); the eight-site figure is a count of `get_input()`
  callers, not of every menu that can hold a dead location.
- **A dead entry is legal, a dead dereference is not.** The spec accepts the staleness that the
  lazy timeout creates; what it promises is a refusal, not a crash. §7.3
  (`docs/superpowers/specs/2026-08-04-cdda-p2p-coop-design.md:367-374`), verbatim:

  > По истечении сервер **не лезет в чужой UI и ничего не закрывает** — он снимает с этого игрока
  > обязанность держать барьер на этот тик.

  > Цена: выбранное может к тому моменту протухнуть (предмет унесли, станок сломали). Игра ответит
  > обычным сообщением об отказе, как отвечает и в одиночной игре.

  [When the countdown expires the server does not reach into another's UI and closes nothing —
  it takes the duty to hold the barrier off that player for this tick. The cost: the choice may
  have gone stale by then (the item was carried off, the workbench broken). The game will answer
  with the ordinary refusal message, as it does in single player too.]

  The server must not reach into the menu to fix it, and it does not: the menu is asked to
  discard what it can no longer use. One consumer already answers that way: with a dead location
  `Character::use()` prints "You don't have that item." and refunds the moves
  (`src/character.cpp:8059-8063`). The list is the one place that dereferences instead of asking.
- **Why only remote input, why only after a boundary.** The invalidation is another thread's
  turn boundary, not a keystroke: the first opening of the menu is safe (run1, run2), and the
  seat must be dropped from a barrier it still holds — a partner acting, or the hurry countdown
  expiring — for the avatar to be swapped under it. In the field this looked cumulative because
  each `i`/`a`/`Escape` round was one more chance for a partner's turn to land inside an open
  menu.
- **Draw order, for Task 3 to check.** `inventory_pick_selector::execute()` calls
  `ui_manager::redraw()` before `get_input()` (`src/inventory_ui.cpp:3884-3885`), and the draw
  path reads invlets through the same `get_invlet()`. In both captured runs the fatal read came
  from `process_input` (the key that ends the wait), not from a draw, but the drop must not
  leave a dead entry drawable between the tick and the next `get_input()`; the regression case
  covers the close-and-reopen cycle, which exercises both.
- **One trade-off for Task 3.** Dropping a dead entry also removes the row for an item that is
  still in the inventory, because `item_location` is a snapshot of a control block, not a
  re-resolvable handle; the row returns on the next opening. Keeping it visible would mean
  re-resolving within the selector (by uid, against `character_id`), which is a display
  refinement and must not change `item_location` semantics.
- **Nothing is owed to `mp_context.cpp`.** The teardown assert is downstream of the crash; with
  the entry dropped, the menu closes normally and the context unwinds.
