# Phase 4b-1 server — native log feed — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Send the process-wide CDDA message log to a `caps` bit1 client as one JSON
control snapshot (`log`) carrying styled runs, palette indices, the source-panel rect,
`LOG_FLOW`, and a `ui` flag, gated by a content hash.

**Architecture:** `Messages::sidebar_snapshot` reads the private deque with
`display_messages`' rules and must not advance `curmes`. `split_colored_text` is extracted
from `print_colored_text` so the wire and the glyph path resolve inline colour tags with
the same code. `src/mp/mp_log.*` owns the JSON body, the source-rect/UI state, and the
hash. `mp_protocol` gains one control kind. `mp_server_flush_screen` sends on change.

**Tech Stack:** C++17; the legacy `Makefile` MP build; Catch2 tests under `tests/`.

**Spec:** `../Cata3DDA/docs/superpowers/specs/2026-11-22-mp-3d-phase4b1-native-log-design.md`

## Global Constraints

- Protocol version 2; `caps` bit1 = log snapshot. No new message kind; `log` is a control
  kind. Screen and scene traffic are unchanged.
- The log is process-wide. No per-seat filtering. `sidebar_snapshot` must not advance
  `curmes`; `display_messages` stays the only owner of "the log was displayed".
- Wire colours are palette indices 0..255. Today the curses backend supplies 16 base
  colours plus 8 for bold.
- Visibility must match `Messages::display_messages`, including the asymmetry: a
  cooldown-hidden message is skipped only when `!log_from_top`.
- `MP_SERVER`-only pieces are guarded; text-only and non-MP builds stay identical.
- 100 columns, English comments, no `Co-Authored-By`. No push.
- Build: `make -j16 CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-`
- Test: `tests/mpserver-cata_test "<filter>" --rng-seed 1 --user-dir=.data/4b1-<task>/`

## Review Focus

1. **`curmes` must not move.** A snapshot that advances it turns every message grey and
   changes coalescing for the whole party. Task 2 pins it.
2. **Cooldown asymmetry.** `new_top` keeps messages `new_bottom` hides; "fixing" it would
   make the native log disagree with the glyph panel the server still draws.
3. **TTL.** Truncating the oldest is silent today; a wrong clock seam makes every message
   ancient or fresh. Task 2 uses an explicit clock and action counter.
4. **UTF-8 truncation.** Cutting mid-codepoint produces an unparseable client snapshot.
   Task 5 pins the boundary.
5. **Hash stability.** A body hash that includes `seq` sends every flush. Task 5 pins it.
6. **`ui` vs rect.** A toggled-off panel is `ui=true` with a zero rect; join and character
   creation are `ui=false`. Task 3 pins the transitions, Task 6 the flush.

---

### Task 1: `split_colored_text` extracted from `print_colored_text`

**Files:**
- Modify: `src/output.h` (add `colored_text_run` and the declaration)
- Modify: `src/output.cpp:240` (`print_colored_text` becomes a loop)
- Create: `tests/mp_log_test.cpp` (guarded by `#if defined(MP_SERVER)`)

**Interfaces:**
- Produces: `struct colored_text_run { std::string text; nc_color color; };`
- Produces: `std::vector<colored_text_run> split_colored_text( std::string_view text,
  nc_color initial, nc_color base, report_color_error color_error );`
- `print_colored_text` keeps its signature and behaviour: it writes every run with
  `wprintz` and leaves the `color` output parameter equal to the last run's colour (or the
  incoming colour when the text produces no runs).

- [ ] **Step 1: Write the failing tests** in `tests/mp_log_test.cpp`:
  `TEST_CASE( "mp_log_split_colored_text", "[mp]" )` with sections: a plain string is one
  run with the base colour; `<color_red>b</color>` becomes three runs and the middle uses
  `c_red`; an unmatched close tag leaves the initial colour in place (pass
  `initial = c_green`, `base = c_white` and expect green after `</color>`); a non-colour
  tag stays literal; an empty string yields no runs.
- [ ] **Step 2: Run to verify it fails.**
  Run: `make ... && tests/mpserver-cata_test "mp_log_split_colored_text" --rng-seed 1 --user-dir=.data/4b1-t1/`
  Expected: compile error, `split_colored_text` not declared.
- [ ] **Step 3: Implement** the extraction in `output.cpp` and rewrite
  `print_colored_text` as a loop over the result. Preserve empty-segment skipping, the
  `seg[0] == '<'` branch, `rm_prefix` for colour tags, and the final `color` value.
- [ ] **Step 4: Run the new test and the existing output tests.**
  Run: `tests/mpserver-cata_test "mp_log_split_colored_text" --rng-seed 1 --user-dir=.data/4b1-t1/` → pass;
  then `tests/mpserver-cata_test "~[.]" --rng-seed 1 --user-dir=.data/4b1-t1/` is too wide —
  instead run `tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=.data/4b1-t1/` and
  confirm the pre-existing `[mp]` cases are unchanged.
- [ ] **Step 5: Commit.** `feat(mp): extract split_colored_text from print_colored_text`

---

### Task 2: `Messages::sidebar_snapshot`

**Files:**
- Modify: `src/messages.h`
- Modify: `src/messages.cpp`
- Modify: `tests/mp_log_test.cpp`

**Interfaces:**
- Produces: `struct log_run { std::string text; nc_color color; };`
- Produces: `struct log_message { std::vector<log_run> runs; };`
- Produces: `std::vector<log_message> sidebar_snapshot( size_t max_messages );`
- Produces (test seam): `std::vector<log_message> sidebar_snapshot_at( size_t max_messages,
  time_point curmes, int user_actions, bool from_top );`
- `sidebar_snapshot` calls the seam with `player_messages.curmes`,
  `g->get_user_action_counter()`, and `log_from_top`; it never writes `curmes`.

- [ ] **Step 1: Write the failing tests** (`"mp_log_sidebar_snapshot", "[mp]"`):
  newest first and `x N` expansion; an old message is one run with
  `remove_color_tags()` text coloured `get_color()`'s grey; a recent message keeps inline
  runs; the `max_messages` cap keeps the newest; TTL via the seam's `user_actions`
  (`message_ttl` set, the counter beyond the insertion); a hidden message is skipped when
  `from_top` is false and kept when true; `curmes` is unchanged after the call.
- [ ] **Step 2: Run to verify it fails.** Same filter; expected: `sidebar_snapshot` undeclared.
- [ ] **Step 3: Implement** in `messages.cpp`: iterate `player_messages.messages` newest to
  oldest; stop at the TTL check against the passed `user_actions`; skip
  `is_in_cooldown()` only when `!from_top`; `col = m.get_color( curmes )`; not recent →
  `{ remove_color_tags( m.get_with_count() ), col }`; recent → `split_colored_text(
  m.get_with_count(), col, col, report_color_error::no )` mapped to `log_run`.
- [ ] **Step 4: Run the test.** Expected: pass, and the pre-existing `[mp]` cases stay green.
- [ ] **Step 5: Commit.** `feat(mp): snapshot the sidebar message log as styled runs`

---

### Task 3: palette index, source rect, UI state

**Files:**
- Modify: `src/cursesport.h`, `src/cursesport.cpp`
- Create: `src/mp/mp_log.h`, `src/mp/mp_log.cpp` (rect/UI part only in this task)
- Modify: `src/panels.cpp` (`draw_messages`), `src/game.cpp` (`draw_panels`)
- Modify: `src/mp/mp_server_screen.cpp` (`mp_server_blank_screens`),
  `src/mp/mp_join_menu.cpp` (`mp_run_join_menu`), `src/mp/mp_server_main.cpp`
  (`install_client_on_seat`)
- Modify: `Makefile` (add `src/mp/mp_log.cpp` next to `mp_server_mask.cpp`)
- Modify: `tests/mp_log_test.cpp`

**Interfaces:**
- Produces: `uint8_t nc_color_foreground_index( const nc_color &color );` — pair FG, plus
  8 for bold; `wattron` uses it for its FG path.
- Produces: `void mp_native_log_note_rect( int x, int y, int w, int h );`
  `bool mp_native_log_source_rect( mp_scene_rect &out );`
  `void mp_native_log_clear_rect();` (also clears UI)
  `bool mp_native_log_panel_enabled();`
  `void mp_native_log_note_ui();` `bool mp_native_log_ui_active();`
- `mp_native_log_note_ui` is called at the start of `game::draw_panels`;
  `mp_native_log_note_rect` from `draw_messages` after `werase` and before drawing;
  `mp_native_log_clear_rect` from the three transition sites.

- [ ] **Step 1: Write failing tests** (`"mp_log_palette_index", "[mp]"` and
  `"mp_log_source_rect", "[mp]"`): `c_white` → 15? (pin the actual table: grey/red/bold);
  note/read/clear round-trip; a zero rect reads false; `ui` false after clear and true
  after `note_ui`; `mp_native_log_panel_enabled()` false for an empty manager.
- [ ] **Step 2: Run to verify it fails.** Expected: undeclared symbols.
- [ ] **Step 3: Implement** the palette function by factoring `wattron`'s FG arithmetic;
  add the rect/UI state to `mp_log.cpp`; wire the four call sites; add the file to the
  Makefile next to `mp_server_mask.cpp`.
- [ ] **Step 4: Run the tests.** Expected: pass; `[mp]` stays green.
- [ ] **Step 5: Commit.** `feat(mp): record the native log source rect and UI state`

---

### Task 4: `log` control kind and its JSON

**Files:**
- Modify: `src/mp/mp_protocol.h`, `src/mp/mp_protocol.cpp`
- Modify: `tests/mp_protocol_test.cpp` and `tests/mp_log_test.cpp`

**Interfaces:**
- Produces: `mp_control_kind::log`.
- Produces: `struct mp_log_run { std::string text; uint8_t color; };`
  `struct mp_log_line { std::vector<mp_log_run> runs; };`
  `struct mp_log_msg { uint32_t seq; bool ui; uint16_t x, y, w, h; bool top;
  std::vector<mp_log_line> lines; };`
- Adds `mp_log_msg log;` to `mp_control`; the JSON uses the spec's keys
  `seq/ui/x/y/w/h/top/lines[{t,c}]`.
- Parsing clamps `w/h` to the 16-bit range, rejects more than 200 lines or 256 runs per
  line, and rejects a run length that exceeds 64 KiB in total.

- [ ] **Step 1: Write failing tests**: round-trip a `log` control with two lines and an
  inline colour; a malformed body (bad `t`, wrong types) returns false; a 201-line body is
  rejected; the client never sends one is not testable here.
- [ ] **Step 2: Run to verify it fails.** Expected: unknown kind / undeclared members.
- [ ] **Step 3: Implement** `kind_to_string`/`string_to_kind` entries, the switch cases in
  both directions, and the structs. Keep unknown fields ignored (`allow_omitted_members`).
- [ ] **Step 4: Run the protocol tests.** Expected: pass.
- [ ] **Step 5: Commit.** `feat(mp): add the log control message`

---

### Task 5: `mp_log_body` and the body hash

**Files:**
- Modify: `src/mp/mp_log.h`, `src/mp/mp_log.cpp`
- Modify: `tests/mp_log_test.cpp`

**Interfaces:**
- Produces: `std::string mp_log_body( bool from_top, bool ui, const mp_scene_rect &rect,
  uint32_t seq );`
- Produces: `uint64_t mp_log_hash( const std::string &body );`
- `mp_log_body` builds an `mp_control` with `kind = log`, calls `sidebar_snapshot(200)`,
  maps every colour through `nc_color_foreground_index`, applies the limits (200 lines,
  256 runs per line folded into the last run, 64 KiB of UTF-8 truncated at a boundary),
  and returns `mp_control_to_json`. `mp_log_hash` is FNV-1a 64 over the body with the
  `seq` digits replaced by a stable placeholder.

- [ ] **Step 1: Write failing tests** (`"mp_log_body", "[mp]"`): the body parses back into
  the same `mp_log_msg`; a colour maps to the palette byte; the 64 KiB cap truncates
  without splitting a codepoint; `mp_log_hash` is equal for `seq=1` and `seq=2` but
  changes when a run's text changes.
- [ ] **Step 2: Run to verify it fails.** Expected: undeclared.
- [ ] **Step 3: Implement** both functions; read the existing message log via
  `Messages::sidebar_snapshot`, not a second copy of the filter.
- [ ] **Step 4: Run the tests.** Expected: pass.
- [ ] **Step 5: Commit.** `feat(mp): build the log snapshot body and hash`

---

### Task 6: session fields and flush gating

**Files:**
- Modify: `src/mp/mp_session.h` (`scene_caps` → `caps`, add log fields)
- Modify: `src/mp/mp_server_input.cpp` (hello), `src/mp/mp_server_main.cpp`
  (`install_client_on_seat` reset), `src/mp/mp_server_screen.cpp` (flush)
- Modify: `tests/mp_log_test.cpp`

**Interfaces:**
- Session: `uint32_t caps`, `uint32_t log_seq = 0; uint64_t log_hash = 0;
  bool log_sent_once = false;` reset on seat install like the scene fields.
- Flush, after the cursor and before the scene: when `( caps & 2u ) != 0`, build
  `mp_log_body( log_from_top, mp_native_log_ui_active(), rect_or_zero, s->log_seq + 1 )`,
  hash it, and send a control frame when `!log_sent_once || hash != log_hash`.

- [ ] **Step 1: Write failing tests** with the session harness:
  bit1 gets one `log` control, bit0 gets none; an unchanged log sends nothing; a changed
  message advances `seq`; `ui` false is sent on a blank screen; a panel-off in-game state
  sends `ui=true` with a zero rect; seat replacement resets the send state.
- [ ] **Step 2: Run to verify it fails.** Expected: the harness sees no `log` frame.
- [ ] **Step 3: Implement** the rename and the flush block; update every `scene_caps` use.
- [ ] **Step 4: Run the full `[mp]` suite.** Expected: green; the existing scene stream
  cases unchanged.
- [ ] **Step 5: Commit.** `feat(mp): stream the native log snapshot on change`

---

### Task 7: gates

- [ ] Run the full `[mp]` suite and record it:
  `python3 tools/run_tests_parallel.py -b tests/mpserver-cata_test "[mp]"` (or the
  single-binary equivalent if the script is unavailable).
- [ ] Confirm a text client (caps 0) receives no `log` frame in the harness.
- [ ] Commit nothing; this is the plan's completion evidence.
