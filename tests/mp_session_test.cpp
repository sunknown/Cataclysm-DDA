#if defined(MP_SERVER)

#include "cata_catch.h"

#include <memory>

#include "avatar.h"
#include "game.h"
#include "game_test_helper.h"
#include "mp/mp_context.h"
#include "mp/mp_server_main.h"
#include "mp/mp_session.h"

TEST_CASE( "sessions_are_matched_to_slots_by_name", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();

    avatar &a = g->player_at( 0 );
    a.name = "Ada";
    avatar &b = g->add_player_slot();
    b.set_body();
    b.name = "Bo";
    mp_sync_context_count();

    mp_sessions_reset_for_test();
    mp_sessions().push_back( std::make_unique<mp_session>() );
    mp_sessions().push_back( std::make_unique<mp_session>() );
    // Deliberately in the opposite order to the slots: the match is by name,
    // not by who connected first, so that reconnecting in a different order
    // still puts everyone back in their own body.
    mp_sessions()[0]->name = "Bo";
    mp_sessions()[1]->name = "Ada";

    std::string err;
    REQUIRE( mp_server_start_session( err ) );
    CHECK( err.empty() );
    CHECK( mp_sessions()[0]->context_index == 1 );
    CHECK( mp_sessions()[1]->context_index == 0 );

    mp_server_end_session();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

TEST_CASE( "an_unknown_name_is_refused_with_a_reason", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    g->player_at( 0 ).name = "Ada";

    mp_sessions_reset_for_test();
    mp_sessions().push_back( std::make_unique<mp_session>() );
    mp_sessions()[0]->name = "Nobody";

    std::string err;
    CHECK_FALSE( mp_server_start_session( err ) );
    CHECK( err.find( "Nobody" ) != std::string::npos );

    mp_server_end_session();
}

#endif // MP_SERVER
