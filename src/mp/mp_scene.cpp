#include "mp_scene.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "avatar.h"
#include "coordinates.h"
#include "enums.h"
#include "game.h"
#include "level_cache.h"
#include "lightmap.h"
#include "map.h"
#include "map_memory.h"
#include "map_scale_constants.h"
#include "trap.h"
#include "type_id.h"
#include "vehicle.h"
#include "vpart_position.h"

namespace
{

static const trap_str_id tr_unfinished_construction( "tr_unfinished_construction" );

// Dictionary built in first-seen order. Index 0 is the empty string, which
// every "nothing here" reference uses.
class scene_dictionary
{
    public:
        scene_dictionary() {
            ids.emplace_back();
        }

        uint16_t add( const std::string &id ) {
            if( id.empty() ) {
                return 0;
            }
            const auto found = index.find( id );
            if( found != index.end() ) {
                return found->second;
            }
            const uint16_t idx = static_cast<uint16_t>( ids.size() );
            index.emplace( id, idx );
            ids.push_back( id );
            return idx;
        }

        std::vector<std::string> ids;
    private:
        std::unordered_map<std::string, uint16_t> index;
};

// The one decoration map memory stores for a seen tile. The priority is the
// tile renderer's: a vehicle part hides a construction, which hides a trap,
// which hides furniture.
std::string tile_decoration_id( map &here, const Character &you, const tripoint_bub_ms &p )
{
    if( const optional_vpart_position vp = here.veh_at( p ) ) {
        const vpart_display display = vp->vehicle().get_display_of_tile( vp->mount_pos() );
        if( !display.id.is_null() ) {
            return display.get_tileset_id();
        }
    }
    if( here.partial_con_at( p ) != nullptr ) {
        return tr_unfinished_construction.str();
    }
    const trap &tr = here.tr_at( p );
    if( !tr.is_null() && tr.can_see( p, you ) ) {
        return tr.loadid.id().str();
    }
    const furn_id &furn = here.furn( p );
    if( furn ) {
        return furn.id().str();
    }
    return {};
}

void append_run( mp_scene_level &level, uint16_t terrain_idx, uint8_t flags, uint8_t lit )
{
    if( !level.terrain.empty() ) {
        mp_scene_terrain_run &back = level.terrain.back();
        if( back.terrain_idx == terrain_idx && back.flags == flags && back.lit == lit &&
            back.repeat < std::numeric_limits<uint16_t>::max() ) {
            ++back.repeat;
            return;
        }
    }
    level.terrain.push_back( mp_scene_terrain_run{ 1, terrain_idx, flags, lit } );
}

constexpr uint8_t terrain_visible = 0x1;
constexpr uint8_t terrain_memorized = 0x2;

} // namespace

mp_scene mp_scene_build( const tripoint_bub_ms &view_centre, int tiles_w, int tiles_h,
                         uint8_t z_levels )
{
    mp_scene scene;
    scene.tiles_w = static_cast<uint16_t>( std::max( 0, tiles_w ) );
    scene.tiles_h = static_cast<uint16_t>( std::max( 0, tiles_h ) );
    const int width = scene.tiles_w;
    const int height = scene.tiles_h;
    scene.view_dx = static_cast<int16_t>( -( width / 2 ) );
    scene.view_dy = static_cast<int16_t>( -( height / 2 ) );

    map &here = get_map();
    avatar &you = get_avatar_legacy();
    const tripoint_bub_ms origin =
        view_centre + tripoint_rel_ms( static_cast<int>( scene.view_dx ),
                                       static_cast<int>( scene.view_dy ), 0 );
    scene.avatar_dx = static_cast<int16_t>( you.pos_bub().x() - origin.x() );
    scene.avatar_dy = static_cast<int16_t>( you.pos_bub().y() - origin.y() );

    scene_dictionary dictionary;

    // The wire says z_rel is relative to the player's level, so the stack is
    // anchored at the avatar even when the view centre has a z offset (look
    // mode). Only x/y come from `view_centre`.
    const int base_z = you.posz();
    const int requested = std::min<int>( z_levels, MP_SCENE_MAX_LEVELS );
    for( int i = 0; i < requested; ++i ) {
        const int z = base_z - i;
        if( z < -OVERMAP_DEPTH ) {
            break;
        }

        mp_scene_level level;
        level.z_rel = static_cast<int8_t>( -i );

        // Writes below are silently dropped on an unprepared region, so prepare
        // the whole rect first.
        if( width > 0 && height > 0 ) {
            you.prepare_map_memory_region(
                here.get_abs( tripoint_bub_ms( origin.xy(), z ) ),
                here.get_abs( tripoint_bub_ms( origin.x() + width - 1,
                                               origin.y() + height - 1, z ) ) );
        }

        const level_cache &cache = here.access_cache( z );
        const visibility_variables &visibility_vars = here.get_visibility_variables_cache();

        for( int y = 0; y < height; ++y ) {
            for( int x = 0; x < width; ++x ) {
                const tripoint_bub_ms p( origin.x() + x, origin.y() + y, z );

                if( !here.inbounds( p ) ) {
                    // Nothing to draw and nothing remembered: an empty
                    // memorized cell still has to cover the rect.
                    append_run( level, 0, terrain_memorized,
                                static_cast<uint8_t>( lit_level::DARK ) );
                    continue;
                }

                const lit_level lit = cache.visibility_cache[p.x()][p.y()];
                uint16_t terrain_idx = 0;
                uint8_t flags = 0;

                switch( here.get_visibility( lit, visibility_vars ) ) {
                    case visibility_type::CLEAR: {
                        const std::string terrain = here.ter( p ).id().str();
                        terrain_idx = dictionary.add( terrain );
                        flags = terrain_visible;

                        const tripoint_abs_ms abs = here.get_abs( p );
                        if( you.get_memorized_tile( abs ).get_ter_id() != terrain ) {
                            you.memorize_terrain( abs, terrain, 0, 0 );
                        }
                        const std::string decoration = tile_decoration_id( here, you, p );
                        if( you.get_memorized_tile( abs ).get_dec_id() != decoration ) {
                            you.memorize_decoration( abs, decoration, 0, 0 );
                        }
                        break;
                    }
                    case visibility_type::DARK:
                    case visibility_type::HIDDEN: {
                        const memorized_tile &memory =
                            you.get_memorized_tile( here.get_abs( p ) );
                        terrain_idx = dictionary.add( memory.get_ter_id() );
                        flags = terrain_memorized;
                        // The wire model has no decoration block, so a
                        // memorized decoration rides the furniture vector.
                        // Task 3 writes real furniture only in the CLEAR
                        // branch, so the two never collide on a cell.
                        const uint16_t decoration_idx = dictionary.add( memory.get_dec_id() );
                        if( decoration_idx != 0 ) {
                            level.furniture.push_back( mp_scene_point_id{
                                static_cast<uint16_t>( x ), static_cast<uint16_t>( y ),
                                decoration_idx
                            } );
                        }
                        break;
                    }
                    // The text renderer draws only the `#` indicator for these
                    // and never reads the map terrain, so sending the actual
                    // terrain id would be a wallhack. Only remembered knowledge
                    // may be sent; an unseen tile carries index 0.
                    case visibility_type::LIT:
                    case visibility_type::BOOMER:
                    case visibility_type::BOOMER_DARK:
                        terrain_idx = dictionary.add(
                                         you.get_memorized_tile( here.get_abs( p ) ).get_ter_id() );
                        break;
                }

                append_run( level, terrain_idx, flags, static_cast<uint8_t>( lit ) );
            }
        }

        scene.levels.push_back( std::move( level ) );
    }

    scene.ids = std::move( dictionary.ids );
    return scene;
}
