#pragma once
#ifndef CATA_SRC_MP_MP_SCENE_CODEC_H
#define CATA_SRC_MP_MP_SCENE_CODEC_H

#include <cstdint>
#include <vector>

struct mp_scene;

// Wire format of one scene frame. All integers are big-endian.
//
//   u8  body_version (must be MP_SCENE_BODY_VERSION)
//   u32 frame_seq
//   u16 tiles_w, u16 tiles_h
//   i16 view_dx, view_dy, avatar_dx, avatar_dy
//   u16 ui_x, ui_y, ui_w, ui_h
//   u8  mask_count, per mask: u16 x, y, w, h; u8 role
//   u16 veh_count, per vehicle: u16 veh_id, u16 heading
//   u16 id_count, per id: u16 len, len bytes of UTF-8
//   u8  z_count (1..10), per level:
//     i8  z_rel
//     u16 run_count, per run: u16 repeat, u16 terrain_idx, u8 flags, u8 lit
//     u16 furniture_count, per entry: u16 x, y, id_idx
//     u16 item_count, per entry: u16 x, y, id_idx; u8 stack_size
//     u16 field_count, per entry: u16 x, y, id_idx; u8 intensity
//     u16 vpart_count, per entry: u16 x, y, id_idx, part_index, veh_id; i8 dir
//     u16 trap_count, per entry: u16 x, y, id_idx
//     u16 construction_count, per entry: u16 x, y, id_idx; u8 percent
//     u16 graffiti_count, per entry: u16 x, y, text_idx
//     u16 creature_count, per entry: u16 x, y; i16 z_rel; u16 id_idx;
//         u8 size_hint, flags
//
// The encoder merges neighbouring terrain runs that share terrain_idx, flags
// and lit, and guarantees that ids[0] exists. The decoder does not expand runs:
// the run vector is the model. Counts and indices are assumed to fit their wire
// widths; mp_scene_apply rejects a body that does not.

// Encodes a full frame. `scene.body_version` is written as-is and the level
// vector as-is; the builder is responsible for clamping levels to ten and for
// keeping terrain coverage within tiles_w * tiles_h.
std::vector<uint8_t> mp_scene_encode( const mp_scene &scene );

// Replaces `target` with the decoded frame. Returns false and leaves `target`
// byte-for-byte untouched when the body is malformed: truncated, longer than
// declared, an unknown version, a bad count, an out-of-range coordinate or
// dictionary index, or an invalid mask. This is the trust boundary for data
// coming off the wire from a peer.
bool mp_scene_apply( mp_scene &target, const std::vector<uint8_t> &body );

// FNV-1a over the body from offset 5 (past body_version and frame_seq) to the
// end. Two frames that differ only in those fields hash equally, which is what
// the send-side change gate compares.
uint64_t mp_scene_body_hash( const std::vector<uint8_t> &body );

#endif // CATA_SRC_MP_MP_SCENE_CODEC_H
