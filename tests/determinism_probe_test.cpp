#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "calendar.h"
#include "cata_catch.h"
#include "determinism_probe.h"
#include "rng.h"

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
