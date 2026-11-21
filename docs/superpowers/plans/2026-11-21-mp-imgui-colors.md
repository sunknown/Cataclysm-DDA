# ImGui colour bridge for the MP server — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore real colours for every text cell the MP server draws through ImGui, so the
join menu and confirmation popups are readable in both the Godot and the curses client.

**Architecture:** The in-memory curses port is the server's only `catacurses::init_pair`
implementation; it fills `cata_cursesport::colorpairs` but not the separate pair table that
`cataimgui::imvec4_from_color()` reads. Feed that table from the same call, under `MP_SERVER`.

**Tech Stack:** C++17, Catch2 2.13.10, in-memory curses (`src/cursesport.cpp`), ImTui/ImGui
colour conversion (`src/cata_imgui.cpp`), server build `MP_SERVER=1`.

**Spec:** `docs/superpowers/specs/2026-11-21-mp-text-client-colors-and-shift-design.md`.

## Global Constraints

- Branch `fix/mp-imgui-colors` from `mp`; merge only after the user's verdict.
- Server build: `RELEASE=0 make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-`.
  Non-server MP build: `RELEASE=0 make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 MULTIPLAYER=1 BUILD_PREFIX=mpclient-`.
- Server gate: `tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=.data/<run>/` — compare
  against the current baseline (240 cases, 0 failed), not absolute zero.
- Client gate: `tests/mpclient-cata_test "[mp],[coop]" --rng-seed 1 --user-dir=.data/<run>/`.
- The change is server-only and must not alter the wire format: same glyphs, same indices.
- Style: 100 columns, English comments; no `Co-Authored-By`; artifacts under `.data/`.
- `astyle` is unavailable; format by hand.

## Review Focus

1. A colour pair allocated at runtime (after `catacurses::init_interface`) reaches the imgui
   table too, not only the pairs registered at startup.
2. Bold colours keep their `+8` shift: light gray and dark gray stay distinguishable, white
   stays bright.
3. Plain (non-MP) test builds and the SDL build are byte-for-byte unaffected: the new call is
   under `MP_SERVER`.
4. The vanilla curses client receives the same bytes as the Godot client and shows the same
   corrected colours.
5. Palette indices stay in 0..255; no pair index outside the 100-entry table is written.

---

### Task 1: Carry the pair index and feed the imgui pair table

**Files:**
- Modify: `src/cursesport.cpp:537-542` (`nc_color::from_color_pair_index`) and
  `src/cursesport.cpp:404-415` (`catacurses::init_pair`)
- Test: `tests/mp_imgui_color_test.cpp` (new; the test wildcard picks it up)

**Interfaces:**
- Consumes: `void cataimgui::init_pair( int p, int f, int b )` (declared under
  `TUI || MP_SERVER` in `src/cata_imgui.h:217`) and
  `ImVec4 cataimgui::imvec4_from_color( const nc_color & )` (`src/cata_imgui.h`).
- Produces: no new symbol; an `nc_color` built by the in-memory port carries its pair
  index, and `catacurses::init_pair` updates both pair tables on the server.

- [ ] **Step 1: Write the failing test**

`tests/mp_imgui_color_test.cpp`:

```cpp
#if defined(MP_SERVER)

#include "cata_catch.h"

#include "cata_imgui.h"
#include "color.h"
#include "cursesdef.h"

TEST_CASE( "the_in_memory_curses_port_feeds_the_imgui_colour_table", "[mp]" )
{
    // The palette arrays are loaded by the imgui client in the game; a unit
    // test has no client, so load them explicitly before asking for a colour.
    cataimgui::load_colors();
    catacurses::init_pair( 90, catacurses::white, catacurses::black );
    const nc_color colour = nc_color::from_color_pair_index( 90 );
    // The imgui conversion reads the pair index off the colour; the in-memory
    // curses port must carry it, not just the attribute bits.
    CHECK( colour.get_index() == 90 );
    const ImVec4 rgba = cataimgui::imvec4_from_color( colour );
    CHECK( rgba.x > 0.5f );
    CHECK( rgba.y > 0.5f );
    CHECK( rgba.z > 0.5f );
}

#endif
```

Note: `catacurses::init_pair` takes a pair index and the base colours; `nc_color`'s pair index
is read back with `from_color_pair_index`. Pair 90 is outside the game's allocated range so no
other test can register it first.

- [ ] **Step 2: Run it and record the RED**

```bash
RELEASE=0 make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-
tests/mpserver-cata_test "the_in_memory_curses_port_feeds_the_imgui_colour_table" \
  --rng-seed 1 --user-dir=.data/colors-t1/
```

Expected: **FAIL** on all four checks — `get_index()` is 0 (the factory drops it) and the
conversion is `(0,0,0)` (the imgui pair table still holds `{0,0}` for pair 90).

- [ ] **Step 3: Implement both halves**

In `src/cursesport.cpp`, carry the index in `nc_color::from_color_pair_index`:

```cpp
nc_color nc_color::from_color_pair_index( const int index )
{
    // The index is what the imgui colour conversion reads; keep it in step with
    // the attribute bits, exactly as the ncurses build does.
    return nc_color( index << 17 & A_COLOR, index );
}
```

add the include:

```cpp
#include "cata_imgui.h"
```

and extend `catacurses::init_pair`:

```cpp
void catacurses::init_pair( const short pair, const base_color f, const base_color b )
{
    cata_cursesport::colorpairs[pair].FG = f;
    cata_cursesport::colorpairs[pair].BG = b;
#if defined( MP_SERVER )
    // This is the server's only catacurses::init_pair: the real ncurses one in
    // ncurses_def.cpp never compiles here. ImGui colours read their own pair
    // table, so mirror the registration or every nc_color reaches the client
    // as palette 0 (black) -- the join menu's unselected labels.
    cataimgui::init_pair( pair, static_cast<int>( f ), static_cast<int>( b ) );
#endif
}
```

- [ ] **Step 4: Run it and record the GREEN**

Run the Step 2 command again.
Expected: **PASS**, one case, all four checks.

- [ ] **Step 5: Commit**

```bash
git add src/cursesport.cpp tests/mp_imgui_color_test.cpp \
  docs/superpowers/specs/2026-11-21-mp-text-client-colors-and-shift-design.md \
  docs/superpowers/plans/2026-11-21-mp-imgui-colors.md
git commit -m "fix(mp): give imgui text its pair index and colour table"
```

---

### Task 2: Live join menu and the gates

**Files:** none (verification only; `.data/` artifacts are not committed).

- [ ] **Step 1: Rebuild and run the server gate**

```bash
RELEASE=0 make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-
tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=.data/colors-gate/
```

Expected: the current baseline (240 cases) plus
`the_in_memory_curses_port_feeds_the_imgui_colour_table`, 0 failed.

- [ ] **Step 2: Run the client gate**

```bash
RELEASE=0 make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 MULTIPLAYER=1 BUILD_PREFIX=mpclient-
tests/mpclient-cata_test "[mp],[coop]" --rng-seed 1 --user-dir=.data/colors-client/
```

Expected: the current client baseline (172 cases), 0 failed.

- [ ] **Step 3: Live join menu**

Use the prepared world copy `.data/phase2-manual/srv` (world `coop-e2e`, character
`Virgilio Fields`) and the Godot client.

```bash
./mpserver-cataclysm-server --server 4405 --world coop-e2e --players 1 \
  --userdir .data/phase2-manual/srv
XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1 \
  godot-mono --path ../Cata3DDA -- --connect 127.0.0.1:4405
```

Press `1` (language), wait for the join menu, screenshot the window, and read it:
each unselected row must show its label — `New character: preset`, `New character: random`,
`Settings`, `Help`, `Quit` — not only the hotkey letter. Compare with
`.data/phase2-manual/20-menu.png` (before) and save the new shot under `.data/phase2-manual/`.

Expected: labels visible, the selected row still highlighted; no other colour regresses.

- [ ] **Step 4: Stop the server and report**

Stop the server and the client. Report the gate numbers, the screenshot path, and the live
result. No commit: Task 1 already carries the change.
