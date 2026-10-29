#include "cata_catch.h"

#include <sstream>
#include <string>

#include "avatar.h"
#include "game.h"
#include "game_test_helper.h"
#include "json_loader.h"
#include "mp/mp_context.h"

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
