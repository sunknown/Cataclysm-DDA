# MP text client: ImGui colour table and shifted keys — design (short)

Date: 2026-11-21. Evidence: `.data/phase2-manual/report.md` — the phase-2 manual pass on
server `mp` @ `1b473faa19` with client Cata3DDA @ `9f4038f`.

## Why

The manual pass found two defects that keep the phase-2 plan's Task 7 Step 4 red:

1. In the join menu every unselected row shows only its hotkey; the label text is invisible
   (`20-menu.png`, wire dump in the report). The server sends `fg=16 bg=16`.
2. The `Are you SURE you're finished? (Case Sensitive)` prompt cannot be answered from the
   Godot client: Shift+`Y` reaches the server as `text "y"`, and `FORCE_CAPITAL_YN` rejects it.

## Defect 1 — root cause

On `MP_SERVER` the linked `catacurses::init_pair` is the in-memory one in `src/cursesport.cpp`;
it fills only `cata_cursesport::colorpairs`. `cataimgui::imvec4_from_color()` reads the private
table in `src/cata_imgui.cpp`, which only the real-ncurses path (`src/ncurses_def.cpp`) fills via
`client::upload_color_pair()`. The table stays zero: `palette_index = 0`, so an unmodified
`c_light_gray` becomes palette 0 (black → ansi 16) and a bold colour becomes palette 8
(dark gray → ansi 241, which is why selected text is gray).

### Approaches

- **A (chosen).** Feed the imgui table at the source: `catacurses::init_pair` in
  `cursesport.cpp`, under `MP_SERVER`, also calls `cataimgui::init_pair( pair, f, b )`.
  No dependency on `imclient`; covers startup and every dynamically allocated pair; callable
  from a unit test. Server-only.
- B. Make `imvec4_from_color` read `cata_cursesport::colorpairs` on `MP_SERVER`. Fewer calls,
  but the two tables stay divergent and the server grows a special case.
- C. Treat `fg == bg` as "default colours" in the client. Hides the defect, changes protocol
  semantics, does not help the vanilla client.

## Defect 2 — root cause

`Main.HandleKey` sends `key.Unicode` as-is and ignores `ShiftPressed`. On Wayland the engine
reports the *unshifted* letter for Shift+letter (symbols are correct: `!`, `?`), so Shift+`Y`
becomes `{"seq":[121],"text":"y"}`. The server's YES binding is the character `Y`
(`data/raw/keybindings.json`), which a lowercase char never matches.

### Approaches

- **A (chosen).** Client-side: when Shift is held and the character is a lowercase letter, send
  its uppercase form. This is exactly what the curses client sends (curses reports `Y`), so the
  wire shape matches the reference implementation and no protocol or server change is needed.
- B. Send Shift+letter as a `keyboard_code` event with the shift modifier. Matches the
  keycode-mode binding too, but the server advertises keychar mode and char-only consumers
  would lose the key; more surface for no observed gain.
- C. Accept lowercase `y` for YES on the server. Weakens `FORCE_CAPITAL_YN`; rejected.

Characters under Shift that are already correct (`!`, `?`) stay untouched. `mods` stays empty,
as in the curses client; Ctrl+A..Z keeps the folded control character.

## Non-goals

- No protocol change, no `MP_PROTOCOL_VERSION` bump, no palette redesign.
- No change to `FORCE_CAPITAL_YN` or to any server keybinding.
- The 93 s join-phase watchdog report seen in the same run is a separate ticket.
- Phase 3 (client 3D) is out of scope; it runs in another session.

## Constraints

- Game repo branch `fix/mp-imgui-colors` from `mp`; client branch `fix/shifted-keys` from `main`.
- Commits: `fix(mp):` / `docs(mp):` in the game repo, `fix(client):` / `docs(client):` in the
  client repo; no `Co-Authored-By`. Merge only after the user's verdict.
- Server build `RELEASE=0 ... TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-`;
  gates `tests/mpserver-cata_test "[mp]"` and `tests/mpclient-cata_test "[mp],[coop]"`;
  client suite `dotnet run --project tests/Cata3DDA.Tests`.
- Artifacts under `.data/` only; 100 columns; English comments; no astyle.

## Verification

- Unit: imgui pair conversion for a freshly registered pair (must not be black); shifted-letter
  helper (`a`+shift → `A`, `!` and controls unchanged).
- Live: join-menu labels of unselected rows are readable; Shift+`Y` finishes a random
  character; the map then moves on arrow keys.
- Gates: server `[mp]`, client `[mp],[coop]`, client test suite.

## Left to the builder

Exact test names, assertions, file lines and commands are in
`docs/superpowers/plans/2026-11-21-mp-imgui-colors.md` and
`Cata3DDA/docs/superpowers/plans/2026-11-21-client-shifted-keys.md`.
