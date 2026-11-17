#include <optional>

#include "avatar.h"
#include "cata_catch.h"
#include "character.h"
#include "enums.h"
#include "game.h"
#include "game_test_helper.h"
#include "map.h"
#include "map_helpers.h"
#include "type_id.h"
#include "veh_type.h"
#include "vehicle.h"
#include "vpart_position.h"
#include "vpart_range.h"

static const vproto_id vehicle_prototype_bicycle( "bicycle" );

namespace
{
// A failed REQUIRE skips the tail of the test case; a leaked extra slot then
// outlives the test and trips the slot/context assertions later in the run.
struct slot_cleanup {
    ~slot_cleanup() {
        game_test_helper::reset_player_slots();
    }
};
} // namespace

// Vehicle code stores a passenger as a character_id and resolves it back to a
// pointer on demand (vehicle_part::passenger_id, vehicle::get_passenger()).
// The id survives a co-op slot swap, but the lookup used to see only the
// avatar currently in `u`, so a partner strapped into any seat read as empty.
TEST_CASE( "a_vehicle_seat_resolves_a_passenger_in_another_slot", "[coop][vehicle]" )
{
    game_test_helper::reset_player_slots();
    slot_cleanup cleanup;
    clear_map_without_vision();
    map &here = get_map();

    avatar &second = g->add_player_slot();
    // add_player_slot() hands out a bare avatar: no body, no id, at the world
    // origin. Production slot creation (debug_menu::toggle_coop_slot) fills
    // those in; a test must too.
    second.set_body();
    second.setID( g->assign_npc_id() );
    second.setpos( here, g->player_at( 0 ).pos_bub( here ) );
    REQUIRE( second.getID().is_valid() );
    REQUIRE( second.getID() != g->player_at( 0 ).getID() );

    // The lookup every id-holding system shares.
    CHECK( g->critter_by_id<Character>( second.getID() ) == &second );

    const tripoint_bub_ms origin( 60, 60, 0 );
    vehicle *veh = here.add_vehicle( vehicle_prototype_bicycle, origin, 0_degrees, 0,
                                     veh_spawn_status::UNDAMAGED );
    REQUIRE( veh != nullptr );

    std::optional<int> seat;
    for( const vpart_reference &vp : veh->get_avail_parts( VPFLAG_BOARDABLE ) ) {
        seat = vp.part_index();
        break;
    }
    REQUIRE( seat.has_value() );

    // Seat the partner by hand rather than through map::board_vehicle(): the
    // latter recentres the reality bubble by writing `u`, so boarding a
    // non-active avatar would move the wrong character. These are exactly the
    // two fields a real boarding writes.
    vehicle_part &part = veh->part( *seat );
    part.set_flag( vp_flag::passenger_flag );
    part.passenger_id = second.getID();

    CHECK( veh->get_passenger( *seat ) == &second );
}
