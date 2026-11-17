#include "avatar.h"
#include "cata_catch.h"
#include "character_id.h"
#include "game.h"
#include "game_test_helper.h"
#include "item.h"
#include "map.h"
#include "mission.h"
#include "player_helpers.h"
#include "type_id.h"

static const itype_id itype_test_rock( "test_rock" );

static const mission_type_id mission_TEST_MISSION_GOAL_CONDITION1( "TEST_MISSION_GOAL_CONDITION1" );

static const morale_type morale_feeling_good( "morale_feeling_good" );

namespace
{
// Missions live in a world-level map and avatars point into it, so a failed
// REQUIRE must still tear both down in the right order: the extra avatar first
// (its mission vectors die with it), then the base avatar's pointers, then the
// missions themselves.
struct mission_test_state {
    ~mission_test_state() {
        game_test_helper::reset_player_slots();
        get_avatar().reset_all_missions();
        mission::clear_all();
    }
};
} // namespace

// A mission is world state assigned to a character_id. The world half processes
// every mission of every slot while slot 0 is the avatar in `u`, so judging
// completion against the active avatar would complete another player's mission
// using this player's position and inventory, and pay out its reward to this
// player. Both halves are checked here: the condition and the end effect.
TEST_CASE( "a_mission_completes_for_the_character_it_is_assigned_to", "[coop][mission]" )
{
    game_test_helper::reset_player_slots();
    mission_test_state cleanup;

    avatar &first = g->player_at( 0 );
    clear_character( first, true );
    first.reset_all_missions();
    mission::clear_all();

    avatar &second = g->add_player_slot();
    // add_player_slot() hands out a bare avatar: no body, no id, at the world
    // origin. Production slot creation (debug_menu::toggle_coop_slot) fills
    // those in; a test must too.
    second.set_body();
    second.setID( g->assign_npc_id() );
    second.setpos( get_map(), first.pos_bub( get_map() ) );
    clear_character( second, true );
    second.reset_all_missions();

    REQUIRE( first.getID() != second.getID() );
    REQUIRE( !first.has_item_with( []( const item &it ) {
        return it.typeId() == itype_test_rock;
    } ) );

    mission *m = mission::reserve_new( mission_TEST_MISSION_GOAL_CONDITION1, character_id() );
    REQUIRE( m != nullptr );
    m->assign( second );
    REQUIRE( m->in_progress() );
    REQUIRE( m->get_assigned_player_id() == second.getID() );

    GIVEN( "the assigned character carries the item the mission asks for" ) {
        item rock( itype_test_rock );
        second.wield( rock );
        REQUIRE( second.has_item_with( []( const item &it ) {
            return it.typeId() == itype_test_rock;
        } ) );

        WHEN( "the world turn processes every mission" ) {
            m->process();

            THEN( "it is judged and rewarded for its owner, not the active slot" ) {
                CHECK( m->is_complete( character_id() ) );
                CHECK( second.get_completed_missions().size() == 1 );
                CHECK( second.has_morale( morale_feeling_good ) == 10 );
                CHECK( first.get_completed_missions().empty() );
                CHECK( first.has_morale( morale_feeling_good ) == 0 );
            }
        }
    }

    WHEN( "only another player carries the item" ) {
        item rock( itype_test_rock );
        first.wield( rock );
        m->process();

        THEN( "the mission stays open for its owner" ) {
            CHECK( m->in_progress() );
            CHECK( second.get_completed_missions().empty() );
            CHECK( second.has_morale( morale_feeling_good ) == 0 );
        }
    }
}
