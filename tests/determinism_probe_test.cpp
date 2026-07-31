#include <cstdint>

#include "cata_catch.h"
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
