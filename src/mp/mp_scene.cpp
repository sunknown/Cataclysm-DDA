#include "mp_scene.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "avatar.h"
#include "catacharset.h"
#include "construction.h"
#include "coordinates.h"
#include "creature.h"
#include "enums.h"
#include "field.h"
#include "game.h"
#include "level_cache.h"
#include "lightmap.h"
#include "map.h"
#include "map_memory.h"
#include "map_scale_constants.h"
#include "monster.h"
#include "mtype.h"
#include "npc.h"
#include "trap.h"
#include "type_id.h"
#include "units.h"
#include "veh_type.h"
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

// Creature flags: the local avatar, another body (NPC or co-op slot), a
// one-level-away hint with no body, and inclusion through an enchantment's
// special vision rather than normal sight.
constexpr uint8_t creature_is_you = 0x1;
constexpr uint8_t creature_other_body = 0x2;
constexpr uint8_t creature_marker_only = 0x4;
constexpr uint8_t creature_special_vision = 0x8;

// The dictionary id the client resolves to a mesh. The local avatar and co-op
// slots share "avatar"; NPC classes fall back to a generic body when unset.
std::string creature_dictionary_id( const Creature &critter )
{
    if( const monster *mon = critter.as_monster() ) {
        return mon->type->id.str();
    }
    if( const npc *guy = critter.as_npc() ) {
        const std::string cls = guy->myclass.str();
        return cls.empty() ? "npc" : cls;
    }
    return "avatar";
}

// Graffiti is untrusted player text: strip controls and bidi overrides that
// could reorder a terminal, then cap it. The cap is in bytes so the wire entry
// stays bounded; whole UTF-8 sequences are kept or dropped together.
constexpr size_t max_graffiti_bytes = 120;

bool stripped_graffiti_codepoint( uint32_t codepoint )
{
    return codepoint <= 0x1F ||                                  // C0 controls
           ( codepoint >= 0x80 && codepoint <= 0x9F ) ||         // C1 controls
           codepoint == 0x200E || codepoint == 0x200F ||         // LRM, RLM
           ( codepoint >= 0x202A && codepoint <= 0x202E ) ||     // LRE..PDF
           ( codepoint >= 0x2066 && codepoint <= 0x2069 );       // LRI..PDI
}

std::string sanitize_graffiti( const std::string &text )
{
    std::string result;
    for( const char32_t codepoint : utf8_view( text ) ) {
        // UTF8_getch reports invalid sequences as the replacement character;
        // dropping them also keeps the output valid UTF-8.
        if( codepoint == UNKNOWN_UNICODE || stripped_graffiti_codepoint( codepoint ) ) {
            continue;
        }
        const std::string encoded = utf32_to_utf8( codepoint );
        if( result.size() + encoded.size() > max_graffiti_bytes ) {
            break;
        }
        result += encoded;
    }
    return result;
}

int8_t part_direction_256( units::angle direction )
{
    const long turn = std::lround( units::to_degrees( direction ) * 256.0 / 360.0 );
    return static_cast<int8_t>( ( turn % 256 + 256 ) % 256 );
}

uint16_t vehicle_heading_65536( const vehicle &veh )
{
    return static_cast<uint16_t>(
               std::lround( units::to_degrees( veh.face.dir() ) * 65536.0 / 360.0 ) & 0xFFFF );
}

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

    // Vehicles need only be unique within this frame; the client groups parts
    // per frame and frames are rebuilt on every send. `om_id` is 0 for any
    // vehicle not yet registered with the overmap, so it cannot be used here.
    std::map<const vehicle *, uint16_t> vehicle_ids;
    uint16_t next_vehicle_id = 0;
    auto vehicle_wire_id = [&]( const vehicle &veh ) -> uint16_t {
        const auto found = vehicle_ids.find( &veh );
        if( found != vehicle_ids.end() ) {
            return found->second;
        }
        const uint16_t id = next_vehicle_id++;
        vehicle_ids.emplace( &veh, id );
        scene.vehicles.push_back( mp_scene_vehicle{ id, vehicle_heading_65536( veh ) } );
        return id;
    };

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

        for( const wrapped_vehicle &wv : here.get_vehicles() ) {
            if( wv.pos.z() == z ) {
                vehicle_wire_id( *wv.v );
            }
        }

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

                        const uint16_t x16 = static_cast<uint16_t>( x );
                        const uint16_t y16 = static_cast<uint16_t>( y );

                        if( const furn_id furn = here.furn( p ) ) {
                            level.furniture.push_back(
                                mp_scene_point_id{ x16, y16, dictionary.add( furn.id().str() ) } );
                        }

                        if( here.sees_some_items( p, you ) ) {
                            const map_stack items = here.i_at( p );
                            level.items.push_back( mp_scene_point_id_aux{
                                x16, y16, dictionary.add( items.rbegin()->typeId().str() ),
                                static_cast<uint8_t>( std::min<size_t>( items.size(), UINT8_MAX ) )
                            } );
                        }

                        const field &fields_here = here.field_at( p );
                        if( fields_here.field_count() > 0 ) {
                            const field_type_id &fid = fields_here.displayed_field_type();
                            if( const field_entry *entry = fields_here.find_field( fid ) ) {
                                level.fields.push_back( mp_scene_point_id_aux{
                                    x16, y16, dictionary.add( fid.id().str() ),
                                    static_cast<uint8_t>( std::min( entry->get_field_intensity(),
                                                         static_cast<int>( UINT8_MAX ) ) )
                                } );
                            }
                        }

                        if( const optional_vpart_position vp = here.veh_at( p ) ) {
                            const vehicle &veh = vp->vehicle();
                            const vehicle_part &part = veh.part( vp->part_index() );
                            level.vparts.push_back( mp_scene_vpart{
                                x16, y16, dictionary.add( part.info().id.str() ),
                                static_cast<uint16_t>( vp->part_index() ),
                                vehicle_wire_id( veh ),
                                part_direction_256( part.direction )
                            } );
                        }

                        const trap &tr = here.tr_at( p );
                        if( !tr.is_null() && tr.can_see( p, you ) ) {
                            level.traps.push_back(
                                mp_scene_point_id{ x16, y16, dictionary.add( tr.id.str() ) } );
                        }

                        if( const partial_con *con = here.partial_con_at( p ) ) {
                            // A malformed partial construction can carry the
                            // default construction_id(-1); obj() is unsafe.
                            if( con->id.is_valid() ) {
                                level.constructions.push_back( mp_scene_point_id_aux{
                                    x16, y16, dictionary.add( con->id.obj().id.str() ),
                                    static_cast<uint8_t>( std::min( con->counter / 100000,
                                                             static_cast<int>( UINT8_MAX ) ) )
                                } );
                            }
                        }

                        if( here.has_graffiti_at( p ) ) {
                            const std::string text = sanitize_graffiti( here.graffiti_at( p ) );
                            if( !text.empty() ) {
                                level.graffiti.push_back(
                                    mp_scene_point_id{ x16, y16, dictionary.add( text ) } );
                            }
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

    // Creatures belong to the player's own level. A creature one level away is
    // only a marker there: the vertical drop is what sight and movement allow,
    // and the client draws a hint rather than a body. Co-op slots are avatars
    // outside all_creatures() and are sent unconditionally.
    if( !scene.levels.empty() ) {
        mp_scene_level &own_level = scene.levels.front();

        // The wire cannot express a cell outside the rect, and the decoder
        // rejects one, so everything here is clipped to it. The avatar itself
        // can be outside while the player looks around; the frame still carries
        // its position in avatar_dx/avatar_dy.
        const auto rect_cell = [&]( const tripoint_bub_ms & p, uint16_t & x16, uint16_t & y16 ) {
            const int x = p.x() - origin.x();
            const int y = p.y() - origin.y();
            if( x < 0 || y < 0 || x >= width || y >= height ) {
                return false;
            }
            x16 = static_cast<uint16_t>( x );
            y16 = static_cast<uint16_t>( y );
            return true;
        };

        for( const Creature &critter : g->all_creatures() ) {
            const tripoint_bub_ms p = critter.pos_bub();
            const int dz = p.z() - base_z;
            uint8_t flags = 0;

            if( dz == 0 ) {
                // Only the active avatar is in all_creatures(), so is_avatar()
                // is exactly the frame's `you`.
                if( critter.is_avatar() ) {
                    flags = creature_is_you;
                } else if( you.sees( here, critter ) ) {
                    flags = critter.is_npc() ? creature_other_body : 0;
                } else if( you.sees_with_specials( critter ) ) {
                    // Seen only through an enchantment: the client needs to
                    // know so it can pick its special-vision presentation.
                    flags = creature_special_vision;
                    if( critter.is_npc() ) {
                        flags |= creature_other_body;
                    }
                } else {
                    continue;
                }
            } else if( std::abs( dz ) == 1 ) {
                if( critter.is_avatar() || !you.sees( here, critter ) ) {
                    continue;
                }
                // The move faces the player's level; an open-air tile between
                // is passable, a floor is not.
                if( !here.valid_move( p, p + tripoint_rel_ms( 0, 0, -dz ) ) ) {
                    continue;
                }
                flags = creature_marker_only;
                if( critter.is_npc() ) {
                    flags |= creature_other_body;
                }
            } else {
                continue;
            }

            uint16_t x16 = 0;
            uint16_t y16 = 0;
            if( !rect_cell( p, x16, y16 ) ) {
                continue;
            }
            own_level.creatures.push_back( mp_scene_creature{
                x16, y16, static_cast<int16_t>( dz ),
                dictionary.add( creature_dictionary_id( critter ) ),
                static_cast<uint8_t>( critter.get_size() ), flags
            } );
        }

        // draw_ter() draws every co-op slot on the player's level without any
        // visibility check: you always know where your own other character is.
        for( avatar *slot : g->get_players() ) {
            if( slot == &you ) {
                continue;
            }
            const tripoint_bub_ms p = slot->pos_bub( here );
            if( p.z() != base_z ) {
                continue;
            }
            uint16_t x16 = 0;
            uint16_t y16 = 0;
            if( !rect_cell( p, x16, y16 ) ) {
                continue;
            }
            own_level.creatures.push_back( mp_scene_creature{
                x16, y16, 0, dictionary.add( "avatar" ),
                static_cast<uint8_t>( slot->get_size() ), creature_other_body
            } );
        }
    }

    scene.ids = std::move( dictionary.ids );
    return scene;
}
