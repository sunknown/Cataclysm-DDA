#pragma once
#ifndef CATA_SRC_MP_MP_SCENE_H
#define CATA_SRC_MP_MP_SCENE_H

#include <cstdint>
#include <string>
#include <vector>

// Version of the scene body, the first byte on the wire. A bump may add
// compression behind a flag; the decoder currently accepts exactly this value.
constexpr uint8_t MP_SCENE_BODY_VERSION = 1;

// The protocol carries at most ten encoded z-levels.
constexpr int MP_SCENE_MAX_LEVELS = 10;

// One text-over-3D region, in absolute terminal cells. `role` 0 draws text
// glyphs over a dimmed 3D view; 1 is the native log.
struct mp_scene_rect {
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t w = 0;
    uint16_t h = 0;
    uint8_t role = 0;
};

// One vehicle in the window. Its parts refer to it by `veh_id`; the heading
// lets the client orient the whole vehicle without spinning each part.
struct mp_scene_vehicle {
    uint16_t veh_id = 0;
    uint16_t heading = 0;
};

// A cell carrying one dictionary id: furniture, a trap or graffiti.
struct mp_scene_point_id {
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t id_idx = 0;
};

// A cell carrying one dictionary id plus a byte whose meaning belongs to the
// layer: item stack size, field intensity or construction percent.
struct mp_scene_point_id_aux {
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t id_idx = 0;
    uint8_t aux = 0;
};

// One vehicle part on a tile. `dir` is the part's direction as 1/256 turn.
struct mp_scene_vpart {
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t id_idx = 0;
    uint16_t part_index = 0;
    uint16_t veh_id = 0;
    int8_t dir = 0;
};

// One creature on a level. `z_rel` is relative to the player's level; a value
// below zero marks a creature one or more levels down, and a marker_only entry
// one level above the player carries +1.
struct mp_scene_creature {
    uint16_t x = 0;
    uint16_t y = 0;
    int16_t z_rel = 0;
    uint16_t id_idx = 0;
    uint8_t size_hint = 0;
    uint8_t flags = 0;
};

// One horizontal run of terrain cells. `repeat` counts cells; `flags` is bit0
// visible and bit1 memorized; `lit` is a `lit_level` in 0..6.
struct mp_scene_terrain_run {
    uint16_t repeat = 0;
    uint16_t terrain_idx = 0;
    uint8_t flags = 0;
    uint8_t lit = 0;
};

// One encoded z-level, in wire order. `z_rel` is relative to the player's
// level, which is level 0 in the scene.
struct mp_scene_level {
    int8_t z_rel = 0;
    std::vector<mp_scene_terrain_run> terrain;
    std::vector<mp_scene_point_id> furniture;
    std::vector<mp_scene_point_id> traps;
    std::vector<mp_scene_point_id> graffiti;
    std::vector<mp_scene_point_id_aux> items;
    std::vector<mp_scene_point_id_aux> fields;
    std::vector<mp_scene_point_id_aux> constructions;
    std::vector<mp_scene_vpart> vparts;
    std::vector<mp_scene_creature> creatures;
};

// One full scene frame. Coordinates are map cells relative to the rect origin;
// `view_dx`/`view_dy` place that origin relative to the view centre, and
// `avatar_dx`/`avatar_dy` give the avatar's cell in rect coordinates (which
// may fall outside the rect while the player looks around). The `ui_*` fields
// are absolute terminal cells. `ids[0]` is always the empty string.
struct mp_scene {
    uint8_t body_version = MP_SCENE_BODY_VERSION;
    uint32_t frame_seq = 0;
    uint16_t tiles_w = 0;
    uint16_t tiles_h = 0;
    int16_t view_dx = 0;
    int16_t view_dy = 0;
    int16_t avatar_dx = 0;
    int16_t avatar_dy = 0;
    uint16_t ui_x = 0;
    uint16_t ui_y = 0;
    uint16_t ui_w = 0;
    uint16_t ui_h = 0;
    std::vector<mp_scene_rect> masks;
    std::vector<mp_scene_vehicle> vehicles;
    std::vector<std::string> ids;
    std::vector<mp_scene_level> levels;
};

#endif // CATA_SRC_MP_MP_SCENE_H
