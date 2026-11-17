#include "cata_catch.h"

#include <chrono>
#include <optional>
#include <sstream>
#include <string>

#include "auto_note.h"
#include "auto_pickup.h"
#include "avatar.h"
#include "calendar.h"
#include "cata_path.h"
#include "cata_utility.h"
#include "character_id.h"
#include "clzones.h"
#include "enums.h"
#include "event.h"
#include "event_bus.h"
#include "filesystem.h"
#include "game.h"
#include "game_test_helper.h"
#include "item.h"
#include "itype.h"
#include "json_loader.h"
#include "map.h"
#include "map_extras.h"
#include "memorial_logger.h"
#include "mp/mp_context.h"
#include "path_info.h"
#include "player_helpers.h"
#include "safemode_ui.h"
#include "worldfactory.h"
#include "zzip.h"

// The save format is the load-bearing change of this phase: a co-op world stops
// being "one save with extra avatars nested inside" and becomes an ordinary
// world with several character saves. These tests pin the format, not the file
// system -- game::unserialize must never be fed a document from a test: it
// replaces the shared test-game state and leaks into later tests.

TEST_CASE( "the_save_document_no_longer_nests_extra_players", "[coop]" )
{
    game_test_helper::reset_player_slots();
    mp_ensure_player_context();
    mp_sync_context_count();

    avatar &second = g->add_player_slot();
    second.name = "SlotOneProbe";
    mp_sync_context_count();
    REQUIRE( g->player_slot_count() == 2 );

    std::ostringstream out;
    g->serialize_json( out );
    const std::string doc = out.str();

    // The active character still writes under the legacy "player" key, so a
    // single-player build opens the file and sees one character.
    CHECK( doc.find( "\"player\"" ) != std::string::npos );
    // The nested array is gone: with it, "load one character" would still drag
    // somebody else's avatar along, and the load split cannot be honest.
    CHECK( doc.find( "extra_players" ) == std::string::npos );
    CHECK( doc.find( "SlotOneProbe" ) == std::string::npos );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

TEST_CASE( "each_slot_serializes_as_a_standalone_character", "[coop]" )
{
    game_test_helper::reset_player_slots();
    mp_ensure_player_context();
    mp_sync_context_count();

    const std::string slot0 = g->player_at( 0 ).name;
    avatar &second = g->add_player_slot();
    second.name = "SlotOneProbe";
    mp_sync_context_count();

    // Serializing under each context is what the per-slot save loop does: the
    // document written while slot i is active carries slot i and nobody else.
    std::string docs[2];
    for( size_t i = 0; i < 2; ++i ) {
        mp_activate_player( i );
        std::ostringstream out;
        g->serialize_json( out );
        docs[i] = out.str();
    }
    mp_activate_player( 0 );

    avatar reloaded[2];
    for( size_t i = 0; i < 2; ++i ) {
        JsonObject root = json_loader::from_string( docs[i] );
        root.read( "player", reloaded[i] );
        root.allow_omitted_members();
    }
    CHECK( reloaded[0].name == slot0 );
    CHECK( reloaded[1].name == "SlotOneProbe" );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

namespace
{

// Reads a file under the current world, or "" when it does not exist.
std::string read_world_file( const cata_path &path )
{
    std::string contents;
    read_from_file_optional( path.get_unrelative_path(), [&contents]( std::istream & fin ) {
        std::ostringstream buffer;
        buffer << fin.rdbuf();
        contents = buffer.str();
    } );
    return contents;
}

// The character save itself, compressed or not: a world may store `.sav`
// inside `<base>.sav.zzip`, and the test must read what the save wrote either
// way.
std::string read_character_save( const cata_path &base )
{
    const cata_path plain = base + ".sav";
    if( file_exist( plain ) ) {
        return read_world_file( plain );
    }
    std::string contents;
    const std::optional<zzip> packed = zzip::load( ( plain + zzip_suffix ).get_unrelative_path() );
    if( packed ) {
        read_from_zzip_optional( *packed, plain.get_unrelative_path().filename(),
        [&contents]( std::string_view sv ) {
            contents = std::string( sv );
        } );
    }
    return contents;
}

// The per-character state a save writes next to `<base>.sav`, as a reader of
// the directory sees it. Player base paths are read through PATH_INFO for the
// seat that is active, which is exactly the path the save loop uses.
struct character_files {
    std::string sav;
    std::string log;
    std::string zones;
    std::string notes;
    std::string pickup;
    std::string safemode;
    std::string playtime;

    void read( const std::string &save_id )
    {
        REQUIRE( get_avatar().get_save_id() == save_id );
        const cata_path base = PATH_INFO::player_base_save_path();
        sav = read_character_save( base );
        log = read_world_file( base + ".log" );
        zones = read_world_file( base + ".zones.json" );
        notes = read_world_file( base + ".ano.json" );
        pickup = read_world_file( base + ".apu.json" );
        safemode = read_world_file( base + ".sfm.json" );
        playtime = read_world_file( base + ".pt" );
    }
};

bool contains( const std::string &haystack, const std::string &needle )
{
    return haystack.find( needle ) != std::string::npos;
}

// Puts the shared test avatar, the calendar and the slots back however the
// test ends, including on a failed REQUIRE half-way through: a character
// loaded into slot 0 would otherwise be the next test's player.
struct round_trip_cleanup {
    avatar original = std::move( g->player_at( 0 ) );
    time_point turn = calendar::turn;

    ~round_trip_cleanup() {
        mp_activate_player( 0 );
        game_test_helper::reset_player_slots();
        g->player_at( 0 ) = std::move( original );
        mp_sync_context_count();
        calendar::turn = turn;
    }

    round_trip_cleanup() = default;
    round_trip_cleanup( const round_trip_cleanup & ) = delete;
    round_trip_cleanup &operator=( const round_trip_cleanup & ) = delete;
};

// Seeds the active seat with state that must end up in *its* files only.
void seed_seat( const std::string &mark, const item &rock )
{
    get_memorial().add( "memorial of " + mark, "memorial of " + mark );
    get_auto_pickup().add_rule( &rock, true );
    get_auto_notes_settings().set_auto_note_status( map_extra_id( "note_" + mark ), true,
            /*bCharacter=*/true );
    get_safemode().add_rule( "monster_" + mark, Creature::Attitude::HOSTILE, 1,
                             rule_state::WHITELISTED );
    zone_manager::get_manager().add( "zone_" + mark, zone_type_id( "LOOT_UNSORTED" ),
                                     faction_id( "your_followers" ), false, true,
                                     tripoint_rel_ms::zero, tripoint_rel_ms::zero );
}

// Restores one of the user's global rule files on scope exit -- writing back
// what was there, or deleting the file this test created -- so the probe rules
// below do not become the next test's global rules.
struct restore_rule_file {
    cata_path path;
    std::optional<std::string> original;

    explicit restore_rule_file( cata_path p ) : path( std::move( p ) ) {
        std::string contents;
        if( read_from_file_optional( path.get_unrelative_path(), [&contents]( std::istream & fin ) {
            std::ostringstream buffer;
            buffer << fin.rdbuf();
            contents = buffer.str();
        } ) ) {
            original = std::move( contents );
        }
    }

    ~restore_rule_file() {
        if( original ) {
            write_to_file( path, [this]( std::ostream & fout ) {
                fout << *original;
            }, "restoring a global rule file" );
        } else {
            remove_file( path );
        }
    }

    restore_rule_file( const restore_rule_file & ) = delete;
    restore_rule_file &operator=( const restore_rule_file & ) = delete;
};

} // namespace

// One event bus, one subscriber per seat log. event_bus::send() broadcasts, and
// memorial_logger::notify() filters only on the acting avatar, so each seat's
// log used to record every other seat's lines too. The guard under test is that
// a line belongs to the log get_memorial() returns, i.e. the acting seat's.
TEST_CASE( "a_memorial_line_reaches_only_the_acting_seat", "[coop][memorial]" )
{
    // Two seats, each with a log of its own subscribed to the shared bus.
    player_context seat_a;
    player_context seat_b;
    // Declared last so it is destroyed first, restoring the active context
    // before the seats above unsubscribe and die.
    restore_active_state restore;
    mp_set_active_context( &seat_a );
    memorial_logger &log_a = get_memorial();
    mp_set_active_context( &seat_b );
    memorial_logger &log_b = get_memorial();
    log_a.clear();
    log_b.clear();

    clear_avatar();
    const character_id ch = get_avatar().getID();

    // Seat A acts. Seat B's log is subscribed to the same bus and must ignore
    // the line: "the avatar did it" is true for both subscribers.
    mp_set_active_context( &seat_a );
    get_event_bus().send(
        cata::event::make<event_type::activates_artifact>( ch, "seat_probe_artifact" ) );
    CHECK( contains( log_a.dump(), "Activated the seat_probe_artifact." ) );
    CHECK_FALSE( contains( log_b.dump(), "seat_probe_artifact" ) );

    // And symmetrically: B acts, A ignores.
    mp_set_active_context( &seat_b );
    get_event_bus().send( cata::event::make<event_type::activates_mininuke>( ch ) );
    CHECK( contains( log_b.dump(), "Activated a mininuke." ) );
    CHECK_FALSE( contains( log_a.dump(), "mininuke" ) );
}

// The global auto-pickup, auto-note and safe-mode files are one per process, not
// one per character. game::load_static_data() loads them into seat 0 only, so a
// seat that joins later used to run with no global rules at all: its only other
// loader, load_character(), fills the character lists.
TEST_CASE( "a_seat_added_later_loads_the_users_global_rules", "[coop]" )
{
    restore_active_state restore;
    game_test_helper::reset_player_slots();
    mp_ensure_player_context();
    mp_sync_context_count();

    restore_rule_file restore_pickup( PATH_INFO::autopickup() );
    restore_rule_file restore_safemode( PATH_INFO::safemode() );
    restore_rule_file restore_notes( PATH_INFO::autonote() );
    write_to_file( PATH_INFO::autopickup(), []( std::ostream & fout ) {
        fout << R"([{"rule":"seat_global_probe","active":true,"exclude":false}])";
    } );
    write_to_file( PATH_INFO::safemode(), []( std::ostream & fout ) {
        fout << R"([{"rule":"seat_global_probe","active":true,"whitelist":false,)"
             R"("attitude":0,"proximity":0}])";
    } );
    write_to_file( PATH_INFO::autonote(), []( std::ostream & fout ) {
        fout << R"({"disabled":["mx_seat_global_probe"]})";
    } );

    // What load_global() yields for the same files, as the reference a seat's
    // state has to match.
    auto_pickup::player_settings pickup_probe;
    pickup_probe.load_global();
    safemode safemode_probe;
    safemode_probe.load_global();
    REQUIRE_FALSE( pickup_probe.empty() );
    REQUIRE_FALSE( safemode_probe.empty() );

    // A second seat, created the way a joining client creates one.
    avatar &second = g->add_player_slot();
    second.name = "SeatGlobalProbe";
    mp_sync_context_count();
    mp_activate_player( 1 );

    CHECK_FALSE( get_auto_pickup().empty() );
    CHECK( pickup_probe.check_item( "seat_global_probe" ) ==
           get_auto_pickup().check_item( "seat_global_probe" ) );
    CHECK_FALSE( get_safemode().empty() );
    const map_extra_id probe_note( "mx_seat_global_probe" );
    CHECK_FALSE( get_auto_notes_settings().has_auto_note_enabled( probe_note, false ) );
    // The lazy load reads globals only: this seat's character-side state is
    // still untouched, not something a character file filled in.
    CHECK_FALSE( get_auto_notes_settings().has_auto_note_enabled( probe_note, true ) );

    mp_activate_player( 0 );
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

// The per-slot save loop writes each character's own .sav and the four sidecar
// files next to it. Those five used to be one process-wide object of each kind:
// every character load replaced the previous character's, and the save then
// wrote the last-loaded character's memorial, zones and notes into every file.
// This is the round trip at file level -- two characters, saved, then read back
// from disk -- because the review's finding is a data-loss bug, not a format
// one.
TEST_CASE( "each_character_keeps_its_own_per_character_files", "[coop]" )
{
    round_trip_cleanup cleanup;
    game_test_helper::reset_player_slots();
    mp_ensure_player_context();
    mp_sync_context_count();

    // The cleanup moved the test avatar out; a fresh one avoids serializing a
    // moved-from character (whose item_locations are null), and a fresh id
    // avoids Character::setID() refusing to replace the one it had.
    g->player_at( 0 ) = avatar();
    avatar &alice = g->player_at( 0 );
    alice.name = "Alice";
    alice.set_save_id( "Alice" );
    alice.set_body();
    alice.setID( g->assign_npc_id() );
    avatar &bob = g->add_player_slot();
    bob.name = "Bob";
    bob.set_save_id( "Bob" );
    bob.set_body();
    bob.setID( g->assign_npc_id() );
    mp_sync_context_count();
    REQUIRE( alice.getID() != bob.getID() );

    // Auto pickup is seeded separately: the item has to outlive the assertions
    // on it below.
    item rock( itype_id( "rock" ) );
    mp_activate_player( 0 );
    seed_seat( "alice", rock );
    mp_activate_player( 1 );
    seed_seat( "bob", rock );

    // Each seat's state must still be its own before anything is written --
    // this is the in-game half of the bug, where a partner played with the
    // other's zones.
    CHECK( contains( get_memorial().dump(), "memorial of bob" ) );
    CHECK_FALSE( contains( get_memorial().dump(), "memorial of alice" ) );
    mp_activate_player( 0 );
    CHECK( contains( get_memorial().dump(), "memorial of alice" ) );
    CHECK_FALSE( contains( get_memorial().dump(), "memorial of bob" ) );

    REQUIRE( g->save() );

    mp_activate_player( 0 );
    character_files alice_files;
    alice_files.read( "Alice" );
    mp_activate_player( 1 );
    character_files bob_files;
    bob_files.read( "Bob" );

    // .sav: one character each, and not the other one.
    CHECK( contains( alice_files.sav, "Alice" ) );
    CHECK_FALSE( contains( alice_files.sav, "Bob" ) );
    CHECK( contains( bob_files.sav, "Bob" ) );
    CHECK_FALSE( contains( bob_files.sav, "Alice" ) );

    // .sav.log: the memorial log is the character's own.
    CHECK( contains( alice_files.log, "memorial of alice" ) );
    CHECK_FALSE( contains( alice_files.log, "memorial of bob" ) );
    CHECK( contains( bob_files.log, "memorial of bob" ) );
    CHECK_FALSE( contains( bob_files.log, "memorial of alice" ) );

    // .zones.json: personal zones are the character's own.
    CHECK( contains( alice_files.zones, "zone_alice" ) );
    CHECK_FALSE( contains( alice_files.zones, "zone_bob" ) );
    CHECK( contains( bob_files.zones, "zone_bob" ) );
    CHECK_FALSE( contains( bob_files.zones, "zone_alice" ) );

    // .ano.json: auto-note settings are the character's own.
    CHECK( contains( alice_files.notes, "note_alice" ) );
    CHECK_FALSE( contains( alice_files.notes, "note_bob" ) );
    CHECK( contains( bob_files.notes, "note_bob" ) );
    CHECK_FALSE( contains( bob_files.notes, "note_alice" ) );

    // .apu.json and .sfm.json ride the same per-seat objects, but their writers
    // decline to write at all unless the character has BOTH `<base>.sav` and
    // `<base>.sav.zzip` (src/auto_pickup.cpp:903, src/safemode_ui.cpp:787), and
    // a world stores one or the other -- an upstream bug outside this wave. The
    // per-seat property is asserted in memory instead, which is also where the
    // gameplay bug lived (a partner picking up by your rules).
    mp_activate_player( 0 );
    CHECK( get_auto_pickup().has_rule( &rock ) );
    CHECK( get_safemode().has_rule( "monster_alice", Creature::Attitude::HOSTILE ) );
    CHECK_FALSE( get_safemode().has_rule( "monster_bob", Creature::Attitude::HOSTILE ) );
    mp_activate_player( 1 );
    CHECK( get_auto_pickup().has_rule( &rock ) );
    CHECK( get_safemode().has_rule( "monster_bob", Creature::Attitude::HOSTILE ) );
    CHECK_FALSE( get_safemode().has_rule( "monster_alice", Creature::Attitude::HOSTILE ) );

    // .pt: each character's own total, not the first-loaded character's. Both
    // are non-empty and equal here (nothing played between the two writes), so
    // the property pinned is "the file exists and parses", not a guess at the
    // clock.
    CHECK_FALSE( alice_files.playtime.empty() );
    CHECK_FALSE( bob_files.playtime.empty() );

    // Now read both characters back from disk. A character-only read must not
    // roll the world's clock back (the old test's property, expressed through
    // the production loader instead of game::unserialize), and it must bring
    // that character's own per-seat state with it.
    const time_point saved_turn = calendar::turn;
    calendar::turn = saved_turn + 1_hours;

    mp_activate_player( 1 );
    REQUIRE( g->load_character( save_t::from_save_id( "Bob" ), 1,
                                /*with_game_state=*/false ) );
    CHECK( calendar::turn == saved_turn + 1_hours );
    CHECK( get_avatar().name == "Bob" );
    CHECK( contains( get_memorial().dump(), "memorial of bob" ) );
    CHECK_FALSE( contains( get_memorial().dump(), "memorial of alice" ) );
    CHECK( contains( read_world_file( PATH_INFO::player_base_save_path() + ".zones.json" ),
                     "zone_bob" ) );

    mp_activate_player( 0 );
    REQUIRE( g->load_character( save_t::from_save_id( "Alice" ), 0,
                                /*with_game_state=*/false ) );
    CHECK( calendar::turn == saved_turn + 1_hours );
    CHECK( get_avatar().name == "Alice" );
    CHECK( contains( get_memorial().dump(), "memorial of alice" ) );
    CHECK_FALSE( contains( get_memorial().dump(), "memorial of bob" ) );
}
