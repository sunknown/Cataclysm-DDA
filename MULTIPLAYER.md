# Testing hotseat co-op via the debug menu

## Build

Build with the `MULTIPLAYER` flag enabled (adds `src/mp/*.cpp` and gates the
debug menu entry behind `#if defined(MULTIPLAYER)`):

- CMake: `-DMULTIPLAYER=ON`
- Make: `MULTIPLAYER=1`

## Enable debug mode + bind the debug menu key

1. In-game, go to **Options** and set **Debug Mode** to a debug build/on
   (or launch with `--debug` / a debug config, depending on your build).
2. The "Debug menu" action (`id: "debug"` in
   `data/raw/keybindings.json`) has **no default key binding**. Open the
   keybindings menu (`?` then `1`), search for "Debug menu", and bind a
   key to it (e.g. `` ` `` or `F9`) if you haven't already.
3. Press that key in-game to open the debug menu.

## Toggle hotseat co-op

1. In the debug menu, go to the **Player** category (or fuzzy-search
   "hotseat").
2. Select **"Toggle hotseat co-op"**.
   - First time: prompts you to pick slot 2's starting gear — "Bare
     avatar (no gear)" or a saved `.template`. Creates a second avatar
     at slot 0's position and activates hotseat mode
     (`mp_mode_set_active(true)`).
   - Selecting it again while active disables hotseat mode and hands
     control back to slot 0.

## What to expect while testing

- Turn order (`mp_hotseat_do_turn()` in `src/mp/mp_hotseat_loop.cpp`):
  each turn, slot 0 acts to completion (moves exhausted), then slot 1
  acts to completion, then the world phase (`g->do_turn()`) runs and
  both slots' moves/needs are replenished. It's literal pass-the-keyboard
  hotseat, not simultaneous control.
- The reality bubble is recentered each turn on the midpoint between the
  two slots (`mp_bubble_anchor()`), clamped to slot 0's z-level.
- A tether limits how far apart the two slots can wander: a warning
  message once you cross the warning band, and a hard block (movement
  undone, turn ended) past the limit — see `mp_tether_check()` in
  `src/mp/mp_tether.cpp`.
- To stop testing, reopen the debug menu and toggle hotseat off again.

## Automated tests

`tests/hotseat_test.cpp` and `tests/hotseat_sim_test.cpp` exercise this
same path without needing manual play.
