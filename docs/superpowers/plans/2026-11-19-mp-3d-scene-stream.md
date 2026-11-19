# MP 3D scene stream (phase 1, server) — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The multiplayer server sends each scene-capable client a full, honest per-player 3D scene frame (ids, light, masks, ten z-levels) alongside the existing text screen, with no protocol version bump and no change for text-only clients.

**Architecture:** A plain-data `mp_scene` built from one walk of the map window per z-level, encoded by a strict big-endian codec into a new message kind, and flushed from inside `mp_server_flush_screen()` while the player's baton is held. Masks come from blit-time per-cell provenance, not from a window registry, and are compressed to role-tagged rectangles.

**Tech Stack:** C++17, the existing Catch2 test binary, the existing MP scheduler/tether/session code, no SDL.

**Spec:** `docs/superpowers/specs/2026-11-19-mp-3d-client-design.md`

**Scope:** phase 1 only (server). The Godot client (phases 2–5) is a separate plan in the client repository.

## Global Constraints

- `MP_PROTOCOL_VERSION` stays **2**. Scene is a capability-gated addition: `hello` member `caps` (bit0 = scene), optional `zl` (1..10, default 10). Do not bump the version, do not break text-only clients.
- Scene is message kind **3**; body starts with `u8 body_version = 1`, then `u32 frame_seq`.
- z-levels: **ten** (clamped by `fov_3d_z_range` and `-OVERMAP_DEPTH`), player's level first, `z_rel` 0..-9.
- Extraction mirrors the text renderer: tile class `get_visibility(...) == CLEAR` gates layers; `BRIGHT_ONLY`-class tiles (`LIT`/`BOOMER`/`BOOMER_DARK`) carry terrain only; traps use `tr_at(p).can_see(p, you)`; items use `sees_some_items(p, you)`; co-op slots are unconditional; z±1 creatures are `marker_only` and require `valid_move` across the level.
- Scene construction runs **only** inside `mp_server_flush_screen()`, only for the session whose seat holds the baton, only when the session set `caps` bit0, and only when the terrain window has been blitted since the last send or the mask changed.
- Hash skip: FNV-1a over the encoded body **excluding** `body_version` and `frame_seq` (fixed 5-byte prefix). Equal to the last hash → send nothing, `frame_seq` unchanged.
- Mask rects are in absolute terminal cells; `ui_rect` is in absolute terminal cells; roles: 0 = text over 3D, 1 = native log. Only windows intersecting the map window are tracked.
- New sources go on the always-built MP list (`Makefile:1123-1128`, `src/CMakeLists.txt:16-27`) and into `tests/` (picked up by the wildcard). Every new `tests/mp_*.cpp` is wrapped in `#if defined(MP_SERVER)` (or `#if defined(MULTIPLAYER)` for cases that must also run in the client build), so a non-MP build compiles an empty translation unit, matching `tests/mp_session_test.cpp`.
- Build/test: server `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver-`, then `tests/mpserver-cata_test "<names>" --rng-seed 1 --user-dir=.data/3d-t<N>/`; non-server client build `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 MULTIPLAYER=1 BUILD_PREFIX=mpclient-`, then `tests/mpclient-cata_test "[mp],[coop]" --rng-seed 1 --user-dir=.data/3d-t<N>/`; full gate `python3 tools/run_tests_parallel.py -o .data/3d-full --seed 1 -j $(nproc)`, compared as a diff against the known failing set (five full-suite, three in `mp_server_backend_test.cpp`).
- `astyle` before each commit; no `Co-Authored-By` trailers; scratch files and the test runner never committed.
- Repository rule in force: this plan carries no C/C++/C#/JSON source. Tests are pinned by name and exact assertions; APIs by exact names and semantics.

## Review Focus

1. A tile leaves sight between two frames: the memorized record must carry the terrain and decoration ids it was last seen with, not blanks or a grey box.
2. Two players with different viewports in the same turn: each scene must be built for its own seat and its own `ter_view_p`, never the previous seat's.
3. A dialog or inventory opened over the map: masks and roles must keep the text legible and must not leave map glyphs drawn where 3D lives.
4. An old client without `caps`: zero scene frames, no protocol errors, screen byte-identical to today.
5. A hostile or oversized scene body and a hostile graffiti string: the decoder rejects the body without partial application, and the graffiti text is sanitized and length-capped.

---

### Task 1: Scene model and codec

**Files:**
- Create: `src/mp/mp_scene.h`, `src/mp/mp_scene_codec.h`, `src/mp/mp_scene_codec.cpp`
- Test: `tests/mp_scene_codec_test.cpp`
- Modify: `Makefile:1123-1128`, `src/CMakeLists.txt:16-27`

**Interfaces:**
- Produces: `mp_scene` with fields `body_version` (u8), `frame_seq` (u32), `tiles_w`/`tiles_h` (u16), `view_dx`/`view_dy`/`avatar_dx`/`avatar_dy` (i16), `ui_x`/`ui_y`/`ui_w`/`ui_h` (u16), `masks` (`std::vector<mp_scene_rect>` with `x,y,w,h` u16 and `role` u8), `vehicles` (`std::vector<mp_scene_vehicle>` with `veh_id`, `heading` u16), `ids` (`std::vector<std::string>`, index 0 always empty), `levels` (`std::vector<mp_scene_level>`).
- Produces: `mp_scene_level` with `z_rel` (i8) and vectors `terrain` (runs of `repeat` u16, `terrain_idx` u16, `flags` u8, `lit` u8), `furniture`, `traps`, `graffiti` (`mp_scene_point_id` with x, y, id_idx), `items`, `fields`, `constructions` (`mp_scene_point_id_aux` with x, y, id_idx, aux u8), `vparts` (`mp_scene_vpart` with x, y, id_idx, part_index, veh_id u16, dir i8), `creatures` (`mp_scene_creature` with x, y u16, z_rel i16, id_idx u16, size_hint, flags u8).
- Produces: `std::vector<uint8_t> mp_scene_encode( const mp_scene & )`, `bool mp_scene_apply( mp_scene &, const std::vector<uint8_t> & )`, `uint64_t mp_scene_body_hash( const std::vector<uint8_t> & )`.

- [ ] **Step 1: Write the failing codec tests**

Tests in `tests/mp_scene_codec_test.cpp`, tag `[mp]`:
- `mp_scene_codec_round_trips_every_block`: build a scene with a non-empty dictionary, two locations of the same terrain in a run, every overlay vector populated, one vehicle, two mask rects (roles 0 and 1), two z-levels; encode, decode into a fresh scene, assert every field equal, including run compression (the encoder must emit one run of repeat 2 for the repeated terrain, not two runs of 1).
- `mp_scene_decoder_rejects_malformed_bodies`: empty body, body truncated at every block boundary (loop over prefixes), `body_version` 0 and 2, `z_count` 11, `repeat` 0, `mask` role 2 — each returns false and leaves the destination scene unchanged.
- `mp_scene_decoder_rejects_out_of_range_indices`: `id_idx` equal to `ids.size()`, tile `x` equal to `tiles_w`, creature `z_rel` -11 — each returns false.
- `mp_scene_body_hash_ignores_the_frame_prefix`: two encodings identical except `body_version` and `frame_seq` produce equal hashes; changing one terrain id changes the hash.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver- && tests/mpserver-cata_test "mp_scene_*" --rng-seed 1 --user-dir=.data/3d-t1/`
Expected: build fails with missing `mp_scene.h` (first), then FAIL "no such test" until declared.

- [ ] **Step 3: Implement the model and codec**

Encode in the spec's field order: `body_version`, `frame_seq`, header, masks, vehicles, dictionary, `z_count`, then per level `z_rel` followed by terrain runs, furniture, items, fields, vparts, traps, constructions, graffiti, creatures. Bounds-check every count and index in `mp_scene_apply` and validate in one pass before mutating the destination. `mp_scene_body_hash` hashes bytes from offset 5 to the end with FNV-1a. Register both sources in the Makefile and `src/CMakeLists.txt` always-built lists.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `make -j$(nproc) CCACHE=1 TESTS=1 MULTIPLAYER=1 MP_SERVER=1 BUILD_PREFIX=mpserver- && tests/mpserver-cata_test "mp_scene_*" --rng-seed 1 --user-dir=.data/3d-t1/`
Expected: 4 test cases pass.

- [ ] **Step 5: Commit**

Commit subject: `feat(mp): add the 3D scene model and codec`.

---

### Task 2: Extraction core — geometry, terrain, visibility, map memory

**Files:**
- Create: `src/mp/mp_scene.cpp`
- Test: `tests/mp_scene_test.cpp`
- Modify: `Makefile:1123-1128`, `src/CMakeLists.txt:16-27`

**Interfaces:**
- Consumes: Task 1's types.
- Produces: `mp_scene mp_scene_build( const tripoint_bub_ms & view_centre, int tiles_w, int tiles_h, uint8_t z_levels )`. It reads the map and the **active** avatar (`get_avatar_legacy()`), fills `tiles_w`/`tiles_h`, `view_dx = -(tiles_w / 2)` and `view_dy = -(tiles_h / 2)` (rect cell 0 maps to `view_centre + (view_dx, view_dy)`), `avatar_dx`/`avatar_dy` = avatar position minus `view_centre` minus `(view_dx, view_dy)` (i.e. the avatar's cell in rect coordinates, possibly outside the rect), the dictionary, and the levels. It leaves `ui_rect`, `masks`, `frame_seq` zeroed for the flush to fill; vehicles are filled by Task 3.

- [ ] **Step 1: Write the failing extraction tests**

Tests in `tests/mp_scene_test.cpp`, tag `[mp]`, built on `mp_session_harness` (real map, avatars, `ter_view_p` after a draw):
- `mp_scene_build_emits_visible_terrain_runs`: `build_test_map("t_pavement")`, build with 1 z-level at the avatar; assert one level, `z_rel == 0`, every tile covered exactly `tiles_w * tiles_h` times by the runs, all flags bit0 set, dictionary index 0 empty and the terrain id resolvable to `t_pavement`.
- `mp_scene_build_marks_dark_tiles_as_memorized`: put a wall between the avatar and a tile, build once (tile invisible, so memorized record with empty ids is acceptable), then move the avatar so the tile is seen, build (ids written to memory), move back so it is unseen, build again and assert the tile is a memorized run (bit1 set, bit0 clear) with the terrain id it was seen with and one decoration id.
- `mp_scene_build_fills_map_memory_ids`: after one build, for a sampled seen tile assert `has_memory_at()` is true and `player_map_memory->get_tile(...).get_ter_id()` equals the map's terrain string id; assert the write also lands when the memory region was not prepared beforehand (the builder must call `prepare_map_memory_region()` itself).
- `mp_scene_build_covers_ten_z_levels`: ask for 10 levels, assert level count 10 and `z_rel` values 0..-9 in order; ask for 11 and assert it is clamped to 10.
- `mp_scene_build_omits_layers_for_vision_effect_tiles`: give the avatar `effect_boomered`, build, and assert tiles of class `BOOMER`/`BOOMER_DARK` carry terrain only and no overlay entries.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `make ... BUILD_PREFIX=mpserver- && tests/mpserver-cata_test "mp_scene_*" --rng-seed 1 --user-dir=.data/3d-t2/`
Expected: FAIL — `mp_scene_build` is not declared.

- [ ] **Step 3: Implement the builder core**

Walk each level's rectangle once. Read `lit` from `here.access_cache( z ).visibility_cache[x][y]` and the character flags from `here.get_visibility_variables_cache()`; classify with `map::get_visibility( lit, cache )` (CLEAR → layers; DARK/HIDDEN/BLANK/MEMORIZED → memorized record; LIT/BOOMER/BOOMER_DARK → terrain only). For seen tiles, compare the stored memory ids and call `avatar::memorize_terrain` / `memorize_decoration` only on change, after `avatar::prepare_map_memory_region()` over the rect. Compress equal consecutive terrain cells (same id, flags, lit) into one run. Clamp `z_levels` to 10 and to `-OVERMAP_DEPTH`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `make ... BUILD_PREFIX=mpserver- && tests/mpserver-cata_test "mp_scene_*" --rng-seed 1 --user-dir=.data/3d-t2/`
Expected: all Task 1 and Task 2 cases pass.

- [ ] **Step 5: Commit**

Commit subject: `feat(mp): extract terrain, light and map memory into the scene`.

---

### Task 3: Extraction overlays and graffiti sanitization

**Files:**
- Modify: `src/mp/mp_scene.cpp`
- Test: `tests/mp_scene_test.cpp`

**Interfaces:**
- Consumes: `mp_scene_build` from Task 2.
- Produces: no new API; the builder fills `furniture`, `items`, `fields`, `vparts`, `traps`, `constructions`, `graffiti`, and the header `vehicles` table.

- [ ] **Step 1: Write the failing overlay tests**

Add cases to `tests/mp_scene_test.cpp`:
- `mp_scene_build_sends_top_item_only`: place three items on a tile; assert one item entry, `id_idx` of the last-placed item, `aux` = 3.
- `mp_scene_build_hides_sealed_container_contents`: place an item inside a sealed container on the floor; assert no item entry for that tile (this is `sees_some_items`, not `i_at().size()`).
- `mp_scene_build_sends_known_traps_only`: set a trap on tile A and a trap on tile B; make the avatar know A only (`avatar::add_known_trap`); assert exactly one trap entry, for A.
- `mp_scene_build_sends_vehicle_heading_and_part_angle`: spawn a vehicle, rotate it, assert one `vehicles` entry whose `heading` matches `face.dir()` in 1/65536 turn and one `vparts` entry with `part_index`, the part's `info().id`, the vehicle id and a non-zero `dir` after rotating the part.
- `mp_scene_build_sends_graffiti_text_sanitized`: set graffiti text containing a newline, a C0 control and a bidi override; assert the dictionary string has them stripped, length is capped, the text index differs from every entity id index used by layers, and the entry is valid UTF-8.
- `mp_scene_build_sends_construction_percent`: place a partial construction with `counter` at 25% and at 99%; assert `aux` 25 and 99.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `make ... BUILD_PREFIX=mpserver- && tests/mpserver-cata_test "mp_scene_*" --rng-seed 1 --user-dir=.data/3d-t3/`
Expected: FAIL — empty overlay vectors.

- [ ] **Step 3: Implement the overlays**

Inside the CLEAR branch of the tile walk, in this order: furniture, items (top item + stack size), fields, vehicle part (one per tile via `veh_at`, `part_index()`, `info().id`, `direction` as 1/256 turn; collect the vehicle heading table once per level), traps (`can_see`), constructions (`counter / 100000`), graffiti (sanitize: strip C0/C1 and bidi controls, cap at 120 bytes on a UTF-8 boundary, reject invalid sequences). Deduplicate dictionary entries by string.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `make ... BUILD_PREFIX=mpserver- && tests/mpserver-cata_test "mp_scene_*" --rng-seed 1 --user-dir=.data/3d-t3/`
Expected: all cases pass.

- [ ] **Step 5: Commit**

Commit subject: `feat(mp): add scene overlays with text-renderer gating`.

---

### Task 4: Extraction — creatures, markers, co-op slots

**Files:**
- Modify: `src/mp/mp_scene.cpp`
- Test: `tests/mp_scene_test.cpp`

**Interfaces:**
- Consumes: `mp_scene_build`.
- Produces: `creatures` entries with flags bit0 is_you, bit1 other_body (NPC or co-op slot, never the local avatar), bit2 marker_only, bit3 special_vision; no facing field.

- [ ] **Step 1: Write the failing creature tests**

Add cases:
- `mp_scene_build_omits_unseen_creatures`: monster behind a closed wall; assert no creature entry.
- `mp_scene_build_marks_creatures_one_level_away`: monster one level below with open air between; assert one entry with `z_rel == -1`, `marker_only` set, size_hint present; then block the vertical path (`valid_move` false) and assert the marker disappears.
- `mp_scene_build_sends_coop_slots_unconditionally`: second avatar in the rect on the same z; assert an entry with bit1 (`other_body`) set and bit0 clear even though `sees()` is false; assert no marker entry for a co-op slot one level away.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `make ... BUILD_PREFIX=mpserver- && tests/mpserver-cata_test "mp_scene_*" --rng-seed 1 --user-dir=.data/3d-t4/`
Expected: FAIL — no creatures.

- [ ] **Step 3: Implement the creature pass**

The avatar is emitted with `is_you` for its own z-level. Other creatures come from `game::all_creatures()` with `you.sees(here, critter)` on the same z; the enchantment special-vision path sets bit3 with the creature's size. z±1 markers require `sees()` and `valid_move(pos, pos±above/below)`. Co-op slots are a separate `get_players()` loop: same z, inside the rect, unconditional, never markers. No facing is written.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `make ... BUILD_PREFIX=mpserver- && tests/mpserver-cata_test "mp_scene_*" --rng-seed 1 --user-dir=.data/3d-t4/`
Expected: all cases pass.

- [ ] **Step 5: Commit**

Commit subject: `feat(mp): send seen creatures, z-markers and co-op slots`.

---

### Task 5: Mask bookkeeping at blit time

**Files:**
- Create: `src/mp/mp_server_mask.h`, `src/mp/mp_server_mask.cpp`
- Modify: `src/mp/mp_curses_server.cpp` (the `curses_drawwindow` blit), `src/messages.cpp` and `src/panels.cpp` (register the log window's role)
- Test: `tests/mp_server_mask_test.cpp`
- Modify: `Makefile:1123-1128`, `src/CMakeLists.txt:16-27`

**Interfaces:**
- Produces: `void mp_scene_mask_clear()`, `void mp_scene_mask_note( uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t role )`, `void mp_scene_mask_register_role( const catacurses::window &, uint8_t role )`, `std::vector<mp_scene_rect> mp_scene_mask_rects()`, `uint64_t mp_scene_mask_hash()`.
- Semantics: the grid covers only the map window's cells; the terrain blit clears it; every other blit intersecting the map window marks its clipped cells with its registered role (0 when unregistered).

- [ ] **Step 1: Write the failing mask tests**

Tests in `tests/mp_server_mask_test.cpp`, using the synthetic backend used by `mp_server_backend_test.cpp`:
- `mp_scene_mask_clears_when_terrain_blits`: note a rect, blit the terrain window, assert `mp_scene_mask_rects()` is empty.
- `mp_scene_mask_tracks_windows_over_the_map`: blit terrain, then a window over the map, assert one rect clipped to the intersection with the map window.
- `mp_scene_mask_rects_merge_and_keep_roles`: two adjacent cells with the same role merge into one rect; a text rect and a log rect stay separate and keep roles.
- `mp_scene_mask_ignores_windows_outside_the_map`: a window entirely outside the map window adds nothing.
- `mp_scene_mask_hash_changes_with_the_rect_list`: equal rect lists hash equal; adding a cell changes the hash.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `make ... BUILD_PREFIX=mpserver- && tests/mpserver-cata_test "mp_scene_mask_*" --rng-seed 1 --user-dir=.data/3d-t5/`
Expected: FAIL — missing header.

- [ ] **Step 3: Implement the bookkeeping and hook the blit**

Store a per-cell role grid sized to the terminal, clipped to the map window. In the MP backend's `curses_drawwindow`, clear on the terrain window and note every other window's rect with its registered role. Compress to rectangles deterministically (horizontal runs, then merge vertically when spans and roles match). Register the log window's role 1 where `messages.cpp` and `panels.cpp` create it; unregistered windows default to role 0.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `make ... BUILD_PREFIX=mpserver- && tests/mpserver-cata_test "mp_scene_mask_*" --rng-seed 1 --user-dir=.data/3d-t5/`
Expected: all cases pass; `mp_server_backend_test.cpp` still passes.

- [ ] **Step 5: Commit**

Commit subject: `feat(mp): track text and log masks at blit time`.

---

### Task 6: Protocol and flush

**Files:**
- Modify: `src/mp/mp_protocol.h`, `src/mp/mp_protocol.cpp` (kind 3, `caps`, `zl`), `src/mp/mp_session.h/.cpp` (scene state), `src/mp/mp_server_screen.h/.cpp` (`mp_server_flush_scene`, called at the end of `mp_server_flush_screen`), `src/mp/mp_server_input.cpp` (parse and store)
- Test: `tests/mp_scene_stream_test.cpp`, extend `tests/mp_protocol_test.cpp`

**Interfaces:**
- Consumes: `mp_scene_build` (Task 2–4), `mp_scene_mask_rects`/`mp_scene_mask_hash` (Task 5), `mp_scene_encode`/`mp_scene_body_hash` (Task 1).
- Produces: `void mp_server_flush_scene()`; `mp_session` fields `scene_caps` (u32), `scene_z_levels` (u8), `scene_seq` (u32), `scene_hash` (u64), `scene_mask_hash` (u64), `scene_sent_once` (bool), `scene_terrain_dirty` (bool); `mp_msg_kind::scene = 3`.

- [ ] **Step 1: Write the failing protocol and flush tests**

- `hello_without_caps_still_connects`: hello with proto 2 and no `caps`; assert the handshake succeeds and `scene_caps == 0`.
- `hello_caps_and_z_levels_round_trip`: `caps` 1 and `zl` 3 round-trip; `zl` 0 and 99 are clamped to 1 and 10; an unknown member is ignored.
- `mp_unwrap_accepts_scene_and_rejects_other_kinds`: kind 3 unwraps (Task 1 body), kind 99 still rejected.
- `scene_frame_follows_the_screen_frame`: two sessions; drain each transport; assert the screen frame precedes the scene frame, the scene `tiles_w/h` equal the map window, and `ui_rect` matches.
- `unchanged_scene_does_not_advance_frame_seq`: flush twice with no world change; assert exactly one scene frame and `frame_seq == 1`.
- `scene_is_not_sent_without_the_baton`: flush from the join-loop context (world baton, no active player) and from the legacy no-scheduler path; assert no scene frame.
- `scene_uses_the_requested_z_levels`: session with `zl` 2; assert `z_count == 2`.
- `scene_is_not_sent_to_a_text_only_client`: `caps` 0; assert no kind-3 frame at all.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `make ... BUILD_PREFIX=mpserver- && tests/mpserver-cata_test "mp_scene_stream_*,mp_unwrap_accepts_scene*" --rng-seed 1 --user-dir=.data/3d-t6/`
Expected: FAIL — no scene kind, no `scene_caps`.

- [ ] **Step 3: Implement the protocol and the flush**

`hello` gains `caps` and `zl` (defaults 0 and 10, clamped 1..10). Kind 3 is added to the kind enum and dispatched as a no-op on the server. `mp_server_flush_scene()` is called at the end of `mp_server_flush_screen()`: gate on an active session, `caps` bit0, the active seat matching `context_index`, and `mp_sched::holds( seat )`; fill `ui_rect` from the active context's viewport and `masks` from `mp_scene_mask_rects()`; build when `scene_terrain_dirty` is set, the mask hash changed, or `!scene_sent_once`; encode, hash, skip when equal to `scene_hash`; otherwise increment `scene_seq`, send kind 3, store hash and mask hash. Reset `scene_terrain_dirty` on send. Note: the flush also runs in the join loop and the legacy path — the gates must make those silent, and a test must prove it.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `make ... BUILD_PREFIX=mpserver- && tests/mpserver-cata_test "mp_scene_stream_*,mp_unwrap_accepts_scene*" --rng-seed 1 --user-dir=.data/3d-t6/`
Expected: all cases pass.

- [ ] **Step 5: Commit**

Commit subject: `feat(mp): stream scene frames to capable clients`.

---

### Task 7: Integration, isolation, and the `[mp]` gate

**Files:**
- Modify: `src/mp/mp_context.cpp` (or the activation function) to invalidate the visibility cache
- Test: `tests/mp_scene_stream_test.cpp`

**Interfaces:**
- Consumes: everything above.
- Produces: a scene stream that is per-seat correct and survives resize; the cache no longer assumes the previous avatar's character flags.

- [ ] **Step 1: Write the failing integration tests**

- `activating_a_player_invalidates_the_visibility_cache`: activate seat A, draw, then activate seat B; assert `visibility_cache_dirty` is true before B's draw (or that B's build recomputes with B's flags after a same-tile setup).
- `two_sessions_get_their_own_scene_rects`: two seats, different viewports; flush both; assert each scene's `ui_rect`, `tiles_w/h` and `view_dx/dy` match their own context and their own avatar cell.
- `scene_rect_survives_a_resize`: apply a `resize` control to one session, redraw, flush; assert the scene rect and `ui_rect` follow the new terminal size while the other session is unchanged.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `make ... BUILD_PREFIX=mpserver- && tests/mpserver-cata_test "activating_a_player_invalidates*,two_sessions_get_their_own_scene_rects,scene_rect_survives_a_resize" --rng-seed 1 --user-dir=.data/3d-t7/`
Expected: FAIL — cache not invalidated / rects shared.

- [ ] **Step 3: Implement the fixes**

Call `map::invalidate_visibility_cache()` on player activation (next to the existing viewport application). Derive the scene rect from the active context per flush; never cache the rect beyond the frame. If the first two tests expose more sharing (e.g. mask grid global), make the mask grid per-session (it lives with the active seat's session at flush time).

- [ ] **Step 4: Run the targeted tests, then the `[mp]` gate**

Run: `make ... BUILD_PREFIX=mpserver- && tests/mpserver-cata_test "[mp]" --rng-seed 1 --user-dir=.data/3d-t7-gate/`
Expected: the known `[mp]` baseline (136 cases) plus the new cases pass; the three known failures in `mp_server_backend_test.cpp` remain the only failures, unchanged.
Then: `make -j$(nproc) CCACHE=1 TILES=0 TESTS=1 MULTIPLAYER=1 BUILD_PREFIX=mpclient- && tests/mpclient-cata_test "[mp],[coop]" --rng-seed 1 --user-dir=.data/3d-t7-client/` → same baseline.
Then: `python3 tools/run_tests_parallel.py -o .data/3d-full --seed 1 -j $(nproc)` → diff against the known failing set; no new failures.

- [ ] **Step 5: Commit**

Commit subject: `fix(mp): keep scene state per seat and invalidate visibility on activation`.

---

## Self-review

- Spec coverage: protocol (Task 6), extraction honesty (Tasks 2–4), memory (Task 2), masks (Task 5), ten levels (Tasks 2, 6), per-seat isolation and resize (Task 7), text-client compatibility (Task 6), fixture-ready codec (Task 1). Client phases 2–5 are out of scope by design.
- Step scan: every step names the test, the assertions, the exact API, or the command with expected output; no "handle edge cases".
- Type consistency: `mp_scene_build( view_centre, tiles_w, tiles_h, z_levels )`, `mp_scene_encode`/`mp_scene_apply`/`mp_scene_body_hash`, `mp_scene_mask_*`, `scene_caps/scene_z_levels/scene_seq/scene_hash/scene_mask_hash` are the same names in every task.
- Review focus: each of the five lines has a named test in the owning task (memory round trip T2, per-seat rects T7, masks/roles T5, caps-absent T6, malformed body T1 and sanitized graffiti T3).
