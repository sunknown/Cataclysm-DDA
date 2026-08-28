#if defined(MP_SERVER)

#include "mp_session_harness.h"

#include <string>
#include <vector>

#include "avatar.h"
#include "game.h"
#include "game_test_helper.h"
#include "map_helpers_tests.h"
#include "player_helpers.h"
#include "type_id.h"
#include "mp/mp_context.h"
#include "mp/mp_loopback_transport.h"
#include "mp/mp_protocol.h"
#include "mp/mp_sched.h"
#include "mp/mp_session.h"
#include "mp/mp_turn.h"

mp_session_harness::mp_session_harness( const size_t players, const int latency_ms )
{
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    g->ensure_gamemode_for_test();
    // The first do_turn() of a session is the one-time "new_game" pass
    // (weather.on_game_start), which does not advance the calendar. Pin it here
    // so turn counting in the tests is deterministic no matter which tests ran
    // before this one in the same process.
    g->new_game = true;
    game_test_helper::reset_player_slots();
    mp_sessions_reset_for_test();

    g->player_at( 0 ).name = "P0";
    for( size_t i = 1; i < players; ++i ) {
        avatar &extra = g->add_player_slot();
        extra.set_body();
        extra.name = "P" + std::to_string( i );
    }
    // g->add_player_slot() leaves a new avatar at moves == 100, unlike a fresh
    // avatar's zero. Zero every slot here so the "fresh avatars start with zero
    // moves" turn-counting the tests rely on is actually true for every slot,
    // not just slot 0.
    for( size_t i = 0; i < players; ++i ) {
        g->player_at( i ).set_moves( 0 );
    }
    mp_sync_context_count();
    // Fresh session: no leftover barrier state from a previous harness. The
    // input-wait flags, the turn-open flag and the end-of-session request belong
    // to a session, not to a turn, so mp_turn_reset() deliberately leaves them --
    // reset_all is the session/test boundary.
    mp_turn_reset_all();

    for( size_t i = 0; i < players; ++i ) {
        // The latency is not decoration. Without it, code that only works at
        // zero round-trip time passes here and fails on the first real wire.
        mp_loopback_pair wire = mp_make_loopback_pair( latency_ms );
        auto session = std::make_unique<mp_session>();
        session->transport = wire.a;
        session->context_index = i;
        session->name = g->player_at( i ).name;
        session->grid.resize( 80, 24 );
        mp_sessions().push_back( std::move( session ) );
        client_wires_.push_back( wire.b );
        client_grids_.emplace_back( 80, 24 );
        client_states_.emplace_back();
    }

    sched_ = std::make_unique<mp_sched>( players );
    mp_set_active_sched( sched_.get() );
    for( size_t i = 0; i < players; ++i ) {
        threads_.emplace_back( [i]() {
            while( true ) {
                mp_active_sched()->wait_for_turn( i );
                if( mp_active_sched()->stopping() ) {
                    return;
                }
                mp_assert_holds( i );
                mp_activate_player( i );
                avatar &ava = g->player_at( i );
                g->avatar_turn_pre( ava );
                g->player_act( ava );
                mp_active_sched()->release_to( i, mp_sched::world );
            }
        } );
    }
}

mp_session_harness::~mp_session_harness()
{
    // Close the wires FIRST. shut_down() only wakes threads parked on the baton;
    // a player sitting in a menu is parked on recv() with no timeout, and joining
    // them before their socket goes away hangs the harness -- and every Task 13
    // test ends with a player in a menu. A closed transport makes
    // mp_server_wait_for_input return a timeout event (the loopback recv polls
    // and sees the closed peer within a millisecond), and the player_act loop
    // sees mp_sched_stopping() and unwinds.
    for( std::shared_ptr<mp_transport> &wire : client_wires_ ) {
        wire->close();
    }
    sched_->shut_down();
    for( std::thread &t : threads_ ) {
        t.join();
    }
    mp_set_active_sched( nullptr );
    mp_sessions_reset_for_test();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_turn_reset_all();
}

void mp_session_harness::press( const size_t slot, const int key )
{
    mp_control msg;
    msg.kind = mp_control_kind::input;
    msg.event = input_event( key, input_event_t::keyboard_char );
    const std::string json = mp_control_to_json( msg );
    client_wires_[slot]->send( mp_wrap( mp_msg_kind::control,
                                        std::vector<uint8_t>( json.begin(), json.end() ) ) );
}

void mp_session_harness::pump_clients()
{
    for( size_t i = 0; i < client_wires_.size(); ++i ) {
        while( const std::optional<std::vector<uint8_t>> msg = client_wires_[i]->recv( 0 ) ) {
            mp_client_apply_message( *msg, client_grids_[i], client_states_[i] );
        }
    }
}

bool mp_session_harness::run_until( const std::function<bool()> &predicate, const int max_turns )
{
    for( int turn = 0; turn < max_turns; ++turn ) {
        g->do_turn();
        pump_clients();
        if( predicate() ) {
            return true;
        }
    }
    // Returning false rather than looping forever is the point: a barrier bug
    // shows up as a failed assertion with a message, not as a hung CI job.
    return false;
}

const mp_screen_grid &mp_session_harness::screen( const size_t slot ) const
{
    return client_grids_[slot];
}

bool mp_grids_equal( const mp_screen_grid &a, const mp_screen_grid &b )
{
    if( a.width() != b.width() || a.height() != b.height() ) {
        return false;
    }
    for( int y = 0; y < a.height(); ++y ) {
        for( int x = 0; x < a.width(); ++x ) {
            if( a.at( x, y ) != b.at( x, y ) ) {
                return false;
            }
        }
    }
    return true;
}

#endif // MP_SERVER
