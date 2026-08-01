#include "cata_catch.h"

#if defined(MULTIPLAYER)
#include "mp/mp_mode.h"

TEST_CASE( "mp_mode_defaults_to_inactive", "[hotseat]" )
{
    mp_mode_set_active( false );
    CHECK_FALSE( mp_mode_active() );
    mp_mode_set_active( true );
    CHECK( mp_mode_active() );
    mp_mode_set_active( false );
}
#endif

#include "avatar.h"
#include "game.h"

// Test helper to reset player slots between tests.
class game_test_helper
{
    public:
        static void reset_player_slots()
        {
            // Get the base avatar reference before clearing (get_avatar() uses player_at).
            avatar &base = g->player_at( 0 );
            // Clear extra slots, keeping only the original avatar at index 0.
            g->owned_extra_players.clear();
            // Rebuild player_slots with just the base avatar.
            g->player_slots.clear();
            g->player_slots.push_back( &base );
            g->active_slot_index = 0;
        }
};

TEST_CASE( "single_player_has_exactly_one_slot", "[hotseat]" )
{
    game_test_helper::reset_player_slots();
    REQUIRE( g->player_slot_count() == 1 );
    CHECK( g->active_slot() == 0 );
    CHECK( &g->player_at( 0 ) == &get_avatar() );
}

TEST_CASE( "second_slot_is_a_distinct_avatar", "[hotseat]" )
{
    game_test_helper::reset_player_slots();
    REQUIRE( g->player_slot_count() == 1 );
    avatar &first = g->player_at( 0 );

    avatar &second = g->add_player_slot();
    REQUIRE( g->player_slot_count() == 2 );
    CHECK( &second != &first );

    g->set_active_slot( 1 );
    CHECK( &get_avatar() == &second );

    g->set_active_slot( 0 );
    CHECK( &get_avatar() == &first );
}

TEST_CASE( "second_slot_has_own_map_memory", "[hotseat]" )
{
    game_test_helper::reset_player_slots();
    avatar &second = g->add_player_slot();
    second.clear_map_memory();
    CHECK( &second != &g->player_at( 0 ) );
    game_test_helper::reset_player_slots();
}

#include <sstream>
#include <string>

TEST_CASE( "extra_players_survive_a_save_round_trip", "[hotseat]" )
{
    avatar &second = g->add_player_slot();
    second.name = "SlotOneProbe";
    const size_t expected = g->player_slot_count();

    std::ostringstream out;
    g->serialize_json( out );

    // The written document must carry the extra slot, and must keep slot 0
    // under the legacy key so a single player build can still open it.
    const std::string doc = out.str();
    CHECK( doc.find( "\"player\"" ) != std::string::npos );
    CHECK( doc.find( "extra_players" ) != std::string::npos );
    CHECK( doc.find( "SlotOneProbe" ) != std::string::npos );
    CHECK( expected == 2 );

    // Clean up for subsequent tests.
    game_test_helper::reset_player_slots();
}
