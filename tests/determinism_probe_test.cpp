#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "avatar.h"
#include "calendar.h"
#include "cata_catch.h"
#include "determinism_probe.h"
#include "game.h"
#include "map_helpers_tests.h"
#include "options_helpers.h"
#include "player_helpers.h"
#include "rng.h"
#include "type_id.h"

static const trait_id trait_HEAVYSLEEPER2( "HEAVYSLEEPER2" );

TEST_CASE( "rng_call_counter_increments", "[.][determinism_probe]" )
{
    rng_reset_call_count();
    REQUIRE( rng_call_count() == 0 );

    rng( 0, 100 );
    CHECK( rng_call_count() == 1 );

    rng( 0, 100 );
    rng_bits();
    CHECK( rng_call_count() == 3 );

    rng_reset_call_count();
    CHECK( rng_call_count() == 0 );

    // Diagnostics must be able to inspect the engine without disturbing the
    // counter they report, otherwise the probe measures itself.
    rng_peek_engine();
    rng_peek_engine();
    CHECK( rng_call_count() == 0 );
}

TEST_CASE( "fnv1a_is_order_sensitive", "[.][determinism_probe]" )
{
    const uint64_t seed = 1469598103934665603ULL;
    CHECK( fnv1a_u64( fnv1a_u64( seed, 1 ), 2 ) != fnv1a_u64( fnv1a_u64( seed, 2 ), 1 ) );
    CHECK( fnv1a_u64( seed, 7 ) == fnv1a_u64( seed, 7 ) );
}

TEST_CASE( "sample_world_state_reports_rng_counter", "[.][determinism_probe]" )
{
    rng_reset_call_count();
    rng( 0, 10 );
    rng( 0, 10 );

    const determinism_sample s = sample_world_state();
    CHECK( s.rng_calls == 2 );
    CHECK( s.turn == to_turn<int>( calendar::turn ) );

    // Sampling must not perturb what it measures, and an unchanged world must
    // hash identically. This is the whole premise of the probe: if two samples
    // of a world nobody touched disagree, every later comparison is noise.
    const determinism_sample again = sample_world_state();
    CHECK( again.rng_calls == s.rng_calls );
    CHECK( again.rng_state == s.rng_state );
    CHECK( again.creature_hash == s.creature_hash );
    CHECK( again.player_hash == s.player_hash );
    CHECK( again.combined == s.combined );
}

TEST_CASE( "write_sample_log_round_trips", "[.][determinism_probe]" )
{
    const std::string path = "determinism_probe_roundtrip.csv";
    std::vector<determinism_sample> samples;
    samples.push_back( determinism_sample{ 1, 2, 3, 4, 5, 6 } );
    samples.push_back( determinism_sample{ 7, 8, 9, 10, 11, 12 } );

    write_sample_log( path, samples );

    std::ifstream in( path );
    REQUIRE( in.good() );
    std::string header;
    std::getline( in, header );
    CHECK( header == "turn,rng_calls,rng_state,creature_hash,player_hash,combined" );

    std::string first;
    std::getline( in, first );
    CHECK( first == "1,2,3,4,5,6" );

    std::string second;
    std::getline( in, second );
    CHECK( second == "7,8,9,10,11,12" );

    in.close();
    std::remove( path.c_str() );
}

TEST_CASE( "do_turn_runs_headless_for_sleeping_avatar", "[.][determinism_probe]" )
{
    const override_option no_autosave( "AUTOSAVE", "false" );

    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    // The test harness's global game init (tests/test_main.cpp) never calls
    // game::start_game() or the save-load path, so game::gamemode is left
    // null. Real do_turn() unconditionally dereferences it after the first
    // turn, so satisfy that precondition without pulling in start_game()'s
    // other side effects (RNG reseed, calendar reset, etc).
    g->ensure_gamemode_for_test();

    avatar &u = get_avatar();
    u.add_effect( efftype_id( "sleep" ), 10_hours );
    REQUIRE( u.has_effect( efftype_id( "sleep" ) ) );

    // The first do_turn() call ever made in a process is a one-time
    // "new_game" setup pass (src/do_turn.cpp: weather.on_game_start()), not a
    // turn advance; calendar::turn only starts advancing from the second
    // call onward. In real gameplay this happens invisibly before the player
    // gets control. Consume that one-time pass here so the call below is a
    // genuine turn advance.
    g->do_turn();

    const int before = to_turn<int>( calendar::turn );
    g->do_turn();
    CHECK( to_turn<int>( calendar::turn ) == before + 1 );
}

TEST_CASE( "determinism_probe_500_turns", "[.][determinism_probe]" )
{
    const override_option no_autosave( "AUTOSAVE", "false" );

    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    // See do_turn_runs_headless_for_sleeping_avatar for why this is needed:
    // the test harness never calls game::start_game()/the save-load path, so
    // game::gamemode is left null, and do_turn() dereferences it
    // unconditionally after the first turn.
    g->ensure_gamemode_for_test();

    avatar &u = get_avatar();
    // The avatar must reliably stay asleep for all 500 turns despite the
    // combat noise from the crowd below (sounds::process_sound_markers()
    // runs every turn, right before the sleep check in do_turn(), so nothing
    // short of raising the wake-up threshold survives it). HEAVYSLEEPER2 is
    // a real, ordinary game trait ("You could probably sleep through a
    // firefight") that raises the noise-based wake roll from dice(2,15) to
    // dice(6,15) against heard volume (src/sounds.cpp) - still probabilistic,
    // not a hard guarantee, and not a code change.
    u.toggle_trait( trait_HEAVYSLEEPER2 );
    u.add_effect( efftype_id( "sleep" ), 10_hours );

    // A crowd is required: a single monster has too few decisions to expose
    // ordering nondeterminism. These positions are fixed, not random.
    for( int i = 0; i < 12; ++i ) {
        const tripoint_bub_ms where( 40 + ( i % 4 ) * 3, 40 + ( i / 4 ) * 3, 0 );
        spawn_test_monster( "mon_zombie", where );
    }
    spawn_npc( { 45, 50 }, "thug" );
    spawn_npc( { 55, 50 }, "thug" );

    // The first do_turn() call ever made in a process is a one-time
    // "new_game" setup pass (src/do_turn.cpp: weather.on_game_start()), not a
    // turn advance. Consume it here, before the RNG counter is reset, so all
    // 500 loop iterations below are genuine turn-advancing calls and the
    // warm-up call's own RNG usage does not pollute the measured count.
    g->do_turn();

    rng_reset_call_count();

    std::vector<determinism_sample> samples;
    samples.reserve( 500 );
    for( int i = 0; i < 500; ++i ) {
        // Re-apply sleep before each turn: the avatar can wake from combat noise
        // or ambient light (this test runs outdoors on t_pavement), and without
        // re-application do_turn() would take the real input-handling branch.
        // This is purely a test-setup measure — it does not change game behavior,
        // only ensures the probe's headless invariant (avatar stays asleep) holds
        // for the full 500 turns.
        u.add_effect( efftype_id( "sleep" ), 10_hours );
        g->do_turn();
        samples.push_back( sample_world_state() );
    }

    const char *env = std::getenv( "CDDA_DETERMINISM_LOG" );
    write_sample_log( env ? env : "determinism_probe.csv", samples );

    CHECK( samples.size() == 500 );
    // The last sample must show RNG activity, otherwise the scenario is inert
    // and the probe would report a meaningless clean result.
    CHECK( samples.back().rng_calls > 0 );
}
