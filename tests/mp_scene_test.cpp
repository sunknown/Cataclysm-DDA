#include "cata_catch.h"

#if defined(MP_SERVER)

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "avatar.h"
#include "calendar.h"
#include "cata_scope_helpers.h"
#include "coordinates.h"
#include "effect.h"
#include "enums.h"
#include "game.h"
#include "game_test_helper.h"
#include "map.h"
#include "map_helpers.h"
#include "map_helpers_tests.h"
#include "map_memory.h"
#include "mp/mp_context.h"
#include "mp/mp_scene.h"
#include "player_helpers.h"
#include "type_id.h"

namespace
{

static const efftype_id effect_boomered( "boomered" );

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

TEST_CASE( "mp_scene_build_omits_layers_for_vision_effect_tiles", "[mp]" )
{
    restore_active_state restore;
    stand_on_a_cleared_map();
    map &here = get_map();
    avatar &you = get_avatar_legacy();
    // Night plus the boomer effect turns the whole outdoors into BOOMER_DARK,
    // the class that must carry terrain only.
    set_time( calendar::turn_zero );
    you.add_effect( effect_boomered, 1_hours );
    on_out_of_scope remove_boomered( [&]() {
        you.remove_effect( effect_boomered );
    } );
    refresh_visibility( 0 );

    const tripoint_bub_ms centre = you.pos_bub();
    const mp_scene scene = mp_scene_build( centre, 21, 21, 1 );
    REQUIRE( scene.levels.size() == 1 );
    const mp_scene_level &level = scene.levels[0];
    const visibility_variables &cache = here.get_visibility_variables_cache();
    const auto &vis_cache = here.access_cache( 0 ).visibility_cache;

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

    // The vision-effect classes carry no overlay entry in any layer.
    CHECK( level.furniture.empty() );
    CHECK( level.traps.empty() );
    CHECK( level.graffiti.empty() );
    CHECK( level.items.empty() );
    CHECK( level.fields.empty() );
    CHECK( level.constructions.empty() );
    CHECK( level.vparts.empty() );
    CHECK( level.creatures.empty() );
    CHECK( scene.vehicles.empty() );
}

#endif // MP_SERVER
