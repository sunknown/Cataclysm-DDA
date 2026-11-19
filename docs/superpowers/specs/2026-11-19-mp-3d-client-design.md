# 3D (low poly) client for the multiplayer server — design

Date: 2026-11-19
Status: approved (grilled and corrected against the tree)
Client project: `/mnt/data/apps/Cata3DDA` (separate repository, to be created; Godot 4 Mono + C#)

## Intent and why

The multiplayer server already carries a complete, authoritative world and streams a
per-player text screen to thin clients (`doc/MULTIPLAYER.md`). What it does not stream is
*what the glyphs mean*: a `#` is a wall, a bookshelf or a car as far as the wire is
concerned. The goal is a second, richer view of that same world: a low-poly 3D client that
renders the map the server decides is visible, while the text screen keeps doing what it
does best — every menu, dialog, inventory and crafting screen.

The player experience: same game, same turn-based lockstep, same server, same character;
the map becomes a blocky 3D scene with an orbit camera, the message log and the cursor are
native, everything that is a menu stays a readable text UI.

Why a separate client rather than an in-engine renderer: the multiplayer split is already a
network seam, and using it keeps the client out of the C++ game build, out of the SDL
renderer's variant-pass/renderer-recovery machinery, and in a language chosen for iteration
speed. A second window owned by the C++ client was rejected: two OS windows cannot stack
the terminal under a 3D surface, so the text-over-3D composition that motivates the project
would be lost.

## Goals

1. A player can connect the 3D client to a stock `MP_SERVER` build, create or continue a
   character, play, and see the world in 3D.
2. The whole existing UI remains usable: menus, dialogs, inventory and crafting are the
   server's text grid; the message log, the cursor/reticle and tile hover are native.
3. The 3D scene is derived from game entity ids (`t_wall`, `f_desk`, `mon_zombie`, …) and
   per-tile light, so the client can choose meshes without the server knowing anything
   about 3D.
4. No new art is required to be playable: any id without a model gets a procedural
   primitive from a category rule.
5. The 3D view is as honest as the text one: exactly the layers the text renderer would
   show on a given tile, known traps, unfinished constructions and graffiti included,
   memorized ground shown as terrain plus one decoration, and no creature the avatar
   cannot see is ever sent.
6. The existing thin text client keeps working unchanged — literally: no protocol version
   bump, no recompile.
7. Depth is real: ten z-levels below the player, as in the tiles build.

## Non-goals (v1)

- Overmap in 3D, weather, particles, sound, 3D minimap.
- Native menus, dialogs, inventory, crafting or the numeric sidebars. They stay text
  (see Anti-goals).
- Character appearance (clothing/mutation layers) — creatures are one mesh each.
- Tile animation, vehicle roofs as drivable interiors, multi-frame sprite composition.
- Item layering: one top item per tile; items, fields and traps are not kept in map
  memory, so memorized tiles show terrain and one decoration.
- Furniture carried on a vehicle, and multi-part stacks on one tile.
- Mouse-driven gameplay beyond map clicks and aiming; no edge scrolling.
- Any change to turn/lockstep semantics, or to how the server decides visibility.

## Anti-goals

- **Reimplementing the UI natively in the 3D client.** A native-UI protocol would have to
  describe 304 `uilist` call sites across 103 files, 1164 action registrations, and the
  bespoke screens (inventory, crafting, character sheet, dialogs); every UI change would
  land in two languages, and the text client would stop being a correctness oracle. The
  hybrid split is the design, not a compromise.
- Loading tilesets or SDL on the server.
- Streaming glyphs as the 3D source of truth (a glyph cannot identify an entity).
- Rendering a 3D scene the player could not see in text (no wallhack: the server only ever
  sends what the text renderer would draw on that tile).
- A second input semantics: menus stay keys, the map gets coordinates through the mouse
  path that already exists.
- Breaking wire compatibility for an optional message.

## Constraints

- Server build: `MP_SERVER=1` links an in-memory curses backend and no SDL
  (`doc/MULTIPLAYER.md`). `map::draw_points_cache` / `tile_render_info` are behind
  `#if defined(TILES)` (`src/map.h:2400`), and `map::draw_maptile` is private, so the scene
  builder is written renderer-agnostically and duplicates the draw *rules*, not the draw
  code.
- Scene construction happens while the player's baton is held, inside
  `mp_server_flush_screen()` (`src/mp/mp_server_screen.cpp:108`), which is reached from the
  input wait in `src/mp/mp_curses_server.cpp:267`. The screen flush has three production
  call sites (also the join loop at `src/mp/mp_server_main.cpp:222` and the legacy path at
  `src/mp/mp_server_input.cpp:245`); putting scene construction inside the flush gives all
  of them one choke point instead of three hooks.
- The visibility cache is a single per-map object (`src/map.h:2319`) keyed on the avatar's
  position (`src/map.cpp:7573-7576`) and rebuilt by `game::draw()` for the active context
  (`src/game.cpp:3446-3448`); its recursive build covers ten z-levels down
  (`src/map.cpp:7578-7580`, `fov_3d_z_range = 10` in `src/game_constants.h:28`) and each
  level's `level_cache::visibility_cache` is already computed, so the extractor reads those
  arrays instead of calling `apparent_light_at` per tile.
- Per-player view state: `player_context.viewport` and `ter_view_p`
  (`src/mp/mp_context.h:30-44`); the view centre is `you.pos_bub() + you.view_offset`
  (`src/game.cpp:3615-3630`), not the avatar's position.
- The map window is `TERMY` cells tall and `TERMX - sidebars` wide
  (`src/mp/mp_context.cpp:208-209`), created at `(sidebar_left, 0)`
  (`src/mp/mp_context.cpp:305-306`). It may still be the shared 80×24 fallback until the
  client sends `resize` (`src/mp/mp_server_input.cpp:212` sizes only the session grid), so
  the client must send `resize` immediately after `hello_ack`.
- Wire rules from `mp_protocol.h` / `mp_screen_codec.h`: one kind byte, big-endian, control
  is JSON, hot paths are compact binary, every decoder is a trust boundary that rejects
  malformed input. `MP_PROTOCOL_VERSION` is 2 (`src/mp/mp_protocol.h:14`) and is enforced
  exactly, server-side only (`src/mp/mp_server_input.cpp:178`).
- Old peers already ignore unknown messages: `mp_unwrap` rejects an unknown kind, and both
  dispatchers then drop it (client `src/mp/mp_client_screen.cpp:80-83`, server
  `src/mp/mp_server_input.cpp:105-119`). Unknown `hello` members are ignored
  (`src/mp/mp_protocol.cpp:196`). This is what makes a capability-gated addition safe.
- Mouse input already carries terminal-cell coordinates end to end
  (`src/mp/mp_protocol.cpp:84-131`, conversion at `src/mp/mp_curses_server.cpp:429-451`) and
  the aiming UI already accepts a click as the target (`src/ranged.cpp:3238-3242`).
- Game builds are slow, `[mp]` is 136 test cases, and "green" is a diff against the known
  pre-existing failure set.
- Godot 4 Mono is `extra/godot-mono 4.7.2-2` (not installed yet); `dotnet` is present; no
  engine download.

## Architecture

```
                 MP server (MP_SERVER=1, no SDL)
  world, visibility + seen caches, per-player ter_view_p
        │                 │                    │
        │ screen (glyphs) │ scene (ids, z,     │ log snapshot, tile
        │                 │ light, masks)      │ descriptions (on request)
        ▼                 ▼                    ▼
                3D client (Godot 4, C#)
        text grid (menus)     3D map + native log/cursor/hover
```

### UI ownership (hybrid)

- **Text grid**: every menu, dialog, inventory, crafting screen and the numeric sidebars.
  Cells over the map window are drawn as text on top of a dimmed 3D view; cells outside the
  map window are ordinary text. Screen cells that belong to the 3D map are never drawn as
  glyphs.
- **Native**: the message log, the cursor/reticle, the hover tooltip, and the camera. The
  log is the one window whose *content* the client renders itself; the server tells it the
  rectangle and the messages.
- **Never native in v1**: menus. This is the anti-goal above.

### Server side

New files, plain data / no SDL, built into the MP server and the tests:

- `src/mp/mp_scene.h/.cpp` — `mp_scene` (plain data) and `mp_scene_build( centre, rect, z_levels )`.
  The rect comes from the map window, the centre from `ter_view_p`.
- `src/mp/mp_scene_codec.h/.cpp` — `mp_scene_encode` / `mp_scene_apply`. The decoder has no
  production consumer: it exists as the C++ side of the fixture generator and as a strict
  trust-boundary reference for the C# decoder, and is tested as such.

Both sources go on the always-built MP list (`Makefile:1123-1128`, `src/CMakeLists.txt:16-27`)
so tests link them without `MULTIPLAYER`. Note that CMake has no `MP_SERVER` option; the
server binary is a Makefile build (`Makefile:161-167`).

Extraction walks the rect once per z-level and applies the same predicates as
`map::draw` + `draw_maptile` (see "Extraction rules"). It reads
`access_cache(z).visibility_cache` for `lit_level`, `get_visibility_variables_cache()` for
the character flags, and never calls `apparent_light_at` per tile.

Flush point: `mp_server_flush_screen()` calls a new `mp_server_flush_scene()` after the
grid send. It sends only when the session negotiated the capability, this session is the
active seat, the baton is held, and a meaningful `ter_view_p` exists. Build is gated: if the
terrain window has not been blitted since the last scene send and the mask set is unchanged,
skip without building. A frame is skipped when the hash of the scene body (with `frame_seq`
zeroed) equals the last one; `frame_seq` increments only on a real send.

Per-client scene state lives in `mp_session` (`src/mp/mp_session.h:21-49`), next to `grid`
and the cursor fields: capability flag, requested z-levels, last `frame_seq`, last body
hash, last mask hash.

Map memory: for every seen tile whose stored id differs, the extractor writes terrain and
one decoration id plus rotations/subtile through `avatar::memorize_terrain` /
`memorize_decoration` (`src/avatar.cpp:274-284`), after
`prepare_map_memory_region()` (`src/map.cpp:7719-7720`) — without it the write lands in the
shared scratch submap and is never saved. The ids already serialize
(`src/map_memory.h:48-49`, `src/savegame_json.cpp:4457-4466`); only the tiles build ever
populated them (`src/cata_tiles.cpp:3048` and following). The symbol continues to be
written by the curses path, unchanged.

One latent edge to close: nothing marks the visibility cache dirty when the active avatar
changes. Avatars cannot share a tile today, so nothing leaks; a one-line
`invalidate_visibility_cache()` in `mp_player_activate()` (or the context switch) removes
the assumption.

### Client side

```
project.godot, Cata3DDA.csproj
scripts/Net/        Transport.cs, FrameCodec.cs, ControlMessages.cs,
                    Handshake.cs, SceneDecoder.cs, InputEncoder.cs, CardStore.cs
scripts/Text/       GridRenderer.cs, Palette.cs, CellMetrics.cs, LogView.cs
scripts/World/      SceneModel.cs, SceneView.cs, MeshLibrary.cs, LayerStack.cs, FogView.cs
scripts/Camera/     OrbitCamera.cs (orthographic, yaw/pitch/zoom/pan, follows avatar)
assets/meshes/      id-named .glb/.obj primitives
assets/fonts/       an OFL-licensed monospace TTF with full UTF-8 coverage
data/id_map.json    generated id -> { mesh, height, colour, category }, header names the
                    game commit it was generated from
tools/gen_id_map.py reads the game's data/json (path argument) and emits id_map.json
tests/              plain C# test project: decode committed fixtures, palette mapping,
                    input encoder, mask/role handling
```

Responsibilities:

1. **Transport/handshake** — TCP, `mp_frame_encode` framing, hello with the capability
   member and the requested z count, `resize` immediately after `hello_ack`, card
   persistence on the `character_card` control message (one card per `host:port`, fields
   `server`, `world_name`, `save_id`, `character_name`; semantics of
   `src/mp/mp_character_card.h`). Client repo is GPL-3; `id_map.json` is committed so the
   client build has no dependency on the game tree.
2. **Text overlay** — the `screen` grid is the UI. Cells inside `ui_rect` but outside every
   text mask rect are not drawn; cells inside a text mask are drawn as text over a dimmed
   3D view; everything outside `ui_rect` is text. Glyph palette: 16 base colours plus the
   256-colour space the codec already carries (`src/mp/mp_cell.h`); cells with an empty
   glyph are wide-character continuations and are skipped (`src/cursesport.cpp:308-322`).
   Font: an OFL monospace TTF; cell metrics chosen by the client.
3. **Native log** — the server sends a snapshot of the message log (raw strings + colour,
   newest first); the client wraps to its own width and draws it inside the log rect. No
   history beyond what the server sends.
4. **Input** — canonical mapping, not "whatever the backend produced": `keyboard_char` for
   printable characters and curses-folded control codes, `keyboard_code` only for
   functional keys, with fixtures. Map clicks are sent as mouse events in terminal cells,
   the way the C++ client sends them: `cell = scene origin + tile`. Mouse *moves* are not
   forwarded — hover is client-side and asks the server for a description (debounced);
   edge scrolling is therefore unavailable. Client-local camera keys are consumed and
   documented, never sent.
5. **3D scene** — `MeshLibrary` resolves each id through `id_map.json` with prefix/category
   fallbacks (`t_wall_*` → wall block, `t_floor*`/`t_*` → slab, `f_*` → box, `mon_*`/`npc`/
   `player` → humanoid, unresolved → grey box). Flat per-category colours shaded by scene
   light; no stretched 2D sprites. Wall shape (block, corner, end, window, door) is derived
   on the client from neighbouring terrain ids; the wire carries no connectivity mask.
   Heights come from `id_map.json`. Memorized tiles are desaturated and dimmed (fog of war);
   the client keeps only the last received frame as its cache — the server remains the
   source of truth for memory.
6. **Creatures/items** — creatures are low-poly primitives with distinct silhouettes and
   colours; marker records (other z-levels) are a small icon, never a body; ground items are
   flat procedural icons in v1.

Scale of the mapping task (counted in `data/json`): 1282 terrain, 703 furniture, 1211
monsters, 503 vehicle parts, 163 field types, ~10 500 items. The table is rule-driven with
explicit overrides for the ids that actually appear often.

Primitive budget: block, corner, end, window, door, slab, raised slab, furniture box,
humanoid, marker, item capsule — eleven shapes for phase 3, with per-category colours doing
most of the differentiation.

## Wire protocol (v2, capability-gated)

`MP_PROTOCOL_VERSION` stays 2. The scene stream is an optional extension negotiated in
`hello`; old clients ignore it, and a new client connected to an old server simply receives
no scene and runs as a text client.

Handshake addition (JSON member, ignored by old peers):

| Member | Type | Meaning |
|---|---|---|
| `caps` | int | bit0 = scene stream (implies log snapshot + describe requests) |
| `zl` | int | requested z-levels, 1..10; absent = 10 |

New message kinds:

| Kind | Name | Body |
|---|---|---|
| 3 | `scene` | compact binary, full frame |
| control `log` | log snapshot | JSON: `seq`, `lines[]` of `{t, c}` |
| control `describe` | client → server | JSON: `seq`, tile `x`, `y`, `z_rel`, `w`, `h` |
| control `describe_ack` | server → client | JSON: `seq`, `lines[]` of `{t, c}` |

Unknown kinds and unknown control `t` values are ignored by both dispatchers today; the
tests that assert `mp_unwrap` rejects an unknown kind stay valid.

Scene body (big-endian; every count and index bounds-checked on decode):

| Field | Type | Meaning |
|---|---|---|
| `body_version` | u8 | 1; a bump may add compression behind a flag |
| `frame_seq` | u32 | increments only on a real send |
| `tiles_w`, `tiles_h` | u16 | map window size in cells |
| `view_dx`, `view_dy` | i16 | rect origin relative to the view centre (`ter_view_p`) |
| `avatar_dx`, `avatar_dy` | i16 | avatar cell in rect coordinates; may fall outside |
| `ui_x`, `ui_y`, `ui_w`, `ui_h` | u16 | where the 3D viewport sits |
| `mask_count` | u8 | native/text regions, in draw order |
| mask entry | u16 x, y, w, h; u8 role | role 0 = text over 3D, 1 = native log |
| `veh_count` | u16 | |
| veh entry | u16 veh_id, u16 heading | `vehicle::face.dir()` as 1/65536 turn |
| `id_count` | u16 | dictionary; index 0 is empty |
| id entry | u16 len, bytes | UTF-8 entity id or sanitized graffiti text |
| `z_count` | u8 | encoded layers, player's level first |
| per level | i8 z_rel, then the layer blocks below | |

Per level:

| Block | Entry |
|---|---|
| terrain runs | u16 repeat, u16 terrain_idx, u8 flags, u8 lit |
| furniture | u16 x, u16 y, u16 id_idx |
| items | u16 x, u16 y, u16 id_idx, u8 stack_size |
| fields | u16 x, u16 y, u16 id_idx, u8 intensity |
| vehicle parts | u16 x, u16 y, u16 id_idx, u16 part_index, u16 veh_id, i8 part_dir_256 |
| traps | u16 x, u16 y, u16 id_idx |
| constructions | u16 x, u16 y, u16 id_idx, u8 percent |
| graffiti | u16 x, u16 y, u16 text_idx |
| creatures | u16 x, u16 y, i16 z_rel, u16 id_idx, u8 size_hint, u8 flags |

`flags` on terrain: bit0 visible, bit1 memorized. `lit` is `lit_level` 0..6
(`src/lightmap.h:94-102`). Creature flags: bit0 is_you, bit1 other_body (NPC or co-op slot), bit2 marker_only,
bit3 special_vision. There is no facing byte: the game only knows left/right/none
(`src/creature.h:217-222`), and the client derives orientation from position deltas.

Semantics:

- Full frame per send. Idempotent, so a dropped or skipped frame cannot corrupt the client.
  At ten levels the body is roughly 60–90 KB per player; the change gate keeps it at turn
  cadence, not at every menu poll.
- Coordinates are integer map cells relative to the rect origin; the view centre is the
  rect centre, and when the player looks around the avatar can leave the rect.
- Only ids are sent. Sprite names, tilesets and colours are not protocol data.
- `memorized` tiles carry terrain plus one decoration id and no items, fields, traps,
  vehicle parts or creatures — exactly what `memorized_tile` stores.
- Text mask rects over `ui_rect` are drawn as glyphs on a dimmed 3D view; the log rect is
  drawn natively from the log snapshot; every other cell of `ui_rect` is 3D only.
- Only seen creatures are sent, plus the avatar and co-op slots; `marker_only` records are
  one z-level away and carry no body.
- Graffiti text is sanitized at encode: C0/C1 controls and bidi overrides stripped, length
  capped, UTF-8 validated. Dictionary entries are opaque strings; `id_idx` selects meshes,
  `text_idx` selects text.

## Extraction rules (the honesty contract)

The rule is "what `map::draw` + `draw_maptile` would put on this cell". Per tile, compute
`vis = get_visibility( access_cache(z).visibility_cache[x][y], cache )`:

| Class | Text behaviour | Scene behaviour |
|---|---|---|
| `CLEAR` (levels LOW/LIT/BRIGHT) | full tile layers | send terrain + all eligible layers |
| `DARK`, `HIDDEN`, `BLANK`, `MEMORIZED` | remembered symbol only | memorized record: terrain + one decoration |
| `LIT`, `BOOMER`, `BOOMER_DARK` | only the `#` indicator | terrain only, no layers, no flag |

Per-layer predicates, all inside `CLEAR`:

| Layer | Predicate |
|---|---|
| terrain | always |
| furniture | non-null `furn_id` |
| trap | `tr_at(p).can_see(p, you)` — unknown traps are **not** sent |
| field | `field_count() > 0` |
| items | `sees_some_items(p, you)` — sealed containers are **not** revealed; top item is `i_at(p).back()`, stack size `i_at(p).size()` |
| vehicle part | `veh_at(p)` present; one part per tile, `part_index()`, `info().id`, `direction` |
| construction | `partial_con_at(p)` non-null; `counter / 100000` as percent |
| graffiti | `has_graffiti_at(p)`; text via `graffiti_at(p)` (the text renderer only tints, never prints) |
| creature, same z | `you.sees(here, critter)`, or the avatar itself |
| creature, z±1 | `you.sees()` **and** `valid_move(pos, pos±above/below)`; marker only |
| co-op slot | same z, inside the rect, unconditional — no `sees()`, no markers |

`game::all_creatures()` contains only the active avatar (`src/game.cpp:11100-11111`);
co-op slots are a separate `get_players()` pass, matching `draw_ter`'s unconditional pass
(`src/game.cpp:3646-3658`).

## Client rendering details

- **Camera**: orthographic orbit, yaw/pitch/zoom, pan clamped inside the sent rect; recentre
  on avatar on movement. Free panning reveals nothing: the rect is all the client has.
- **Fog**: memorized tiles are rendered from the server's memorized records, desaturated
  and dimmed. Neither side keeps memory the other does not have.
- **Masks**: role 0 rects are text over a dimmed 3D view; role 1 is the native log. A window
  with nothing but transparent cells still counts: every cell a window actually wrote is in
  the mask.
- **Hover**: on pointer rest, the client sends a `describe` request for the hovered tile and
  draws the returned lines in a native tooltip; responses are cached per tile and throttled.
- **Cursor/reticle**: client-side. Look mode centres the view, so the look cursor is the
  rect centre; aiming is mouse-driven and the click itself is the target.

## Delivery phases (each phase end-to-end runnable)

1. **Server scene stream.** `mp_scene` + codec + `[mp]` tests; `caps`/`zl` in `hello`;
   kind 3; build inside `mp_server_flush_screen()` with the change gate; mask roles from
   blit-time bookkeeping; layer predicates; memory ids; ten z-levels; cache invalidation on
   avatar activation.
2. **Client text client.** Transport, handshake, card, `resize`, text grid, palette, font,
   input canon, native cursor — a fully playable text client in Godot, proving the seam.
3. **Client 3D map.** Terrain, walls, floors, light, fog, orbit/pan camera, mask handling.
4. **Client entities and native HUD.** Creatures (including markers), items, fields, traps,
   constructions, graffiti, vehicle parts with heading; native log and hover.
5. **End-to-end smoke and data pipeline.** Real server, id_map generator over the game's
   data, cold-start behaviour with a save written before ids existed, manual pass through
   inventory, crafting, log, look and aiming.

## Testing and verification

- **Server** (`[mp]`, same test binary; harness `mp_session_harness` provides a real map,
  avatars, `ter_view_p` after a draw and per-client grids): scene extraction on a fixed
  synthetic map (exact ids, light, flags); round-trip through encode/decode; decoder
  rejection of truncated, oversized, wrong-size and bad-index payloads; unseen monster
  absent, seen present, one-z creature `marker_only`, co-op slot present without `sees()`;
  known trap present, unknown trap absent; sealed container contents absent; unfinished
  construction and graffiti present; a tile that leaves sight returns with the ids it was
  seen with; mask roles; hash skip does not advance `frame_seq`; ten levels encode in the
  documented order.
- **Client**: committed binary fixtures produced by the C++ codec test; fixtures for
  palette mapping and the input encoder (both silent-corruption risks); a headless Godot
  run (`--headless`) against a local server asserting a non-empty scene and a non-empty
  text grid.
- **Manual**: run `cataclysm-server` (Makefile build), connect the Godot client, create a
  character, walk, look, aim, open inventory and crafting; the text UI stays usable while
  the map is 3D, and the log and tooltip stay readable.

## Verified facts this design relies on

- `MP_PROTOCOL_VERSION` is 2 (`src/mp/mp_protocol.h:14`); the server is the only enforcer
  (`src/mp/mp_server_input.cpp:178`). `hello` is JSON (`src/mp/mp_protocol.cpp:144-152`).
- Unknown kinds are ignored by both dispatchers (`src/mp/mp_client_screen.cpp:80-83`,
  `src/mp/mp_server_input.cpp:105-119`).
- `mp_server_flush_screen()` has three production call sites and runs at every blocking
  input wait; screens are dirty-row incremental, not full frames.
- There is no window registry or frame log; `wnoutrefresh` → `curses_drawwindow`
  (`src/cursesport.cpp:179-195`, `src/mp/mp_curses_server.cpp:80-105`) is the only hook, and
  it knows only the window being blitted.
- The `cursor` control message exists but has no production caller (`ui_adaptor`'s cursor
  restore is TUI-only, `src/ui_manager.cpp:228-234`).
- Mouse input is terminal cells end to end; the server subtracts the capture-window origin
  (`src/mp/mp_curses_server.cpp:429-451`); mouse aiming already works
  (`src/ranged.cpp:3238-3242`).
- `memorized_tile` carries terrain/decoration ids in every build and serializes them
  (`src/map_memory.h:48-49`, `src/savegame_json.cpp:4457-4466`); only the callers are
  TILES-only (`src/cata_tiles.cpp:1`).
- `memorize_terrain` / `memorize_decoration` are blind stores that need a prepared memory
  region (`src/map_memory.cpp:221-249,435-452`).
- The visibility cache recurses ten levels down (`src/map.cpp:7578-7580`) and is keyed on
  the avatar position; avatar activation does not invalidate it.
- `map::draw_maptile` is private (`src/map.h:2170-2178`); `draw_maptile` gating is entirely
  in `map::draw` (`src/map.cpp:7749-7781`); traps use `can_see`, items use
  `sees_some_items` (`src/map.cpp:6601-6640`, `7902`, `7985-7986`); graffiti is only a red
  tint (`src/map.cpp:8030-8031`).
- Creature facing is only NONE/LEFT/RIGHT (`src/creature.h:217-222`); size is
  `creature_size` (`src/creature.h:75-90,768`).
- The map window is `TERMY` × (`TERMX - sidebars`) cells (`src/mp/mp_context.cpp:208-209`),
  and the shared root may stay 80×24 until `resize`.
- Terminus is `data/font/Terminus.ttf` (OFL, `LICENSE.txt:11`); item art is not in this
  tree — the default tileset lives in the external CC-BY-SA `CDDA-Tilesets` repository.
- `game::print_all_tile_info` renders localized description lines into a curses window
  (`src/game_ui_tile_info.cpp:66`), which is what the `describe` response reuses.

## Decisions taken (why the document looks like this)

- Separate client, because text-over-3D composition needs one window; the C++ client stays
  the reference implementation and must keep building and passing `[mp]` on every protocol
  change.
- Hybrid UI, not native menus: 304 `uilist` sites and the bespoke screens make a native UI
  a second project, and the text client is the correctness oracle.
- Capability-gated extension under protocol v2 instead of a version bump: old clients keep
  working unchanged, which is goal 6.
- Full frames with a hash skip, not diffs: idempotence removes a class of client bugs.
- Ten z-levels, matching the tiles build; the change gate absorbs the cost.
- Coordinates relative to the view centre plus an explicit avatar cell, so looking around
  and panning do not misplace the avatar.
- Masks from blit-time per-cell provenance, compressed to rects: there is no window
  registry to enumerate, and this answers "all windows or only opaque ones" by construction.
- Layer gating mirrors the text predicates, including `can_see` for traps and
  `sees_some_items` for items.
- Map memory is filled by the extractor with a prepared region and a change check; saves
  written before this change show memorized ground as fallback primitives until re-seen.
- The scene decoder is a fixture generator and reference decoder, not production code.
- Client repo GPL-3, `id_map.json` committed with the source commit in its header.
- Primitive budget: eleven shapes in phase 3, colours per category.

## Left to the builder

- Exact Godot scene structure and node naming; the C# test runner.
- The `describe` response layout details (how many lines, whether the client asks for a
  specific width or lets the server wrap) — a builder call tested against the look screen.
- `id_map.json` generator heuristics and the override list for frequent ids.
- Whether the native log also replaces the sidebar "Log" panel in v1 or only the full-screen
  log; both are the same rect-and-role mechanism.
- Compression, if a real session measures the scene body as a problem; the body version
  byte reserves the space for it.
