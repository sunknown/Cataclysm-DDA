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
