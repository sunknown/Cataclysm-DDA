#include "mp/mp_scene_codec.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

#include "debug.h"
#include "mp/mp_scene.h"

namespace
{

void put_u8( std::vector<uint8_t> &out, const uint8_t value )
{
    out.push_back( value );
}

void put_u16( std::vector<uint8_t> &out, const uint16_t value )
{
    out.push_back( static_cast<uint8_t>( ( value >> 8 ) & 0xFF ) );
    out.push_back( static_cast<uint8_t>( value & 0xFF ) );
}

void put_i16( std::vector<uint8_t> &out, const int16_t value )
{
    put_u16( out, static_cast<uint16_t>( value ) );
}

void put_u32( std::vector<uint8_t> &out, const uint32_t value )
{
    out.push_back( static_cast<uint8_t>( ( value >> 24 ) & 0xFF ) );
    out.push_back( static_cast<uint8_t>( ( value >> 16 ) & 0xFF ) );
    out.push_back( static_cast<uint8_t>( ( value >> 8 ) & 0xFF ) );
    out.push_back( static_cast<uint8_t>( value & 0xFF ) );
}

void put_id( std::vector<uint8_t> &out, const std::string &text )
{
    put_u16( out, static_cast<uint16_t>( text.size() ) );
    out.insert( out.end(), text.begin(), text.end() );
}

class reader
{
    public:
        explicit reader( const std::vector<uint8_t> &data ) : data_( data ) {}

        bool u8( uint8_t &out ) {
            if( pos_ >= data_.size() ) {
                return false;
            }
            out = data_[pos_++];
            return true;
        }
        bool u16( uint16_t &out ) {
            uint8_t hi = 0;
            uint8_t lo = 0;
            if( !u8( hi ) || !u8( lo ) ) {
                return false;
            }
            out = static_cast<uint16_t>( ( static_cast<uint16_t>( hi ) << 8 ) | lo );
            return true;
        }
        bool i8( int8_t &out ) {
            uint8_t value = 0;
            if( !u8( value ) ) {
                return false;
            }
            out = value < 0x80 ? static_cast<int8_t>( value )
                  : static_cast<int8_t>( static_cast<int>( value ) - 0x100 );
            return true;
        }
        bool i16( int16_t &out ) {
            uint16_t value = 0;
            if( !u16( value ) ) {
                return false;
            }
            out = value < 0x8000 ? static_cast<int16_t>( value )
                  : static_cast<int16_t>( static_cast<int>( value ) - 0x10000 );
            return true;
        }
        bool u32( uint32_t &out ) {
            uint16_t hi = 0;
            uint16_t lo = 0;
            if( !u16( hi ) || !u16( lo ) ) {
                return false;
            }
            out = ( static_cast<uint32_t>( hi ) << 16 ) | lo;
            return true;
        }
        bool str( const uint16_t len, std::string &out ) {
            if( remaining() < len ) {
                return false;
            }
            out.assign( reinterpret_cast<const char *>( data_.data() + pos_ ), len );
            pos_ += len;
            return true;
        }
        size_t remaining() const {
            return data_.size() - pos_;
        }
        bool at_end() const {
            return pos_ == data_.size();
        }

    private:
        const std::vector<uint8_t> &data_;
        size_t pos_ = 0;
};

// Every layer entry except terrain is packed into a fixed number of bytes, so
// a hostile count can be rejected against the bytes left before any of them is
// read or stored.
constexpr size_t furniture_entry_bytes = 6;
constexpr size_t aux_entry_bytes = 7;
constexpr size_t vpart_entry_bytes = 11;
constexpr size_t creature_entry_bytes = 10;
constexpr size_t terrain_run_bytes = 6;

bool read_point_ids( reader &r, std::vector<mp_scene_point_id> &out,
                     const uint16_t tiles_w, const uint16_t tiles_h,
                     const size_t id_count )
{
    uint16_t count = 0;
    if( !r.u16( count ) || r.remaining() < static_cast<size_t>( count ) * furniture_entry_bytes ) {
        return false;
    }
    for( uint16_t i = 0; i < count; ++i ) {
        mp_scene_point_id entry;
        if( !r.u16( entry.x ) || !r.u16( entry.y ) || !r.u16( entry.id_idx ) ) {
            return false;
        }
        if( entry.x >= tiles_w || entry.y >= tiles_h || entry.id_idx >= id_count ) {
            return false;
        }
        out.push_back( entry );
    }
    return true;
}

bool read_point_ids_aux( reader &r, std::vector<mp_scene_point_id_aux> &out,
                         const uint16_t tiles_w, const uint16_t tiles_h,
                         const size_t id_count )
{
    uint16_t count = 0;
    if( !r.u16( count ) || r.remaining() < static_cast<size_t>( count ) * aux_entry_bytes ) {
        return false;
    }
    for( uint16_t i = 0; i < count; ++i ) {
        mp_scene_point_id_aux entry;
        if( !r.u16( entry.x ) || !r.u16( entry.y ) || !r.u16( entry.id_idx ) ||
            !r.u8( entry.aux ) ) {
            return false;
        }
        if( entry.x >= tiles_w || entry.y >= tiles_h || entry.id_idx >= id_count ) {
            return false;
        }
        out.push_back( entry );
    }
    return true;
}

bool read_vparts( reader &r, std::vector<mp_scene_vpart> &out,
                  const uint16_t tiles_w, const uint16_t tiles_h,
                  const size_t id_count )
{
    uint16_t count = 0;
    if( !r.u16( count ) || r.remaining() < static_cast<size_t>( count ) * vpart_entry_bytes ) {
        return false;
    }
    for( uint16_t i = 0; i < count; ++i ) {
        mp_scene_vpart entry;
        if( !r.u16( entry.x ) || !r.u16( entry.y ) || !r.u16( entry.id_idx ) ||
            !r.u16( entry.part_index ) || !r.u16( entry.veh_id ) || !r.i8( entry.dir ) ) {
            return false;
        }
        if( entry.x >= tiles_w || entry.y >= tiles_h || entry.id_idx >= id_count ) {
            return false;
        }
        out.push_back( entry );
    }
    return true;
}

bool read_creatures( reader &r, std::vector<mp_scene_creature> &out,
                     const uint16_t tiles_w, const uint16_t tiles_h,
                     const size_t id_count )
{
    uint16_t count = 0;
    if( !r.u16( count ) || r.remaining() < static_cast<size_t>( count ) * creature_entry_bytes ) {
        return false;
    }
    for( uint16_t i = 0; i < count; ++i ) {
        mp_scene_creature entry;
        if( !r.u16( entry.x ) || !r.u16( entry.y ) || !r.i16( entry.z_rel ) ||
            !r.u16( entry.id_idx ) || !r.u8( entry.size_hint ) || !r.u8( entry.flags ) ) {
            return false;
        }
        if( entry.x >= tiles_w || entry.y >= tiles_h || entry.id_idx >= id_count ) {
            return false;
        }
        // The frame carries the player's level and nine levels below it; a
        // creature can also be a marker one level above the player.
        if( entry.z_rel < -MP_SCENE_MAX_LEVELS || entry.z_rel > 1 ) {
            return false;
        }
        out.push_back( entry );
    }
    return true;
}

} // namespace

std::vector<uint8_t> mp_scene_encode( const mp_scene &scene )
{
    std::vector<uint8_t> out;
    put_u8( out, scene.body_version );
    put_u32( out, scene.frame_seq );
    put_u16( out, scene.tiles_w );
    put_u16( out, scene.tiles_h );
    put_i16( out, scene.view_dx );
    put_i16( out, scene.view_dy );
    put_i16( out, scene.avatar_dx );
    put_i16( out, scene.avatar_dy );
    put_u16( out, scene.ui_x );
    put_u16( out, scene.ui_y );
    put_u16( out, scene.ui_w );
    put_u16( out, scene.ui_h );

    const size_t mask_count = std::min( scene.masks.size(),
                                        static_cast<size_t>( MP_SCENE_MAX_MASKS ) );
    put_u8( out, static_cast<uint8_t>( mask_count ) );
    for( size_t i = 0; i < mask_count; ++i ) {
        const mp_scene_rect &mask = scene.masks[i];
        put_u16( out, mask.x );
        put_u16( out, mask.y );
        put_u16( out, mask.w );
        put_u16( out, mask.h );
        put_u8( out, mask.role );
    }
    if( scene.masks.size() > mask_count ) {
        static bool warned = false;
        if( !warned ) {
            warned = true;
            DebugLog( D_WARNING, D_MAIN ) << "mp scene: dropping "
                                          << scene.masks.size() - mask_count
                                          << " masks past the "
                                          << static_cast<int>( MP_SCENE_MAX_MASKS )
                                          << "-mask wire limit";
        }
    }

    put_u16( out, static_cast<uint16_t>( scene.vehicles.size() ) );
    for( const mp_scene_vehicle &vehicle : scene.vehicles ) {
        put_u16( out, vehicle.veh_id );
        put_u16( out, vehicle.heading );
    }

    // Index 0 is the empty id by contract. A scene that has never interned
    // anything still needs that entry, or the first id_idx would be invalid.
    put_u16( out, static_cast<uint16_t>( scene.ids.empty() ? 1 : scene.ids.size() ) );
    if( scene.ids.empty() ) {
        put_id( out, std::string() );
    } else {
        for( const std::string &id : scene.ids ) {
            put_id( out, id );
        }
    }

    put_u8( out, static_cast<uint8_t>( scene.levels.size() ) );
    for( const mp_scene_level &level : scene.levels ) {
        out.push_back( static_cast<uint8_t>( level.z_rel ) );

        // Compression belongs to the encoder: a builder may push one run per
        // cell (or per changed cell), and neighbouring runs of the same cell
        // leave as a single run. repeat is a u16, so a merge that would not
        // fit starts a new run instead.
        std::vector<mp_scene_terrain_run> runs;
        for( const mp_scene_terrain_run &run : level.terrain ) {
            if( !runs.empty() ) {
                mp_scene_terrain_run &last = runs.back();
                const uint32_t summed = static_cast<uint32_t>( last.repeat ) + run.repeat;
                if( last.terrain_idx == run.terrain_idx && last.flags == run.flags &&
                    last.lit == run.lit && summed <= 0xFFFF ) {
                    last.repeat = static_cast<uint16_t>( summed );
                    continue;
                }
            }
            runs.push_back( run );
        }
        put_u16( out, static_cast<uint16_t>( runs.size() ) );
        for( const mp_scene_terrain_run &run : runs ) {
            put_u16( out, run.repeat );
            put_u16( out, run.terrain_idx );
            put_u8( out, run.flags );
            put_u8( out, run.lit );
        }

        put_u16( out, static_cast<uint16_t>( level.furniture.size() ) );
        for( const mp_scene_point_id &entry : level.furniture ) {
            put_u16( out, entry.x );
            put_u16( out, entry.y );
            put_u16( out, entry.id_idx );
        }

        put_u16( out, static_cast<uint16_t>( level.items.size() ) );
        for( const mp_scene_point_id_aux &entry : level.items ) {
            put_u16( out, entry.x );
            put_u16( out, entry.y );
            put_u16( out, entry.id_idx );
            put_u8( out, entry.aux );
        }

        put_u16( out, static_cast<uint16_t>( level.fields.size() ) );
        for( const mp_scene_point_id_aux &entry : level.fields ) {
            put_u16( out, entry.x );
            put_u16( out, entry.y );
            put_u16( out, entry.id_idx );
            put_u8( out, entry.aux );
        }

        put_u16( out, static_cast<uint16_t>( level.vparts.size() ) );
        for( const mp_scene_vpart &entry : level.vparts ) {
            put_u16( out, entry.x );
            put_u16( out, entry.y );
            put_u16( out, entry.id_idx );
            put_u16( out, entry.part_index );
            put_u16( out, entry.veh_id );
            out.push_back( static_cast<uint8_t>( entry.dir ) );
        }

        put_u16( out, static_cast<uint16_t>( level.traps.size() ) );
        for( const mp_scene_point_id &entry : level.traps ) {
            put_u16( out, entry.x );
            put_u16( out, entry.y );
            put_u16( out, entry.id_idx );
        }

        put_u16( out, static_cast<uint16_t>( level.constructions.size() ) );
        for( const mp_scene_point_id_aux &entry : level.constructions ) {
            put_u16( out, entry.x );
            put_u16( out, entry.y );
            put_u16( out, entry.id_idx );
            put_u8( out, entry.aux );
        }

        put_u16( out, static_cast<uint16_t>( level.graffiti.size() ) );
        for( const mp_scene_point_id &entry : level.graffiti ) {
            put_u16( out, entry.x );
            put_u16( out, entry.y );
            put_u16( out, entry.id_idx );
        }

        put_u16( out, static_cast<uint16_t>( level.creatures.size() ) );
        for( const mp_scene_creature &entry : level.creatures ) {
            put_u16( out, entry.x );
            put_u16( out, entry.y );
            put_i16( out, entry.z_rel );
            put_u16( out, entry.id_idx );
            put_u8( out, entry.size_hint );
            put_u8( out, entry.flags );
        }
    }

    return out;
}

bool mp_scene_apply( mp_scene &target, const std::vector<uint8_t> &body )
{
    reader r( body );
    mp_scene scene;
    if( !r.u8( scene.body_version ) || scene.body_version != MP_SCENE_BODY_VERSION ) {
        return false;
    }
    if( !r.u32( scene.frame_seq ) || !r.u16( scene.tiles_w ) || !r.u16( scene.tiles_h ) ||
        !r.i16( scene.view_dx ) || !r.i16( scene.view_dy ) ||
        !r.i16( scene.avatar_dx ) || !r.i16( scene.avatar_dy ) ||
        !r.u16( scene.ui_x ) || !r.u16( scene.ui_y ) ||
        !r.u16( scene.ui_w ) || !r.u16( scene.ui_h ) ) {
        return false;
    }
    if( scene.tiles_w == 0 || scene.tiles_h == 0 ) {
        return false;
    }

    uint8_t mask_count = 0;
    if( !r.u8( mask_count ) || r.remaining() < static_cast<size_t>( mask_count ) * 9 ) {
        return false;
    }
    for( uint8_t i = 0; i < mask_count; ++i ) {
        mp_scene_rect mask;
        if( !r.u16( mask.x ) || !r.u16( mask.y ) || !r.u16( mask.w ) ||
            !r.u16( mask.h ) || !r.u8( mask.role ) ) {
            return false;
        }
        // Mask rects are absolute terminal cells and must stay inside the
        // viewport's ui_rect; an empty or oversized rect cannot be drawn.
        if( mask.role > 1 || mask.w == 0 || mask.h == 0 ) {
            return false;
        }
        const uint32_t ui_right = static_cast<uint32_t>( scene.ui_x ) + scene.ui_w;
        const uint32_t ui_bottom = static_cast<uint32_t>( scene.ui_y ) + scene.ui_h;
        if( mask.x < scene.ui_x || mask.y < scene.ui_y ||
            static_cast<uint32_t>( mask.x ) + mask.w > ui_right ||
            static_cast<uint32_t>( mask.y ) + mask.h > ui_bottom ) {
            return false;
        }
        scene.masks.push_back( mask );
    }

    uint16_t vehicle_count = 0;
    if( !r.u16( vehicle_count ) || r.remaining() < static_cast<size_t>( vehicle_count ) * 4 ) {
        return false;
    }
    for( uint16_t i = 0; i < vehicle_count; ++i ) {
        mp_scene_vehicle vehicle;
        if( !r.u16( vehicle.veh_id ) || !r.u16( vehicle.heading ) ) {
            return false;
        }
        scene.vehicles.push_back( vehicle );
    }

    uint16_t id_count = 0;
    if( !r.u16( id_count ) || id_count == 0 ||
        r.remaining() < static_cast<size_t>( id_count ) * 2 ) {
        return false;
    }
    for( uint16_t i = 0; i < id_count; ++i ) {
        uint16_t len = 0;
        std::string id;
        if( !r.u16( len ) || !r.str( len, id ) ) {
            return false;
        }
        // Index 0 is the empty id by contract; a body that names it otherwise
        // cannot be one this encoder produced.
        if( i == 0 && !id.empty() ) {
            return false;
        }
        scene.ids.push_back( std::move( id ) );
    }

    uint8_t z_count = 0;
    if( !r.u8( z_count ) || z_count < 1 || z_count > MP_SCENE_MAX_LEVELS ) {
        return false;
    }

    const uint16_t tiles_w = scene.tiles_w;
    const uint16_t tiles_h = scene.tiles_h;
    const size_t ids = scene.ids.size();
    const uint64_t cells = static_cast<uint64_t>( tiles_w ) * tiles_h;
    for( uint8_t i = 0; i < z_count; ++i ) {
        mp_scene_level level;
        if( !r.i8( level.z_rel ) ) {
            return false;
        }
        // Levels arrive as the player's level and the nine below it, in that
        // order; z_rel is what every client layer is placed against, so any
        // other value would silently put the level somewhere it is not.
        if( level.z_rel != -static_cast<int8_t>( i ) ) {
            return false;
        }

        uint16_t run_count = 0;
        if( !r.u16( run_count ) || r.remaining() < static_cast<size_t>( run_count ) *
            terrain_run_bytes ) {
            return false;
        }
        uint64_t covered = 0;
        for( uint16_t j = 0; j < run_count; ++j ) {
            mp_scene_terrain_run run;
            if( !r.u16( run.repeat ) || !r.u16( run.terrain_idx ) ||
                !r.u8( run.flags ) || !r.u8( run.lit ) ) {
                return false;
            }
            // lit_level runs 0..6 (src/lightmap.h); anything else is not a
            // value the renderer could have produced.
            if( run.repeat == 0 || run.lit > 6 || run.terrain_idx >= ids ) {
                return false;
            }
            covered += run.repeat;
            if( covered > cells ) {
                return false;
            }
            level.terrain.push_back( run );
        }
        // Every cell of the level must be covered exactly once; a short cover
        // would leave the client's tile layer undefined.
        if( covered != cells ) {
            return false;
        }

        if( !read_point_ids( r, level.furniture, tiles_w, tiles_h, ids ) ||
            !read_point_ids_aux( r, level.items, tiles_w, tiles_h, ids ) ||
            !read_point_ids_aux( r, level.fields, tiles_w, tiles_h, ids ) ||
            !read_vparts( r, level.vparts, tiles_w, tiles_h, ids ) ||
            !read_point_ids( r, level.traps, tiles_w, tiles_h, ids ) ||
            !read_point_ids_aux( r, level.constructions, tiles_w, tiles_h, ids ) ||
            !read_point_ids( r, level.graffiti, tiles_w, tiles_h, ids ) ||
            !read_creatures( r, level.creatures, tiles_w, tiles_h, ids ) ) {
            return false;
        }

        scene.levels.push_back( std::move( level ) );
    }

    if( !r.at_end() ) {
        return false;
    }
    target = std::move( scene );
    return true;
}

uint64_t mp_scene_body_hash( const std::vector<uint8_t> &body )
{
    uint64_t hash = 14695981039346656037ULL;
    for( size_t i = 5; i < body.size(); ++i ) {
        hash ^= static_cast<uint64_t>( body[i] );
        hash *= 1099511628211ULL;
    }
    return hash;
}
