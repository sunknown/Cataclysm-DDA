# Phase 4b-2 server — hover describe feed — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Answer a `caps` bit2 client's hover request with the tile text the `x` look
panel would show, as styled runs, gated by the seat's own UI state and a server-side
interval.

**Architecture:** `mp_scene_tile_at` is the single cell↔tile formula shared with
`mp_scene_build`. `mp_describe_extract` is pure text cleanup; `mp_describe_lines` renders
one tile into a scratch curses window through a `cursesport` cell accessor;
`mp_describe_reply` maps the cell and fills the message; the flush serializes and sends
after the scene. `mp_session_ui` owns the per-seat in-game flag and the
describe-pending invariant.

**Tech Stack:** C++17; the legacy `Makefile` MP build; Catch2 tests under `tests/`.

**Spec:** `../Cata3DDA/docs/superpowers/specs/2026-11-22-mp-3d-phase4b2-hover-describe-design.md`

## Global Constraints

- Protocol version 2. `caps` bit2 = describe; `hello_ack.caps` carries the server bits
  (scene 1, log 2, describe 4 = 7). No new message kind; `describe` is a control kind.
  No version bump.
- Response order: screen, cursor, log, **scene, describe**. `frame` is `s->scene_seq`
  after the scene block; the client accepts only a matching frame.
- Per-seat `ui_active`: only `mp_session_ui_*` writes it. `leave` also clears
  `describe_pending` and `describe_last`.
- Server interval: `MP_DESCRIBE_MIN_INTERVAL_MS = 100`, latest-wins coalescing, no queue.
  The wait caps its sleep to the due time; a flood cannot drive the frame rate.
- Render module has no JSON dependency. `mp_describe_extract` is pure; the flush calls
  `mp_control_to_json`.
- Parity is narrow glyphs and foreground colour. The wide-glyph continuation cell and
  background colour are the native log's documented limits.
- 100 columns, English comments, no `Co-Authored-By`. No push.
- Build: `make -j16 CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-`
- Test: `tests/mpserver-cata_test "<filter>" --rng-seed 1 --user-dir=.data/4b2-<task>/`

## Review Focus

1. **Peer-driven frame rate.** A burst of describe messages must produce at most one
   build per interval. Task 8 pins the wait cap, the flush interval, and the backlog drop.
2. **Stale view text.** The reply's `frame` and the scene-before-describe order are the
   only defenders against describing a tile the client no longer shows. Task 8 pins the
   order and the frame.
3. **Per-seat UI leak.** A creation seat must not inherit a playing seat's `ui_active`,
   and leaving the UI must not leave a pending request. Task 4 pins enter/leave and the
   log's `ui` regression.
4. **Text rules.** Trimming, common indent, blank lines, run grouping, and the
   24/256/16 KiB caps are easy to get subtly wrong. Task 5 is the pure seam for all of
   them.
5. **Wide glyphs.** Dropping the continuation cell must be a deliberate, tested limit, not
   a silent shift. Task 5 pins it.
6. **Wait without the baton.** The input wait may read only session fields; the map is
   read in the flush under the baton. Task 8 pins the `holds` gate.

---

### Task 1: Shared cell↔tile geometry

**Files:**
- Modify: `src/mp/mp_scene.h` (declare)
- Modify: `src/mp/mp_scene.cpp:180-198` (shared origin)
- Test: `tests/mp_scene_test.cpp`

**Interfaces:**
- Produces: `std::optional<tripoint_bub_ms> mp_scene_tile_at( const mp_scene_rect &rect,
  const tripoint_bub_ms &view_centre, int cell_x, int cell_y );`
- Consumes: `mp_scene_rect` from `src/mp/mp_scene.h`.

- [ ] **Step 1: Write the failing test**

`TEST_CASE( "mp_scene_tile_at_matches_the_scene_origin", "[mp]" )` with sections:
- rect `{0, 0, 160, 45}`, view `(100, 100, 0)`: cell `(0, 0)` → `(20, 78)`;
  `(80, 22)` → `(100, 100)`; `(159, 44)` → `(179, 122)`.
- the four cells one step outside the rect → `std::nullopt`.
- rect `{30, 5, 40, 10}` with view `(100, 100, 0)`: cell `(30, 5)` → `(80, 95)`.
- odd width: rect `{0, 0, 161, 45}`, cell `(80, 22)` → the view centre, `(160, 22)` →
  `view + (80, 0)`.
- the returned z is the view's z.

- [ ] **Step 2: Run test to verify it fails**

Run: `tests/mpserver-cata_test "mp_scene_tile_at" --rng-seed 1 --user-dir=.data/4b2-t1/`
Expected: compile error, `mp_scene_tile_at` not declared.

- [ ] **Step 3: Implement**

Add an internal `scene_origin( const tripoint_bub_ms &view, int w, int h )` that returns
`view + ( -( w / 2 ), -( h / 2 ), 0 )`, use it in `mp_scene_build` for its existing
origin, and implement `mp_scene_tile_at` as: reject a cell outside `rect`, then
`scene_origin( view, rect.w, rect.h ) + ( cell_x - rect.x, cell_y - rect.y, 0 )`.

- [ ] **Step 4: Run tests**

Run the new test, then `tests/mpserver-cata_test "mp_scene_" --rng-seed 1
--user-dir=.data/4b2-t1/`.
Expected: PASS; existing scene tests unchanged.

- [ ] **Step 5: Commit.** `feat(mp): share the cell-to-tile geometry with the scene builder`

---

### Task 2: Curses window-cell accessor

**Files:**
- Modify: `src/cursesport.h` (declare `window_cell` and `get_cell`)
- Modify: `src/cursesport.cpp` (beside `nc_color_foreground_index`)
- Modify: `src/mp/mp_curses_server.cpp:100-120` (blit reads through the accessor)
- Create: `tests/mp_describe_test.cpp` (guarded by `#if defined(MP_SERVER)`)

**Interfaces:**
- Produces: `namespace catacurses { struct window_cell { std::string ch; uint8_t fg = 0;
  uint8_t bg = 0; }; bool get_cell( const window &win, const point &p,
  window_cell &out ); }`
- The existing `cursecell::FG` is already the palette byte; `get_cell` copies it.

- [ ] **Step 1: Write the failing tests**

In `tests/mp_describe_test.cpp`, `TEST_CASE( "mp_window_get_cell", "[mp]" )`:
- a 2x5 window with `mvwprintz( win, point( 0, 0 ), c_red, "x" )`: `get_cell` returns
  `ch == "x"`, `fg == nc_color_foreground_index( c_red )`, and the cell's `bg`;
- a wide glyph `"日"`: cell 0 carries the glyph, cell 1 carries `ch.empty()`;
- `get_cell` at `(5, 0)`, `(-1, 0)`, and on a default-constructed `catacurses::window`
  returns false and leaves `out` untouched.

- [ ] **Step 2: Run to verify it fails**

Run: `tests/mpserver-cata_test "mp_window_get_cell" --rng-seed 1 --user-dir=.data/4b2-t2/`
Expected: compile error, `get_cell` not declared.

- [ ] **Step 3: Implement the accessor**

Null window, negative point, or a point outside `width`/`height` → false. Otherwise copy
`line[y].chars[x]` into `out`. Do not normalise the wide-glyph continuation cell; the
caller decides.

- [ ] **Step 4: Refactor the screen blit and run the tests**

Replace the three direct `cell.ch`/`cell.FG`/`cell.BG` reads in
`mp_curses_server.cpp` with `get_cell( win_, point( i, j ), c )`. Keep the existing
wide-glyph comment on `mp_cell`.

Run: `tests/mpserver-cata_test "mp_window_get_cell" --rng-seed 1 --user-dir=.data/4b2-t2/`
then `tests/mpserver-cata_test "mp_server_" --rng-seed 1 --user-dir=.data/4b2-t2/`.
Expected: PASS.

- [ ] **Step 5: Commit.** `refactor(curses): read window cells through one accessor`

---

### Task 3: `describe` protocol and capability bits

**Files:**
- Modify: `src/mp/mp_protocol.h`
- Modify: `src/mp/mp_protocol.cpp`
- Modify: `src/mp/mp_server_input.cpp` (`send_hello_ack` sets caps)
- Modify: `src/mp/mp_server_screen.cpp` (replace `1u`/`2u` cap literals)
- Test: `tests/mp_protocol_test.cpp`

**Interfaces:**
- Produces: `constexpr uint32_t MP_CAP_SCENE = 1u; MP_CAP_LOG = 2u;
  MP_CAP_DESCRIBE = 4u; MP_SERVER_CAPS = 7u;`
- Produces: `struct mp_describe_request { uint32_t seq = 0; uint16_t x = 0;
  uint16_t y = 0; uint16_t w = 0; };`
- Produces: `struct mp_describe_msg { uint32_t seq = 0; uint32_t frame = 0;
  uint16_t x = 0; uint16_t y = 0; uint16_t w = 0; std::vector<mp_log_line> lines; };`
- Produces: `mp_hello_ack` gains `uint32_t caps = 0`; `mp_control_kind::describe`;
  `mp_control` gains `mp_describe_msg describe`.

- [ ] **Step 1: Write the failing tests**

Add to `tests/mp_protocol_test.cpp`:
- `hello_ack` with `caps = 7` round-trips; an ack without `caps` parses as 0.
- a request `{"t":"describe","seq":12,"x":147,"y":39,"w":40}` round-trips with empty
  `lines` and `frame == 0`.
- a response with `frame = 417`, two lines, an inline colour run, and an empty inner line
  round-trips text and palette bytes.
- reject: no `x`; 25 lines; 257 runs on one line; 16 KiB + 1 byte of text; colour 256;
  a non-object body. A request without `lines` is accepted.
- an unknown member is ignored (`mp_control_from_json_ignores_a_field_it_does_not_know`
  already covers the pattern; add a describe case).

- [ ] **Step 2: Run to verify it fails**

Run: `tests/mpserver-cata_test "mp_control" --rng-seed 1 --user-dir=.data/4b2-t3/`
Expected: compile error, `mp_describe_msg`/`describe` missing.

- [ ] **Step 3: Implement**

- Add the constants and structs to `mp_protocol.h`; add the enum value and the union
  member.
- `kind_to_string`/`string_to_kind`: `"describe"`.
- `to_json`: `hello_ack` writes `caps`; `describe` writes `seq`, `frame`, `x`, `y`, `w`,
  and `lines` (each run `{t,c}`).
- `from_json`: `hello_ack` reads `caps` (absent 0, clamp at 0). `describe` requires
  `seq`, `x`, `y`, `w` (`has_int`), reads `frame` when present (absent 0), and parses
  `lines` only when `has_array("lines")`, applying the log's 24-line / 256-run / 16 KiB
  limits. Return false on a missing required member or an over-limit body.
- `send_hello_ack( ... )` sets `ack.hello_ack.caps = MP_SERVER_CAPS` on every ack.
- Replace the `( s->caps & 2u )` and `( s->caps & 1u )` literals in
  `mp_server_screen.cpp` with `MP_CAP_LOG`/`MP_CAP_SCENE`.

- [ ] **Step 4: Run the protocol and server tests**

Run: `tests/mpserver-cata_test "mp_control" --rng-seed 1 --user-dir=.data/4b2-t3/` and
`tests/mpserver-cata_test "mp_server_" --rng-seed 1 --user-dir=.data/4b2-t3/`.
Expected: PASS.

- [ ] **Step 5: Commit.** `feat(mp): add the describe control and capability advertisement`

---

### Task 4: Per-seat UI state and the log `ui` regression

**Files:**
- Create: `src/mp/mp_session_ui.h`
- Create: `src/mp/mp_session_ui.cpp`
- Modify: `src/mp/mp_session.h` (`ui_active`, `describe_pending`, `describe_last`)
- Modify: `src/game.cpp:3498` (`draw_panels`)
- Modify: `src/mp/mp_server_screen.cpp:55-75,181`
- Modify: `src/mp/mp_join_menu.cpp:105`
- Modify: `src/mp/mp_server_main.cpp:761-770`
- Modify: `src/mp/mp_log.h`, `src/mp/mp_log.cpp` (remove the process-wide flag and the
  note/active functions; `clear_rect` no longer clears ui)
- Test: `tests/mp_session_test.cpp`, `tests/mp_log_test.cpp`

**Interfaces:**
- Produces: `void mp_session_ui_enter( mp_session &s );`
- Produces: `void mp_session_ui_leave( mp_session &s );` — clears `ui_active`,
  `describe_pending`, and `describe_last`.
- Produces: `bool mp_session_ui_active( const mp_session &s );`
- Produces: `bool mp_session_can_describe( const mp_session &s );` — caps bit2 and
  `ui_active`.

- [ ] **Step 1: Write the failing tests**

`tests/mp_session_test.cpp`:
- a session with caps `MP_CAP_DESCRIBE`; `can_describe` false before `enter`, true after;
  `leave` sets the flag false, resets `describe_pending`, and resets `describe_last`;
- a session with caps 3 stays false after `enter` (no bit2).

`tests/mp_log_test.cpp`:
- the snapshot `ui` follows the session flag: `enter` → `ui=true`; `leave` → `ui=false`.
  Drive it through the existing flush fixture or `mp_log_body` with
  `mp_session_ui_active( *s )`. This is the 4b-1 regression: a creation seat's leave must
  not be overwritten by another seat's draw.

- [ ] **Step 2: Run to verify it fails**

Run: `tests/mpserver-cata_test "mp_session_ui|mp_log_ui" --rng-seed 1
--user-dir=.data/4b2-t4/`
Expected: compile error, `mp_session_ui_*` missing.

- [ ] **Step 3: Implement**

- Add the three fields to `mp_session`; add `mp_describe_request` to `mp_protocol.h`
  (Task 3) so the optional compiles.
- Implement the four functions. `enter` sets the flag; `leave` clears the flag, the
  request, and the timestamp; `can_describe` is the caps test plus the flag.
- Call sites:
  - `game::draw_panels`: `if( mp_session *s = mp_active_session() )
    mp_session_ui_enter( *s );` (replaces `mp_native_log_note_ui()`).
  - `mp_server_blank_screens`: `leave` every session before the grid reset.
  - `mp_join_menu.cpp:105`: `leave` the active session before/with the rect clear.
  - `install_client_on_seat`: `leave` the fresh/retained seat.
- `mp_log.cpp`: delete the static `g_ui_active` and the note/active functions;
  `mp_native_log_clear_rect` clears only the rect.
- `mp_server_screen.cpp`: the log body uses `mp_session_ui_active( *s )`.

- [ ] **Step 4: Run the tests**

Run: `tests/mpserver-cata_test "mp_session|mp_log|mp_server_" --rng-seed 1
--user-dir=.data/4b2-t4/`.
Expected: PASS, including the existing 4b-1 log cases.

- [ ] **Step 5: Commit.** `feat(mp): make the in-game UI flag per seat`

---

### Task 5: Pure text extraction

**Files:**
- Create: `src/mp/mp_describe.h`
- Create: `src/mp/mp_describe.cpp`
- Test: `tests/mp_describe_test.cpp`

**Interfaces:**
- Produces: `constexpr int MP_DESCRIBE_MAX_LINES = 24;
  MP_DESCRIBE_MIN_WIDTH = 20; MP_DESCRIBE_MAX_WIDTH = 80;
  MP_DESCRIBE_MIN_INTERVAL_MS = 100;
  constexpr size_t MP_DESCRIBE_MAX_TEXT_BYTES = 16 * 1024;`
- Produces: `std::vector<mp_log_line> mp_describe_extract(
  const std::vector<mp_cell> &cells, int width, int height );`
- `cells` is row-major, `width * height`; `mp_cell` carries `ch`, `fg`, `bg`.

- [ ] **Step 1: Write the failing tests**

`TEST_CASE( "mp_describe_extract_cleans_cells", "[mp]" )` with sections:
- trailing spaces are removed; a common one-cell left indent is removed while a two-space
  nested indent keeps its extra space;
- leading and trailing blank rows are dropped; an inner blank row becomes an empty line;
- consecutive cells with the same `fg` become one run; an `fg` change starts a new run;
- 30 input rows stop at 24 lines; a 300-run line stops at 256; 16 KiB + 1 byte of text is
  cut at a UTF-8 boundary;
- a wide glyph `"日"` in cell 0 and `ch.empty()` in cell 1 produce the glyph text in the
  run and no extra character for the continuation cell (the known shift);
- `bg` is ignored.

- [ ] **Step 2: Run to verify it fails**

Run: `tests/mpserver-cata_test "mp_describe_extract" --rng-seed 1
--user-dir=.data/4b2-t5/`
Expected: compile error.

- [ ] **Step 3: Implement**

Straight pass over the rows: trim trailing blanks, compute the minimum leading blanks over
non-blank rows, drop that many from every row, drop leading/trailing blank rows, group by
`fg`, and stop at the caps. Do not invent spaces or move text for a continuation cell.

- [ ] **Step 4: Run the test.**

- [ ] **Step 5: Commit.** `feat(mp): extract describe lines from a cell grid`

---

### Task 6: Render one tile

**Files:**
- Modify: `src/mp/mp_describe.h`, `src/mp/mp_describe.cpp`
- Test: `tests/mp_describe_test.cpp`

**Interfaces:**
- Produces: `std::vector<mp_log_line> mp_describe_lines(
  const tripoint_bub_ms &tile, int width );`
- Consumes: `catacurses::get_cell`, `mp_describe_extract`, `game::pre_print_all_tile_info`.

- [ ] **Step 1: Write the failing test**

Use the `mp_scene_test.cpp` fixture shape: `stand_on_a_cleared_map()` equivalent with
`t_pavement`, a 10x40 map window at `(10, 0)`, and `ter_view_p` inside it. Call
`mp_describe_lines( tile, 40 )`; assert at least one line, every run has a non-negative
palette byte, and the concatenated text is non-empty. Add a case with `width = 20` and one
with `width = 80`: the calls do not throw and the widths are respected by the fold (line
cell width ≤ width).

- [ ] **Step 2: Run to verify it fails**

Run: `tests/mpserver-cata_test "mp_describe_lines" --rng-seed 1
--user-dir=.data/4b2-t6/`
Expected: compile error.

- [ ] **Step 3: Implement**

`catacurses::newwin( MP_DESCRIBE_MAX_LINES, width, point::zero )`, `werase`, call
`g->pre_print_all_tile_info( tile, win, 0, MP_DESCRIBE_MAX_LINES - 1,
get_map().get_visibility_variables_cache() )`, read every cell with
`catacurses::get_cell` into a row-major `std::vector<mp_cell>`, and return
`mp_describe_extract( cells, width, MP_DESCRIBE_MAX_LINES )`. Never refresh the window.

- [ ] **Step 4: Run the test.**

- [ ] **Step 5: Commit.** `feat(mp): render a tile into describe lines`

---

### Task 7: Reply envelope

**Files:**
- Modify: `src/mp/mp_describe.h`, `src/mp/mp_describe.cpp`
- Test: `tests/mp_describe_test.cpp`

**Interfaces:**
- Produces: `mp_describe_msg mp_describe_reply( uint32_t seq, uint32_t frame,
  int cell_x, int cell_y, int width );`
- Consumes: `mp_scene_mask_map_rect`, `mp_scene_tile_at`, `mp_describe_lines`.

- [ ] **Step 1: Write the failing tests**

With a 40x10 map window at `(10, 0)` and `ter_view_p = (20, 5, 0)`:
- cell `(30, 5)` → `seq`, `frame`, `x/y`, `w = 40`, and non-empty `lines`;
- cell `(9, 0)` → empty `lines`, same envelope;
- `width = 5` → `w = 20`; `width = 500` → `w = 80`;
- no map window (default `w_terrain`) → empty `lines`;
- `g == nullptr` is not reachable in a game fixture; do not invent a test for it.

- [ ] **Step 2: Run to verify it fails.**

- [ ] **Step 3: Implement**

Null `g`/context, `mp_scene_mask_map_rect` false or zero area, `mp_scene_tile_at` nullopt,
or `!here.inbounds( tile )` → return the envelope with empty `lines`. Otherwise clamp the
width and call `mp_describe_lines`. Set `seq`, `frame`, `x`, `y`, `w`, `lines`.

- [ ] **Step 4: Run the tests.**

- [ ] **Step 5: Commit.** `feat(mp): build the describe reply envelope`

---

### Task 8: Dispatch, wait, and flush

**Files:**
- Modify: `src/mp/mp_server_input.cpp` (`dispatch`, the wait loop)
- Modify: `src/mp/mp_server_screen.cpp` (describe block)
- Test: `tests/mp_server_io_test.cpp`

**Interfaces:**
- Consumes: `mp_describe_reply`, `mp_session_can_describe`, `mp_session_ui_*`,
  `MP_DESCRIBE_MIN_INTERVAL_MS`.

- [ ] **Step 1: Write the failing tests**

In `tests/mp_server_io_test.cpp`, with a loopback pair and a session whose caps include
bit2 and whose `ui_active` is true:
- a describe request is answered after the scene (when scene caps are on) and carries
  `seq`, `frame`, `x`, `y`, and `lines`;
- a client without bit2 receives no describe;
- `ui_active == false` → no describe and `describe_pending` is cleared;
- a seat that does not hold the baton keeps its request pending (use the two-seat harness
  from `mp_scene_stream_test.cpp`);
- `describe_last = now` → no reply; `describe_last = now - 101 ms` → reply;
- 1000 describe requests in one burst → one reply, one wake, `describe_last` advanced;
- a transport over `MP_SCREEN_BACKLOG_BYTES` → no describe, pending cleared;
- a pending request makes `mp_server_wait_for_input( 5000 )` return a timeout within the
  interval, not after the full timeout.

- [ ] **Step 2: Run to verify it fails.**

- [ ] **Step 3: Implement**

- `dispatch`: on `describe`, store the request only when
  `mp_session_can_describe( *s )`; latest wins.
- The wait: when `describe_pending`, compute the due time from `describe_last` and cap the
  `recv` timeout; after `dispatch_frame`, return `make_timeout_event()` when due. Input and
  `pending_resize` keep precedence.
- The flush, after `mp_server_flush_scene()`:
  - clear a pending request when `!mp_session_can_describe( *s )`;
  - require `due` and (`sched == nullptr || sched->holds( context_index )`);
  - drop the request when the transport is over the backlog cap;
  - otherwise `describe_last = now`, fill `mp_control{ kind = describe, describe =
    mp_describe_reply( ... ) }`, and send `mp_control_to_json`.
- The log block above reads `mp_session_ui_active( *s )`.

- [ ] **Step 4: Run the tests**

Run: `tests/mpserver-cata_test "mp_server_io|mp_scene_stream|mp_log" --rng-seed 1
--user-dir=.data/4b2-t8/`.
Expected: PASS.

- [ ] **Step 5: Commit.** `feat(mp): serve describe requests with a server interval`

---

### Task 9: Full gates and manual pass

- [ ] **Step 1: Build and run the whole `[mp]` suite**

```bash
make -j16 CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-
tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=.data/4b2-gate/
```

Expected: pass, no new warnings on the touched files.

- [ ] **Step 2: Run the client gates against this server** (after the client plan lands):
  `dotnet run --project ../Cata3DDA/tests/Cata3DDA.Tests`,
  `dotnet build ../Cata3DDA/Cata3DDA.csproj`,
  `PHASE3_SMOKE=1 dotnet run ... -- SceneSmoke` with `DESCRIBE_OK`.

- [ ] **Step 3: Manual pass.** Hover floor, wall, creature, item, vehicle, field, trap,
  graffiti; compare with `x`; check the sidebar, menu, log, mouse-down, look mode, `z`/`Z`,
  resize, two clients, and an old client. Record the date, commits, and world in
  `../Cata3DDA/docs/roadmap/3d-client.md` only after the run.
