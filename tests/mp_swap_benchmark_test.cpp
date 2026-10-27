#include "cata_catch.h"

#include "avatar.h"
#include "game.h"
#include "game_test_helper.h"
#include "mp/mp_context.h"

TEST_CASE( "swap_cost_is_measured", "[.][mp][benchmark]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();

    // Phase 2 left this open as risk 5: the avatar swap is three moves of a
    // whole avatar, and the split turn puts it on the hot path -- at least 2N
    // swaps per tick. If this is expensive, the fix is an accessor instead of a
    // content swap, which the Phase 2 spec already costed.
    BENCHMARK( "mp_activate_player round trip" ) {
        mp_activate_player( 1 );
        mp_activate_player( 0 );
    };

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}
