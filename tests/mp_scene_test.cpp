#include "cata_catch.h"

#if defined(MP_SERVER)

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "avatar.h"
#include "calendar.h"
#include "catacharset.h"
#include "cata_scope_helpers.h"
#include "construction.h"
#include "coordinates.h"
#include "effect.h"
#include "enums.h"
#include "game.h"
#include "game_test_helper.h"
#include "item.h"
#include "level_cache.h"
#include "lightmap.h"
#include "map.h"
#include "map_helpers.h"
#include "map_helpers_tests.h"
#include "map_memory.h"
#include "monster.h"
#include "mp/mp_context.h"
#include "mp/mp_scene.h"
#include "player_helpers.h"
#include "trap.h"
#include "type_id.h"
#include "units.h"
#include "veh_type.h"
#include "vehicle.h"
#include "vpart_position.h"

namespace
{

static const efftype_id effect_boomered( "boomered" );
static const itype_id itype_2x4( "2x4" );
static const itype_id itype_rock( "rock" );
static const itype_id itype_stick( "stick" );
static const vproto_id vehicle_prototype_test_shopping_cart( "test_shopping_cart" );

// A world with a cleared map, one avatar and no creatures. The active context
// is restored when the test ends, as every other co-op test does.
void stand_on_a_cleared_map()
{
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    clear_creatures();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

// The rectangle the builder walks is centred on `view_centre`, so the stored
// avatar position is irrelevant to visibility: the avatar can be anywhere.
void refresh_visibility( int z )
{
    map &here = get_map();
    here.invalidate_visibility_cache();
    here.update_visibility_cache( z );
}

// The run that covers rect cell (x, y), or nullptr when the runs do not cover
// it. Coverage is terrain-raster order: y outer, x inner.
const mp_scene_terrain_run *run_at( const mp_scene &scene, const mp_scene_level &level, int x,
                                    int y )
{
    const size_t cell = static_cast<size_t>( y ) * scene.tiles_w + static_cast<size_t>( x );
    size_t first = 0;
    for( const mp_scene_terrain_run &run : level.terrain ) {
        const size_t past = first + run.repeat;
        if( cell >= first && cell < past ) {
            return &run;
        }
        first = past;
    }
    return nullptr;
}

size_t terrain_coverage( const mp_scene_level &level )
{
    size_t covered = 0;
    for( const mp_scene_terrain_run &run : level.terrain ) {
        covered += run.repeat;
    }
    return covered;
}

// Number of decoration entries on the cell, which is what a memorized record
// may carry at most one of.
size_t decorations_at( const mp_scene_level &level, int x, int y )
{
    size_t count = 0;
    for( const mp_scene_point_id &p : level.furniture ) {
        count += p.x == x && p.y == y ? 1 : 0;
    }
    for( const mp_scene_point_id &p : level.traps ) {
        count += p.x == x && p.y == y ? 1 : 0;
    }
    for( const mp_scene_point_id &p : level.graffiti ) {
        count += p.x == x && p.y == y ? 1 : 0;
    }
    return count;
}

const std::string &id_at( const mp_scene &scene, uint16_t idx )
{
    REQUIRE( idx < scene.ids.size() );
    return scene.ids[idx];
}

} // namespace

TEST_CASE( "mp_scene_build_emits_visible_terrain_runs", "[mp]" )
{
    restore_active_state restore;
    stand_on_a_cleared_map();
    map &here = get_map();
    avatar &you = get_avatar_legacy();
    set_time_to_day();
    refresh_visibility( you.posz() );

    const tripoint_bub_ms centre = you.pos_bub();
    const mp_scene scene = mp_scene_build( centre, 21, 21, 1 );

    REQUIRE( scene.levels.size() == 1 );
    const mp_scene_level &level = scene.levels[0];
    CHECK( level.z_rel == 0 );
    CHECK( scene.tiles_w == 21 );
    CHECK( scene.tiles_h == 21 );

    // index 0 is the empty string by construction.
    REQUIRE( !scene.ids.empty() );
    CHECK( scene.ids[0].empty() );

    // Every cell is covered exactly once, and every one of them is a visible
    // (bit0) terrain run: daylight over open pavement.
    CHECK( terrain_coverage( level ) == scene.tiles_w * scene.tiles_h );
    for( size_t i = 0; i < level.terrain.size(); ++i ) {
        const mp_scene_terrain_run &run = level.terrain[i];
        CHECK( ( run.flags & 0x1 ) == 0x1 );
        CHECK( ( run.flags & 0x2 ) == 0x0 );
        REQUIRE( run.terrain_idx < scene.ids.size() );
        CHECK( id_at( scene, run.terrain_idx ) == "t_pavement" );
        // The model is compressed before the encoder ever sees it.
        if( i > 0 ) {
            const mp_scene_terrain_run &prev = level.terrain[i - 1];
            CHECK_FALSE( ( prev.terrain_idx == run.terrain_idx && prev.flags == run.flags &&
                           prev.lit == run.lit ) );
        }
    }

    // The rect origin and the avatar cell follow the pinned geometry.
    CHECK( scene.view_dx == -10 );
    CHECK( scene.view_dy == -10 );
    CHECK( scene.avatar_dx == 10 );
    CHECK( scene.avatar_dy == 10 );
    CHECK( scene.ui_w == 0 );
    CHECK( scene.frame_seq == 0 );
    CHECK( scene.vehicles.empty() );
}

TEST_CASE( "mp_scene_build_marks_dark_tiles_as_memorized", "[mp]" )
{
    restore_active_state restore;
    stand_on_a_cleared_map();
    map &here = get_map();
    avatar &you = get_avatar_legacy();
    set_time_to_day();

    // The target carries a decoration so that "terrain + one decoration" is
    // observable rather than an empty string.
    const tripoint_bub_ms target( 20, 20, 0 );
    here.ter_set( target, ter_id( "t_floor" ) );
    here.furn_set( target, furn_id( "f_chair" ) );
    here.invalidate_map_cache( 0 );
    here.build_map_cache( 0, true );
    you.clear_map_memory();

    // Seen from four tiles away, inside a 21x21 rect centred on the target.
    you.setpos( here, tripoint_bub_ms( 20, 24, 0 ), false );
    refresh_visibility( 0 );
    mp_scene_build( target, 21, 21, 1 );
    const tripoint_abs_ms target_abs = here.get_abs( target );
    REQUIRE( you.has_memory_at( target_abs ) );

    // Far away the target is dark: the record must come from map memory.
    you.setpos( here, tripoint_bub_ms( 120, 120, 0 ), false );
    refresh_visibility( 0 );
    const mp_scene scene = mp_scene_build( target, 21, 21, 1 );

    REQUIRE( scene.levels.size() == 1 );
    const mp_scene_level &level = scene.levels[0];
    const mp_scene_terrain_run *run = run_at( scene, level, 10, 10 );
    REQUIRE( run != nullptr );
    // Memorized (bit1), not visible (bit0).
    CHECK( ( run->flags & 0x1 ) == 0 );
    CHECK( ( run->flags & 0x2 ) == 0x2 );
    CHECK( id_at( scene, run->terrain_idx ) == "t_floor" );
    CHECK( decorations_at( level, 10, 10 ) == 1 );
}

TEST_CASE( "mp_scene_build_fills_map_memory_ids", "[mp]" )
{
    restore_active_state restore;
    stand_on_a_cleared_map();
    map &here = get_map();
    avatar &you = get_avatar_legacy();
    set_time_to_day();
    refresh_visibility( 0 );

    // No prepare_map_memory_region() call here: the builder owns preparing the
    // rect it is about to write into.
    you.clear_map_memory();
    const tripoint_bub_ms target = you.pos_bub() + tripoint_rel_ms( 2, 0, 0 );
    const tripoint_abs_ms target_abs = here.get_abs( target );

    mp_scene_build( target, 11, 11, 1 );

    CHECK( you.has_memory_at( target_abs ) );
    CHECK( you.get_memorized_tile( target_abs ).get_ter_id() == here.ter( target ).id().str() );
}

TEST_CASE( "mp_scene_build_covers_ten_z_levels", "[mp]" )
{
    restore_active_state restore;
    stand_on_a_cleared_map();
    avatar &you = get_avatar_legacy();

    const mp_scene ten = mp_scene_build( you.pos_bub(), 7, 7, 10 );
    REQUIRE( ten.levels.size() == 10 );
    for( size_t i = 0; i < ten.levels.size(); ++i ) {
        CHECK( ten.levels[i].z_rel == -static_cast<int8_t>( i ) );
    }

    const mp_scene clamped = mp_scene_build( you.pos_bub(), 7, 7, 11 );
    CHECK( clamped.levels.size() == 10 );
}

TEST_CASE( "mp_scene_build_anchors_levels_to_the_players_level", "[mp]" )
{
    restore_active_state restore;
    stand_on_a_cleared_map();
    map &here = get_map();
    avatar &you = get_avatar_legacy();
    restore_on_out_of_scope restore_view_offset( you.view_offset );

    // The view centre can carry a z offset from look mode. Level 0 must still
    // be the avatar's own level, so the two z-levels get distinct terrain and
    // both are pinned visible: a regression to view_centre.z() would send the
    // wall instead of the floor.
    const tripoint_bub_ms own = you.pos_bub();
    you.view_offset = tripoint_rel_ms( 0, 0, 1 );
    level_cache &own_cache = here.access_cache( own.z() );
    level_cache &above_cache = here.access_cache( own.z() + 1 );
    for( int dy = -3; dy <= 3; ++dy ) {
        for( int dx = -3; dx <= 3; ++dx ) {
            here.ter_set( tripoint_bub_ms( own.x() + dx, own.y() + dy, own.z() ),
                          ter_id( "t_floor" ) );
            here.ter_set( tripoint_bub_ms( own.x() + dx, own.y() + dy, own.z() + 1 ),
                          ter_id( "t_wall" ) );
            own_cache.visibility_cache[own.x() + dx][own.y() + dy] = lit_level::LOW;
            above_cache.visibility_cache[own.x() + dx][own.y() + dy] = lit_level::LOW;
        }
    }

    const mp_scene scene = mp_scene_build( own + you.view_offset, 7, 7, 2 );
    REQUIRE( scene.levels.size() == 2 );
    CHECK( scene.levels[0].z_rel == 0 );
    CHECK( scene.levels[1].z_rel == -1 );
    CHECK( terrain_coverage( scene.levels[0] ) == 49 );
    for( const mp_scene_terrain_run &run : scene.levels[0].terrain ) {
        CHECK( id_at( scene, run.terrain_idx ) == "t_floor" );
    }
}

TEST_CASE( "mp_scene_build_omits_layers_for_vision_effect_tiles", "[mp]" )
{
    restore_active_state restore;
    stand_on_a_cleared_map();
    map &here = get_map();
    avatar &you = get_avatar_legacy();
    you.clear_map_memory();

    // First remember a floor tile; then replace it with a wall in the map. The
    // scene must keep sending the remembered floor, never the wall the player
    // cannot see. Night plus the boomer effect puts the tile in the
    // BOOMER_DARK class, which must carry terrain only.
    const tripoint_bub_ms centre = you.pos_bub();
    const tripoint_bub_ms target = centre + tripoint_rel_ms( 2, 0, 0 );
    here.ter_set( target, ter_id( "t_floor" ) );
    set_time_to_day();
    refresh_visibility( 0 );
    mp_scene_build( target, 21, 21, 1 );
    REQUIRE( you.has_memory_at( here.get_abs( target ) ) );

    here.ter_set( target, ter_id( "t_wall" ) );
    set_time( calendar::turn_zero );
    you.add_effect( effect_boomered, 1_hours );
    on_out_of_scope remove_boomered( [&]() {
        you.remove_effect( effect_boomered );
    } );
    refresh_visibility( 0 );

    auto &vis_cache = here.access_cache( 0 ).visibility_cache;
    const visibility_variables &cache = here.get_visibility_variables_cache();
    // Pin the target's class: ambient light next to the avatar varies with the
    // test world, but extraction must treat a DARK tile while boomered as
    // BOOMER_DARK regardless of how dark it actually got.
    vis_cache[target.x()][target.y()] = lit_level::DARK;

    const mp_scene scene = mp_scene_build( centre, 21, 21, 1 );
    REQUIRE( scene.levels.size() == 1 );
    const mp_scene_level &level = scene.levels[0];

    size_t boomer_tiles = 0;
    for( int y = 0; y < scene.tiles_h; ++y ) {
        for( int x = 0; x < scene.tiles_w; ++x ) {
            const tripoint_bub_ms p( centre.x() + scene.view_dx + x,
                                     centre.y() + scene.view_dy + y, centre.z() );
            const visibility_type vis =
                here.get_visibility( vis_cache[p.x()][p.y()], cache );
            if( vis != visibility_type::BOOMER && vis != visibility_type::BOOMER_DARK ) {
                continue;
            }
            ++boomer_tiles;
            const mp_scene_terrain_run *run = run_at( scene, level, x, y );
            REQUIRE( run != nullptr );
            // No flag at all: neither visible nor memorized.
            CHECK( run->flags == 0 );
        }
    }
    CHECK( boomer_tiles > 0 );

    // The target is the wallhack case: the real terrain is a wall the player
    // has never seen, so only the remembered floor may be sent.
    const int target_x = target.x() - ( centre.x() + scene.view_dx );
    const int target_y = target.y() - ( centre.y() + scene.view_dy );
    const visibility_type target_vis =
        here.get_visibility( vis_cache[target.x()][target.y()], cache );
    CHECK( ( target_vis == visibility_type::BOOMER ||
             target_vis == visibility_type::BOOMER_DARK ) );
    const mp_scene_terrain_run *target_run = run_at( scene, level, target_x, target_y );
    REQUIRE( target_run != nullptr );
    CHECK( target_run->flags == 0 );
    CHECK( id_at( scene, target_run->terrain_idx ) == "t_floor" );

    // The vision-effect classes carry no overlay entry in any layer. The only
    // creature is the avatar's own body: it is always on its own level.
    CHECK( level.furniture.empty() );
    CHECK( level.traps.empty() );
    CHECK( level.graffiti.empty() );
    CHECK( level.items.empty() );
    CHECK( level.fields.empty() );
    CHECK( level.constructions.empty() );
    CHECK( level.vparts.empty() );
    REQUIRE( level.creatures.size() == 1 );
    CHECK( level.creatures[0].flags == 0x1 );
    CHECK( scene.vehicles.empty() );
}

TEST_CASE( "mp_scene_build_sends_top_item_only", "[mp]" )
{
    restore_active_state restore;
    stand_on_a_cleared_map();
    map &here = get_map();
    avatar &you = get_avatar_legacy();
    set_time_to_day();

    const tripoint_bub_ms target = you.pos_bub();
    here.add_item( target, item( itype_rock ) );
    here.add_item( target, item( itype_stick ) );
    here.add_item( target, item( itype_2x4 ) );
    refresh_visibility( you.posz() );

    const mp_scene scene = mp_scene_build( target, 21, 21, 1 );
    REQUIRE( scene.levels.size() == 1 );
    const mp_scene_level &level = scene.levels[0];

    // One entry for the tile: the uppermost (last-placed) item and the stack
    // size, never one entry per item.
    REQUIRE( level.items.size() == 1 );
    CHECK( level.items[0].x == 10 );
    CHECK( level.items[0].y == 10 );
    CHECK( id_at( scene, level.items[0].id_idx ) == "2x4" );
    CHECK( level.items[0].aux == 3 );
}

TEST_CASE( "mp_scene_build_hides_sealed_container_contents", "[mp]" )
{
    restore_active_state restore;
    stand_on_a_cleared_map();
    map &here = get_map();
    avatar &you = get_avatar_legacy();
    set_time_to_day();

    // A sealed container is terrain/furniture the text renderer refuses to
    // draw items over, even though the map stack holds one.
    const tripoint_bub_ms target = you.pos_bub() + tripoint_rel_ms( 1, 0, 0 );
    here.furn_set( target, furn_id( "f_machinery_heavy" ) );
    here.add_item( target, item( itype_rock ) );
    REQUIRE( here.has_items( target ) );
    refresh_visibility( you.posz() );

    const mp_scene scene = mp_scene_build( target, 21, 21, 1 );
    REQUIRE( scene.levels.size() == 1 );
    CHECK( scene.levels[0].items.empty() );
}

TEST_CASE( "mp_scene_build_sends_known_traps_only", "[mp]" )
{
    restore_active_state restore;
    stand_on_a_cleared_map();
    map &here = get_map();
    avatar &you = get_avatar_legacy();
    set_time_to_day();

    const trap_id glass( "tr_glass" );
    const tripoint_bub_ms known = you.pos_bub() + tripoint_rel_ms( 1, 0, 0 );
    const tripoint_bub_ms unknown = you.pos_bub() + tripoint_rel_ms( 2, 0, 0 );
    here.trap_set( known, glass );
    here.trap_set( unknown, glass );
    you.add_known_trap( known, glass.obj() );
    refresh_visibility( you.posz() );

    const mp_scene scene = mp_scene_build( you.pos_bub(), 21, 21, 1 );
    REQUIRE( scene.levels.size() == 1 );
    const mp_scene_level &level = scene.levels[0];

    REQUIRE( level.traps.size() == 1 );
    CHECK( level.traps[0].x == 11 );
    CHECK( level.traps[0].y == 10 );
    CHECK( id_at( scene, level.traps[0].id_idx ) == "tr_glass" );
}

TEST_CASE( "mp_scene_build_sends_vehicle_heading_and_part_angle", "[mp]" )
{
    restore_active_state restore;
    stand_on_a_cleared_map();
    map &here = get_map();
    avatar &you = get_avatar_legacy();
    set_time_to_day();

    const tripoint_bub_ms first_pos = you.pos_bub() + tripoint_rel_ms( 2, 0, 0 );
    const tripoint_bub_ms second_pos = you.pos_bub() + tripoint_rel_ms( 5, 0, 0 );
    vehicle *first = here.add_vehicle( vehicle_prototype_test_shopping_cart, first_pos,
                                       45_degrees, 0, veh_spawn_status::UNDAMAGED );
    vehicle *second = here.add_vehicle( vehicle_prototype_test_shopping_cart, second_pos,
                                        180_degrees, 0, veh_spawn_status::UNDAMAGED );
    REQUIRE( first != nullptr );
    REQUIRE( second != nullptr );

    const optional_vpart_position first_vp = here.veh_at( first_pos );
    const optional_vpart_position second_vp = here.veh_at( second_pos );
    REQUIRE( first_vp.has_value() );
    REQUIRE( second_vp.has_value() );
    const int first_part = static_cast<int>( first_vp->part_index() );
    const int second_part = static_cast<int>( second_vp->part_index() );
    first->part( first_part ).direction = 90_degrees;
    second->part( second_part ).direction = 90_degrees;
    refresh_visibility( you.posz() );

    const mp_scene scene = mp_scene_build( first_pos, 21, 21, 1 );
    REQUIRE( scene.levels.size() == 1 );

    const auto heading_of = []( const vehicle & veh ) -> uint16_t {
        return static_cast<uint16_t>(
                   std::lround( units::to_degrees( veh.face.dir() ) * 65536.0 / 360.0 ) & 0xFFFF );
    };
    const uint16_t first_heading = heading_of( *first );
    const uint16_t second_heading = heading_of( *second );
    REQUIRE( first_heading != second_heading );

    // Both vehicles carry om_id 0 in the test world, so the wire ids must be
    // frame-local and distinct.
    REQUIRE( scene.vehicles.size() == 2 );
    uint16_t first_id = 0xFFFF;
    uint16_t second_id = 0xFFFF;
    for( const mp_scene_vehicle &entry : scene.vehicles ) {
        if( entry.heading == first_heading ) {
            first_id = entry.veh_id;
        } else if( entry.heading == second_heading ) {
            second_id = entry.veh_id;
        }
    }
    CHECK( first_id != 0xFFFF );
    CHECK( second_id != 0xFFFF );
    CHECK( first_id != second_id );

    const auto wire_id_of = [&]( const vehicle & veh ) -> uint16_t {
        for( const mp_scene_vehicle &entry : scene.vehicles ) {
            if( entry.heading == heading_of( veh ) ) {
                return entry.veh_id;
            }
        }
        return 0xFFFF;
    };

    const std::vector<mp_scene_vpart> &vparts = scene.levels[0].vparts;
    const tripoint_bub_ms rect_origin =
        first_pos + tripoint_rel_ms( static_cast<int>( scene.view_dx ),
                                     static_cast<int>( scene.view_dy ), 0 );
    const auto part_at = [&]( const tripoint_bub_ms & tile,
    int part_index ) -> const mp_scene_vpart * {
        for( const mp_scene_vpart &entry : vparts ) {
            const tripoint_bub_ms p( rect_origin.x() + entry.x, rect_origin.y() + entry.y,
                                     rect_origin.z() );
            if( p == tile && entry.part_index == part_index ) {
                return &entry;
            }
        }
        return nullptr;
    };

    const mp_scene_vpart *first_entry = part_at( first_pos, first_part );
    REQUIRE( first_entry != nullptr );
    CHECK( first_entry->veh_id == first_id );
    CHECK( first_entry->veh_id == wire_id_of( *first ) );
    CHECK( id_at( scene, first_entry->id_idx ) == first->part( first_part ).info().id.str() );
    // 90 degrees is 64/256 of a turn; the spawn angle must not be sent.
    CHECK( first_entry->dir == 64 );

    const mp_scene_vpart *second_entry = part_at( second_pos, second_part );
    REQUIRE( second_entry != nullptr );
    CHECK( second_entry->veh_id == second_id );
    CHECK( second_entry->veh_id == wire_id_of( *second ) );
    CHECK( id_at( scene, second_entry->id_idx ) == second->part( second_part ).info().id.str() );
    CHECK( second_entry->dir == 64 );

    // Every part entry names the vehicle actually occupying that tile.
    for( const mp_scene_vpart &entry : vparts ) {
        const tripoint_bub_ms tile( rect_origin.x() + entry.x, rect_origin.y() + entry.y,
                                    rect_origin.z() );
        const optional_vpart_position tile_vp = here.veh_at( tile );
        REQUIRE( tile_vp.has_value() );
        CHECK( entry.veh_id == wire_id_of( tile_vp->vehicle() ) );
    }
}

TEST_CASE( "mp_scene_build_sends_graffiti_text_sanitized", "[mp]" )
{
    restore_active_state restore;
    stand_on_a_cleared_map();
    map &here = get_map();
    avatar &you = get_avatar_legacy();
    set_time_to_day();

    const tripoint_bub_ms base = you.pos_bub();

    // One tile per sanitizer rule, so a regression in any rule is visible.
    std::string raw = "hello\n\x01world";
    raw += "\xE2\x80\xAE"; // U+202E RIGHT-TO-LEFT OVERRIDE
    raw += "\xE2\x81\xA6"; // U+2066 LEFT-TO-RIGHT ISOLATE
    raw += "\xC2\x85";     // U+0085 NEXT LINE (C1)
    raw += "!";
    raw += std::string( 200, 'x' );
    here.set_graffiti( base + tripoint_rel_ms( 1, 0, 0 ), raw );

    // 119 bytes plus a 4-byte code point: a byte slice at 120 would keep one
    // invalid byte, while a boundary-safe cap drops the whole code point.
    const std::string boundary = std::string( 119, 'a' ) + "\xF0\x9F\x98\x80" + "tail";
    here.set_graffiti( base + tripoint_rel_ms( 2, 0, 0 ), boundary );

    // Exactly at the cap, with trailing bytes that must not fit.
    here.set_graffiti( base + tripoint_rel_ms( 3, 0, 0 ),
                       std::string( 120, 'a' ) + "zzz" );

    // Invalid UTF-8: a lone continuation byte and a truncated sequence.
    here.set_graffiti( base + tripoint_rel_ms( 4, 0, 0 ), "ok" + std::string( "\x80" ) );
    here.set_graffiti( base + tripoint_rel_ms( 5, 0, 0 ), "ok" + std::string( "\xC3" ) );

    // Sanitizes to nothing: the tile must produce no entry at all.
    here.set_graffiti( base + tripoint_rel_ms( 6, 0, 0 ), "\n\x01" );

    refresh_visibility( you.posz() );

    const mp_scene scene = mp_scene_build( base, 21, 21, 1 );
    REQUIRE( scene.levels.size() == 1 );
    const mp_scene_level &level = scene.levels[0];

    const auto text_at = [&]( int x, int y ) -> const std::string * {
        for( const mp_scene_point_id &entry : level.graffiti ) {
            if( entry.x == x && entry.y == y ) {
                return &id_at( scene, entry.id_idx );
            }
        }
        return nullptr;
    };

    // Five entries: the empty-after-sanitize tile contributes none.
    REQUIRE( level.graffiti.size() == 5 );

    const std::string *mixed = text_at( 11, 10 );
    REQUIRE( mixed != nullptr );
    CHECK( mixed->rfind( "helloworld!", 0 ) == 0 );
    CHECK( mixed->find( '\n' ) == std::string::npos );
    CHECK( mixed->find( '\x01' ) == std::string::npos );
    CHECK( mixed->find( "\xE2\x80\xAE" ) == std::string::npos );
    CHECK( mixed->find( "\xE2\x81\xA6" ) == std::string::npos );
    CHECK( mixed->find( "\xC2\x85" ) == std::string::npos );
    CHECK( mixed->size() == 120 );
    CHECK( utf32_to_utf8( utf8_to_utf32( *mixed ) ) == *mixed );

    const std::string *truncated = text_at( 12, 10 );
    REQUIRE( truncated != nullptr );
    CHECK( *truncated == std::string( 119, 'a' ) );
    CHECK( truncated->size() == 119 );
    CHECK( truncated->find( "\xF0\x9F\x98\x80" ) == std::string::npos );
    CHECK( utf32_to_utf8( utf8_to_utf32( *truncated ) ) == *truncated );

    const std::string *exact_cap = text_at( 13, 10 );
    REQUIRE( exact_cap != nullptr );
    CHECK( *exact_cap == std::string( 120, 'a' ) );
    CHECK( exact_cap->size() == 120 );

    const std::string *lone_continuation = text_at( 14, 10 );
    REQUIRE( lone_continuation != nullptr );
    CHECK( *lone_continuation == "ok" );

    const std::string *truncated_sequence = text_at( 15, 10 );
    REQUIRE( truncated_sequence != nullptr );
    CHECK( *truncated_sequence == "ok" );

    CHECK( text_at( 16, 10 ) == nullptr );

    // Graffiti text is opaque: it must never share an index with an entity id
    // the layers reference.
    const uint16_t text_idx = level.graffiti.front().id_idx;
    REQUIRE( text_idx != 0 );
    std::vector<uint16_t> entity_ids;
    for( const mp_scene_terrain_run &run : level.terrain ) {
        entity_ids.push_back( run.terrain_idx );
    }
    for( const mp_scene_point_id &p : level.furniture ) {
        entity_ids.push_back( p.id_idx );
    }
    for( const mp_scene_point_id &p : level.traps ) {
        entity_ids.push_back( p.id_idx );
    }
    for( const mp_scene_point_id_aux &p : level.items ) {
        entity_ids.push_back( p.id_idx );
    }
    for( const mp_scene_point_id_aux &p : level.fields ) {
        entity_ids.push_back( p.id_idx );
    }
    for( const mp_scene_point_id_aux &p : level.constructions ) {
        entity_ids.push_back( p.id_idx );
    }
    for( const mp_scene_vpart &p : level.vparts ) {
        entity_ids.push_back( p.id_idx );
    }
    CHECK( std::find( entity_ids.begin(), entity_ids.end(), text_idx ) == entity_ids.end() );
}

TEST_CASE( "mp_scene_build_sends_construction_percent", "[mp]" )
{
    restore_active_state restore;
    stand_on_a_cleared_map();
    map &here = get_map();
    avatar &you = get_avatar_legacy();
    set_time_to_day();

    const construction_id build_id = construction_str_id( "constr_pit" ).id();
    const tripoint_bub_ms quarter = you.pos_bub() + tripoint_rel_ms( 1, 0, 0 );
    const tripoint_bub_ms almost = you.pos_bub() + tripoint_rel_ms( 2, 0, 0 );
    const tripoint_bub_ms malformed = you.pos_bub() + tripoint_rel_ms( 3, 0, 0 );
    partial_con pc_quarter;
    pc_quarter.id = build_id;
    pc_quarter.counter = 2500000;
    here.partial_con_set( quarter, pc_quarter );
    partial_con pc_almost;
    pc_almost.id = build_id;
    pc_almost.counter = 9900000;
    here.partial_con_set( almost, pc_almost );
    // The default construction_id(-1) is invalid; obj() must never be called
    // on it, so this tile contributes no entry.
    partial_con pc_malformed;
    pc_malformed.counter = 500000;
    here.partial_con_set( malformed, pc_malformed );
    refresh_visibility( you.posz() );

    const mp_scene scene = mp_scene_build( you.pos_bub(), 21, 21, 1 );
    REQUIRE( scene.levels.size() == 1 );
    const mp_scene_level &level = scene.levels[0];
    REQUIRE( level.constructions.size() == 2 );

    bool saw_quarter = false;
    bool saw_almost = false;
    for( const mp_scene_point_id_aux &entry : level.constructions ) {
        CHECK( id_at( scene, entry.id_idx ) == "constr_pit" );
        if( entry.x == 11 && entry.y == 10 ) {
            saw_quarter = true;
            CHECK( entry.aux == 25 );
        } else if( entry.x == 12 && entry.y == 10 ) {
            saw_almost = true;
            CHECK( entry.aux == 99 );
        }
        CHECK_FALSE( ( entry.x == 13 && entry.y == 10 ) );
    }
    CHECK( saw_quarter );
    CHECK( saw_almost );
}

TEST_CASE( "mp_scene_build_omits_unseen_creatures", "[mp]" )
{
    restore_active_state restore;
    stand_on_a_cleared_map();
    map &here = get_map();
    avatar &you = get_avatar_legacy();
    set_time_to_day();

    // A wall between the avatar and the monster keeps the monster's tile out
    // of sight, so it must not appear even though it is inside the rect.
    const tripoint_bub_ms base = you.pos_bub();
    here.ter_set( base + tripoint_rel_ms( 1, 0, 0 ), ter_id( "t_wall" ) );
    // Rebuild so no earlier test's cached sight line survives the new wall.
    here.build_map_cache( base.z() );
    spawn_test_monster( "mon_zombie", base + tripoint_rel_ms( 2, 0, 0 ) );
    refresh_visibility( you.posz() );

    const mp_scene scene = mp_scene_build( base, 21, 21, 1 );
    REQUIRE( scene.levels.size() == 1 );
    const mp_scene_level &level = scene.levels[0];

    // The avatar itself is always a body; nobody else is.
    REQUIRE( level.creatures.size() == 1 );
    const mp_scene_creature &avatar_body = level.creatures[0];
    CHECK( ( avatar_body.flags & 0x1 ) == 0x1 );
    CHECK( ( avatar_body.flags & 0x4 ) == 0 );
    CHECK( avatar_body.x == 10 );
    CHECK( avatar_body.y == 10 );
    CHECK( avatar_body.z_rel == 0 );
    CHECK( id_at( scene, avatar_body.id_idx ) == "avatar" );
    CHECK( avatar_body.size_hint == static_cast<uint8_t>( you.get_size() ) );
}

TEST_CASE( "mp_scene_build_marks_creatures_one_level_away", "[mp]" )
{
    restore_active_state restore;
    stand_on_a_cleared_map();
    map &here = get_map();
    avatar &you = get_avatar_legacy();
    set_time_to_day();

    // The player stands over open air; the monster below stands on a floor.
    // That is the vertical drop the sight and move checks accept, so the
    // monster arrives as a marker in the player's own level.
    const tripoint_bub_ms base = you.pos_bub();
    const tripoint_bub_ms below( base.x(), base.y(), base.z() - 1 );
    here.ter_set( base, ter_id( "t_open_air" ) );
    here.ter_set( below, ter_id( "t_pavement" ) );
    here.build_map_cache( base.z() );
    here.build_map_cache( base.z() - 1 );
    const monster &mon = spawn_test_monster( "mon_zombie", below );
    refresh_visibility( base.z() );
    refresh_visibility( base.z() - 1 );

    const mp_scene scene = mp_scene_build( base, 21, 21, 2 );
    REQUIRE( scene.levels.size() == 2 );

    const mp_scene_creature *marker = nullptr;
    for( const mp_scene_creature &entry : scene.levels[0].creatures ) {
        if( ( entry.flags & 0x4 ) != 0 ) {
            REQUIRE( marker == nullptr );
            marker = &entry;
        }
    }
    REQUIRE( marker != nullptr );
    CHECK( marker->x == 10 );
    CHECK( marker->y == 10 );
    CHECK( marker->z_rel == -1 );
    CHECK( ( marker->flags & 0x2 ) == 0 );
    CHECK( ( marker->flags & 0x1 ) == 0 );
    CHECK( id_at( scene, marker->id_idx ) == "mon_zombie" );
    CHECK( marker->size_hint == static_cast<uint8_t>( mon.get_size() ) );

    // A floor over the drop blocks both the sight line and the vertical move,
    // so the marker must disappear.
    here.ter_set( base, ter_id( "t_floor" ) );
    here.build_map_cache( base.z() );
    refresh_visibility( base.z() );
    refresh_visibility( base.z() - 1 );

    const mp_scene blocked = mp_scene_build( base, 21, 21, 2 );
    REQUIRE( blocked.levels.size() == 2 );
    for( const mp_scene_creature &entry : blocked.levels[0].creatures ) {
        CHECK( ( entry.flags & 0x4 ) == 0 );
    }
}

TEST_CASE( "mp_scene_build_sends_coop_slots_unconditionally", "[mp]" )
{
    restore_active_state restore;
    stand_on_a_cleared_map();
    map &here = get_map();
    avatar &you = get_avatar_legacy();
    set_time_to_day();

    on_out_of_scope restore_slots( []() {
        game_test_helper::reset_player_slots();
        mp_sync_context_count();
    } );

    const tripoint_bub_ms base = you.pos_bub();
    avatar &partner = g->add_player_slot();
    mp_sync_context_count();
    // Production slot creation fills the body and id in; a bare slot is not
    // representative.
    partner.set_body();
    partner.setID( g->assign_npc_id() );
    const tripoint_bub_ms partner_pos = base + tripoint_rel_ms( 8, 0, 0 );
    partner.setpos( here, partner_pos, false );

    // The partner is not visible through the wall, yet a co-op slot is sent
    // unconditionally.
    here.ter_set( base + tripoint_rel_ms( 1, 0, 0 ), ter_id( "t_wall" ) );
    here.build_map_cache( base.z() );
    refresh_visibility( you.posz() );
    REQUIRE_FALSE( you.sees( here, partner ) );

    const mp_scene scene = mp_scene_build( base, 21, 21, 2 );
    REQUIRE( scene.levels.size() == 2 );

    const mp_scene_creature *slot = nullptr;
    for( const mp_scene_creature &entry : scene.levels[0].creatures ) {
        if( entry.x == 18 && entry.y == 10 && entry.z_rel == 0 ) {
            REQUIRE( slot == nullptr );
            slot = &entry;
        }
    }
    REQUIRE( slot != nullptr );
    CHECK( ( slot->flags & 0x2 ) == 0x2 );
    CHECK( ( slot->flags & 0x1 ) == 0 );
    CHECK( ( slot->flags & 0x4 ) == 0 );
    CHECK( id_at( scene, slot->id_idx ) == "avatar" );
    CHECK( slot->size_hint == static_cast<uint8_t>( partner.get_size() ) );

    // One level away the same slot is not sent at all: co-op slots never
    // become markers, and the avatar's own level has no body for it.
    partner.setpos( here, tripoint_bub_ms( partner_pos.x(), partner_pos.y(), base.z() - 1 ),
                    false );
    refresh_visibility( base.z() - 1 );
    const mp_scene below = mp_scene_build( base, 21, 21, 2 );
    REQUIRE( below.levels.size() == 2 );
    REQUIRE( below.levels[0].creatures.size() == 1 );
    for( const mp_scene_creature &entry : below.levels[0].creatures ) {
        CHECK( entry.z_rel == 0 );
        CHECK( ( entry.flags & 0x1 ) == 0x1 );
    }
}

#endif // MP_SERVER
