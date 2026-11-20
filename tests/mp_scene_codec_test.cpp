#include "cata_catch.h"

#if defined(MULTIPLAYER)

#include <cstdint>
#include <cstddef>
#include <string>
#include <tuple>
#include <vector>

#include "mp/mp_scene.h"
#include "mp/mp_scene_codec.h"

namespace
{

// Field-by-field comparisons. The model has no equality operators on purpose:
// a defaulted one would need a manual field list anyway in C++17, and the codec
// is the part that must prove every field survives the wire.

bool same( const mp_scene_rect &a, const mp_scene_rect &b )
{
    return std::tie( a.x, a.y, a.w, a.h, a.role ) ==
           std::tie( b.x, b.y, b.w, b.h, b.role );
}

bool same( const mp_scene_vehicle &a, const mp_scene_vehicle &b )
{
    return std::tie( a.veh_id, a.heading ) == std::tie( b.veh_id, b.heading );
}

bool same( const mp_scene_point_id &a, const mp_scene_point_id &b )
{
    return std::tie( a.x, a.y, a.id_idx ) == std::tie( b.x, b.y, b.id_idx );
}

bool same( const mp_scene_point_id_aux &a, const mp_scene_point_id_aux &b )
{
    return std::tie( a.x, a.y, a.id_idx, a.aux ) ==
           std::tie( b.x, b.y, b.id_idx, b.aux );
}

bool same( const mp_scene_vpart &a, const mp_scene_vpart &b )
{
    return std::tie( a.x, a.y, a.id_idx, a.part_index, a.veh_id, a.dir ) ==
           std::tie( b.x, b.y, b.id_idx, b.part_index, b.veh_id, b.dir );
}

bool same( const mp_scene_creature &a, const mp_scene_creature &b )
{
    return std::tie( a.x, a.y, a.z_rel, a.id_idx, a.size_hint, a.flags ) ==
           std::tie( b.x, b.y, b.z_rel, b.id_idx, b.size_hint, b.flags );
}

bool same( const mp_scene_terrain_run &a, const mp_scene_terrain_run &b )
{
    return std::tie( a.repeat, a.terrain_idx, a.flags, a.lit ) ==
           std::tie( b.repeat, b.terrain_idx, b.flags, b.lit );
}

template <typename T>
bool same_vec( const std::vector<T> &a, const std::vector<T> &b,
               bool ( *eq )( const T &, const T & ) )
{
    if( a.size() != b.size() ) {
        return false;
    }
    for( size_t i = 0; i < a.size(); ++i ) {
        if( !eq( a[i], b[i] ) ) {
            return false;
        }
    }
    return true;
}

bool same( const mp_scene_level &a, const mp_scene_level &b )
{
    return a.z_rel == b.z_rel &&
           same_vec( a.terrain, b.terrain, same ) &&
           same_vec( a.furniture, b.furniture, same ) &&
           same_vec( a.items, b.items, same ) &&
           same_vec( a.fields, b.fields, same ) &&
           same_vec( a.vparts, b.vparts, same ) &&
           same_vec( a.traps, b.traps, same ) &&
           same_vec( a.constructions, b.constructions, same ) &&
           same_vec( a.graffiti, b.graffiti, same ) &&
           same_vec( a.creatures, b.creatures, same );
}

// A scene that exercises every block of the wire format at least once.
mp_scene full_scene()
{
    mp_scene s;
    s.body_version = MP_SCENE_BODY_VERSION;
    s.frame_seq = 4242;
    s.tiles_w = 8;
    s.tiles_h = 4;
    s.view_dx = -4;
    s.view_dy = -2;
    // The avatar may sit outside the rect while the player looks around.
    s.avatar_dx = 9;
    s.avatar_dy = 2;
    s.ui_x = 0;
    s.ui_y = 0;
    s.ui_w = 8;
    s.ui_h = 4;

    s.masks.push_back( { 0, 0, 5, 3, 0 } );
    s.masks.push_back( { 5, 0, 3, 1, 1 } );

    s.vehicles.push_back( { 5, 0x8000 } );

    s.ids = { "", "t_floor", "t_wall", "f_bench", "mon_zombie", "item_rock",
              "field_blood", "vpart_cart" };

    mp_scene_level ground;
    ground.z_rel = 0;
    // 2 + 20 + 10 cells: the same terrain twice in one run, and a neighbouring
    // run that only differs by flags and light.
    ground.terrain = { { 2, 1, 3, 4 }, { 20, 2, 2, 2 }, { 10, 1, 0, 1 } };
    ground.furniture = { { 1, 1, 3 }, { 6, 3, 3 } };
    ground.items = { { 2, 2, 5, 7 } };
    ground.fields = { { 4, 1, 6, 9 } };
    ground.vparts = { { 3, 3, 7, 2, 5, -3 } };
    ground.traps = { { 0, 3, 1 } };
    ground.constructions = { { 5, 2, 2, 62 } };
    ground.graffiti = { { 7, 3, 1 } };
    ground.creatures = { { 4, 2, 0, 4, 150, 1 },
        // A marker_only creature one level above the player.
        { 5, 1, 1, 4, 0, 0x04 } };

    mp_scene_level below;
    below.z_rel = -1;
    below.terrain = { { 32, 2, 1, 0 } };
    below.creatures = { { 4, 2, -1, 4, 0, 0x0A } };

    s.levels = { ground, below };
    return s;
}

// Wire-building helpers for the hand-written minimal body below. Keeping the
// malformed-body fixtures independent of mp_scene_encode means an encoder bug
// cannot hide behind them.
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

// A complete, valid body: 2x2 tiles, two dictionary entries, one entry in
// every layer block. `bytes` is what a correct encoder would produce for the
// corresponding scene; the offsets record the fields the rejection tests
// corrupt in place.
struct minimal_body {
    std::vector<uint8_t> bytes;
    size_t tiles_w = 0;
    size_t tiles_h = 0;
    size_t id_count = 0;
    size_t first_id_len = 0;
    size_t mask_role = 0;
    size_t z_count = 0;
    size_t level_z_rel = 0;
    size_t terrain_repeat = 0;
    size_t furniture_x = 0;
    size_t furniture_id_idx = 0;
    size_t creature_z_rel = 0;
};

minimal_body make_minimal_body()
{
    minimal_body f;
    std::vector<uint8_t> &b = f.bytes;

    put_u8( b, MP_SCENE_BODY_VERSION );
    put_u32( b, 9 );
    f.tiles_w = b.size();
    put_u16( b, 2 );            // tiles_w
    f.tiles_h = b.size();
    put_u16( b, 2 );            // tiles_h
    put_i16( b, -1 );           // view_dx
    put_i16( b, -1 );           // view_dy
    put_i16( b, 0 );            // avatar_dx
    put_i16( b, 1 );            // avatar_dy
    put_u16( b, 1 );            // ui_x
    put_u16( b, 2 );            // ui_y
    put_u16( b, 3 );            // ui_w
    put_u16( b, 4 );            // ui_h

    put_u8( b, 1 );             // mask_count
    put_u16( b, 1 );            // x
    put_u16( b, 2 );            // y
    put_u16( b, 3 );            // w
    put_u16( b, 4 );            // h
    f.mask_role = b.size();
    put_u8( b, 0 );             // role: text over 3D

    put_u16( b, 1 );            // veh_count
    put_u16( b, 5 );            // veh_id
    put_u16( b, 0x8000 );       // heading

    f.id_count = b.size();
    put_u16( b, 2 );            // id_count
    f.first_id_len = b.size();
    put_id( b, "" );
    put_id( b, "t_floor" );

    f.z_count = b.size();
    put_u8( b, 1 );             // z_count
    f.level_z_rel = b.size();
    put_u8( b, 0 );             // z_rel, as int8_t

    put_u16( b, 1 );            // terrain run count
    f.terrain_repeat = b.size();
    put_u16( b, 4 );            // repeat
    put_u16( b, 1 );            // terrain_idx
    put_u8( b, 3 );             // flags: visible | memorized
    put_u8( b, 4 );             // lit

    put_u16( b, 1 );            // furniture count
    f.furniture_x = b.size();
    put_u16( b, 1 );            // x
    put_u16( b, 1 );            // y
    f.furniture_id_idx = b.size();
    put_u16( b, 1 );            // id_idx

    put_u16( b, 1 );            // items count
    put_u16( b, 0 );            // x
    put_u16( b, 1 );            // y
    put_u16( b, 1 );            // id_idx
    put_u8( b, 3 );             // stack_size

    put_u16( b, 1 );            // fields count
    put_u16( b, 0 );            // x
    put_u16( b, 1 );            // y
    put_u16( b, 1 );            // id_idx
    put_u8( b, 2 );             // intensity

    put_u16( b, 1 );            // vparts count
    put_u16( b, 1 );            // x
    put_u16( b, 0 );            // y
    put_u16( b, 1 );            // id_idx
    put_u16( b, 2 );            // part_index
    put_u16( b, 5 );            // veh_id
    put_u8( b, 7 );             // dir

    put_u16( b, 1 );            // traps count
    put_u16( b, 0 );            // x
    put_u16( b, 0 );            // y
    put_u16( b, 1 );            // id_idx

    put_u16( b, 1 );            // constructions count
    put_u16( b, 1 );            // x
    put_u16( b, 1 );            // y
    put_u16( b, 1 );            // id_idx
    put_u8( b, 50 );            // percent

    put_u16( b, 1 );            // graffiti count
    put_u16( b, 0 );            // x
    put_u16( b, 1 );            // y
    put_u16( b, 1 );            // text_idx

    put_u16( b, 1 );            // creatures count
    put_u16( b, 1 );            // x
    put_u16( b, 1 );            // y
    f.creature_z_rel = b.size();
    put_i16( b, 0 );            // z_rel
    put_u16( b, 1 );            // id_idx
    put_u8( b, 2 );             // size_hint
    put_u8( b, 1 );             // flags

    return f;
}

std::vector<uint8_t> with_u8( const std::vector<uint8_t> &body, const size_t at,
                              const uint8_t value )
{
    std::vector<uint8_t> copy = body;
    copy[at] = value;
    return copy;
}

std::vector<uint8_t> with_u16( const std::vector<uint8_t> &body, const size_t at,
                               const uint16_t value )
{
    std::vector<uint8_t> copy = body;
    copy[at] = static_cast<uint8_t>( ( value >> 8 ) & 0xFF );
    copy[at + 1] = static_cast<uint8_t>( value & 0xFF );
    return copy;
}

// A destination scene with content, to prove a rejected body changes nothing.
mp_scene populated_destination()
{
    mp_scene s;
    s.body_version = MP_SCENE_BODY_VERSION;
    s.frame_seq = 77;
    s.tiles_w = 1;
    s.tiles_h = 1;
    s.ui_w = 1;
    s.ui_h = 1;
    s.ids = { "" };
    mp_scene_level level;
    level.terrain = { { 1, 0, 0, 0 } };
    s.levels = { level };
    return s;
}

} // namespace

TEST_CASE( "mp_scene_codec_round_trips_every_block", "[mp]" )
{
    const mp_scene scene = full_scene();

    mp_scene decoded;
    REQUIRE( mp_scene_apply( decoded, mp_scene_encode( scene ) ) );

    CHECK( decoded.body_version == scene.body_version );
    CHECK( decoded.frame_seq == scene.frame_seq );
    CHECK( decoded.tiles_w == scene.tiles_w );
    CHECK( decoded.tiles_h == scene.tiles_h );
    CHECK( decoded.view_dx == scene.view_dx );
    CHECK( decoded.view_dy == scene.view_dy );
    CHECK( decoded.avatar_dx == scene.avatar_dx );
    CHECK( decoded.avatar_dy == scene.avatar_dy );
    CHECK( decoded.ui_x == scene.ui_x );
    CHECK( decoded.ui_y == scene.ui_y );
    CHECK( decoded.ui_w == scene.ui_w );
    CHECK( decoded.ui_h == scene.ui_h );
    REQUIRE( decoded.masks.size() == scene.masks.size() );
    CHECK( same_vec( decoded.masks, scene.masks, same ) );
    REQUIRE( decoded.vehicles.size() == scene.vehicles.size() );
    CHECK( same_vec( decoded.vehicles, scene.vehicles, same ) );
    CHECK( decoded.ids == scene.ids );
    REQUIRE( decoded.levels.size() == scene.levels.size() );
    for( size_t i = 0; i < scene.levels.size(); ++i ) {
        CHECK( same( decoded.levels[i], scene.levels[i] ) );
    }

    // An above-player marker lives in the player's level with z_rel +1; the
    // decoder must not reject the whole frame because of it.
    REQUIRE( decoded.levels[0].creatures.size() == 2 );
    CHECK( decoded.levels[0].creatures[1].z_rel == 1 );
    CHECK( decoded.levels[0].creatures[1].flags == 0x04 );

    // The encoder, not the decoder, is what turns equal neighbouring cells
    // into one run. A scene whose terrain vector still holds two adjacent
    // repeat-1 runs of the same cell must come back as one repeat-2 run.
    mp_scene split = full_scene();
    split.levels[0].terrain = { { 1, 1, 3, 4 }, { 1, 1, 3, 4 }, { 30, 2, 2, 2 } };
    mp_scene merged;
    REQUIRE( mp_scene_apply( merged, mp_scene_encode( split ) ) );
    REQUIRE( merged.levels[0].terrain.size() == 2 );
    CHECK( merged.levels[0].terrain[0].repeat == 2 );
    CHECK( merged.levels[0].terrain[0].terrain_idx ==
           split.levels[0].terrain[0].terrain_idx );

    // A scene that never interned anything still has to encode dictionary
    // entry 0, or the first id_idx on the wire would have nothing to name.
    mp_scene bare;
    bare.tiles_w = 1;
    bare.tiles_h = 1;
    mp_scene_level bare_level;
    bare_level.terrain = { { 1, 0, 0, 0 } };
    bare.levels = { bare_level };
    mp_scene bare_decoded;
    REQUIRE( mp_scene_apply( bare_decoded, mp_scene_encode( bare ) ) );
    REQUIRE( bare_decoded.ids.size() == 1 );
    CHECK( bare_decoded.ids[0].empty() );
}

TEST_CASE( "mp_scene_decoder_rejects_malformed_bodies", "[mp]" )
{
    const minimal_body fixture = make_minimal_body();
    const std::vector<uint8_t> &valid = fixture.bytes;

    // The hand-written fixture must be a body the decoder accepts before any
    // of the corruptions below can mean anything.
    mp_scene accepted;
    REQUIRE( mp_scene_apply( accepted, valid ) );

    std::vector<std::pair<std::string, std::vector<uint8_t>>> bad;
    bad.emplace_back( "empty body", std::vector<uint8_t> {} );
    for( size_t n = 0; n < valid.size(); ++n ) {
        bad.emplace_back( "truncated at byte " + std::to_string( n ),
                          std::vector<uint8_t>( valid.begin(), valid.begin() + n ) );
    }
    bad.emplace_back( "body_version 0", with_u8( valid, 0, 0 ) );
    bad.emplace_back( "body_version 2", with_u8( valid, 0, 2 ) );
    bad.emplace_back( "tiles_w 0", with_u16( valid, fixture.tiles_w, 0 ) );
    bad.emplace_back( "tiles_h 0", with_u16( valid, fixture.tiles_h, 0 ) );
    bad.emplace_back( "id_count 0", with_u16( valid, fixture.id_count, 0 ) );
    // Index 0 carrying a name, not the empty id the contract requires.
    std::vector<uint8_t> named_first_id(
        valid.begin(), valid.begin() + fixture.first_id_len );
    put_u16( named_first_id, 1 );
    named_first_id.push_back( 'x' );
    named_first_id.insert( named_first_id.end(),
                           valid.begin() + fixture.first_id_len + 2, valid.end() );
    bad.emplace_back( "first id not empty", named_first_id );
    bad.emplace_back( "z_count 11", with_u8( valid, fixture.z_count, 11 ) );
    bad.emplace_back( "terrain covers too few cells",
                      with_u16( valid, fixture.terrain_repeat, 3 ) );
    bad.emplace_back( "repeat 0", with_u16( valid, fixture.terrain_repeat, 0 ) );
    bad.emplace_back( "mask role 2", with_u8( valid, fixture.mask_role, 2 ) );
    bad.emplace_back( "level 0 with z_rel -1",
                      with_u8( valid, fixture.level_z_rel, 0xFF ) );
    std::vector<uint8_t> trailing = valid;
    trailing.push_back( 0 );
    bad.emplace_back( "trailing byte", trailing );

    mp_scene dest = populated_destination();
    const std::vector<uint8_t> before = mp_scene_encode( dest );
    for( const std::pair<std::string, std::vector<uint8_t>> &entry : bad ) {
        INFO( entry.first );
        CHECK_FALSE( mp_scene_apply( dest, entry.second ) );
        CHECK( mp_scene_encode( dest ) == before );
    }
}

TEST_CASE( "mp_scene_decoder_rejects_out_of_range_indices", "[mp]" )
{
    const minimal_body fixture = make_minimal_body();
    const std::vector<uint8_t> &valid = fixture.bytes;

    mp_scene dest = populated_destination();
    const std::vector<uint8_t> before = mp_scene_encode( dest );

    // The fixture carries exactly two ids, so 2 == ids.size() is the first
    // out-of-range value.
    const std::vector<uint8_t> bad_id_idx =
        with_u16( valid, fixture.furniture_id_idx, 2 );
    const std::vector<uint8_t> bad_x = with_u16( valid, fixture.furniture_x, 2 );
    const std::vector<uint8_t> bad_z = with_u16( valid, fixture.creature_z_rel, 0xFFF5 );

    CHECK_FALSE( mp_scene_apply( dest, bad_id_idx ) );
    CHECK_FALSE( mp_scene_apply( dest, bad_x ) );
    CHECK_FALSE( mp_scene_apply( dest, bad_z ) );
    CHECK( mp_scene_encode( dest ) == before );
}

TEST_CASE( "mp_scene_encoder_truncates_masks_to_the_wire_limit", "[mp]" )
{
    mp_scene scene;
    scene.tiles_w = 16;
    scene.tiles_h = 16;
    scene.ui_w = 16;
    scene.ui_h = 16;
    scene.ids = { "" };
    mp_scene_level level;
    level.terrain = { { 256, 0, 0, 0 } };
    scene.levels = { level };
    for( int i = 0; i < 256; ++i ) {
        scene.masks.push_back( { static_cast<uint16_t>( i % 16 ),
                                 static_cast<uint16_t>( i / 16 ), 1, 1,
                                 static_cast<uint8_t>( i % 2 ) } );
    }

    const std::vector<uint8_t> body = mp_scene_encode( scene );
    mp_scene decoded;
    REQUIRE( mp_scene_apply( decoded, body ) );
    REQUIRE( decoded.masks.size() == 255 );
    CHECK( decoded.masks.size() == MP_SCENE_MAX_MASKS );
    for( size_t i = 0; i < decoded.masks.size(); ++i ) {
        CHECK( same( decoded.masks[i], scene.masks[i] ) );
    }
}

TEST_CASE( "mp_scene_body_hash_ignores_the_frame_prefix", "[mp]" )
{
    const mp_scene scene = full_scene();
    const std::vector<uint8_t> body = mp_scene_encode( scene );

    // Independent FNV-1a over the body from offset 5, so the test pins the
    // hash as well as the equality it is used for.
    uint64_t expected = 14695981039346656037ULL;
    for( size_t i = 5; i < body.size(); ++i ) {
        expected ^= static_cast<uint64_t>( body[i] );
        expected *= 1099511628211ULL;
    }
    CHECK( mp_scene_body_hash( body ) == expected );

    mp_scene reframed = scene;
    reframed.body_version = 2;
    reframed.frame_seq = scene.frame_seq + 1;
    const std::vector<uint8_t> reframed_body = mp_scene_encode( reframed );
    REQUIRE( reframed_body.size() == body.size() );
    REQUIRE( reframed_body[0] != body[0] );
    CHECK( mp_scene_body_hash( reframed_body ) == mp_scene_body_hash( body ) );

    mp_scene changed = scene;
    changed.levels[0].terrain[0].terrain_idx = 2;
    CHECK( mp_scene_body_hash( mp_scene_encode( changed ) ) != expected );
}

#endif // MULTIPLAYER
