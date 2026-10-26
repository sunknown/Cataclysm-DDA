#if defined(MP_SERVER)

#include "mp/mp_server_main.h"

#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "avatar.h"
#include "cata_scope_helpers.h"
#include "debug.h"
#include "game.h"
#include "mp/mp_context.h"
#include "mp/mp_protocol.h"
#include "mp/mp_sched.h"
#include "mp/mp_server_input.h"
#include "mp/mp_server_log.h"
#include "mp/mp_server_screen.h"
#include "mp/mp_session.h"
#include "mp/mp_socket.h"
#include "mp/mp_tcp_transport.h"
#include "mp/mp_transport.h"
#include "mp/mp_turn.h"

// The baton and the player threads of the running session. File scope, because
// they outlive every function that creates them: raised when the world has
// loaded, joined in mp_server_end_session() long after whoever raised them
// returned.

static std::unique_ptr<mp_sched> g_sched;
static std::vector<std::thread> g_player_threads;

int mp_server_main( const int port, const std::string &world_name, const int players )
{
    mp_socket_startup();

    // Everything from here on is also the operator's console log: the server
    // has no screen of its own, and until this point it started up silently.
    mp_server_log_enable();

    std::string err;
    std::optional<mp_listener> listener = mp_listener::bind( port, err );
    if( !listener ) {
        mp_server_log( "cannot listen on port " + std::to_string( port ) + ": " + err );
        DebugLog( D_ERROR, D_MAIN ) << "mp server: cannot listen on port " << port << ": " << err;
        return 1;
    }

    // The world is loaded later by main() from the same cli_opts it handed us;
    // name it here so an operator can see what the client is about to get. An
    // empty name means main() will offer the world-selection menu instead.
    mp_server_log( "listening on port " + std::to_string( port ) + " (world: " +
                   ( world_name.empty() ? "chosen by the client" : "\"" + world_name + "\"" ) +
                   ", protocol " + std::to_string( MP_PROTOCOL_VERSION ) + ")" );

    mp_server_log( "waiting for " + std::to_string( players ) + " client" +
                   ( players == 1 ? "" : "s" ) );
    for( int joined = 0; joined < players; ++joined ) {
        // A server waits for its roster as long as it takes. Connection
        // timeouts and grace periods are Phase 5, together with the rest of
        // the session lifecycle.
        std::shared_ptr<mp_transport> transport = listener->accept( -1 );
        if( !transport ) {
            mp_server_log( "accepting a client failed" );
            return 1;
        }
        mp_server_log( "connection from " + transport->peer_description() + "; shaking hands" );

        auto session = std::make_unique<mp_session>();
        session->transport = transport;
        std::string reject_reason;
        if( !mp_server_handshake( *transport, *session, reject_reason ) ) {
            mp_server_log( "handshake rejected: " + reject_reason );
            return 1;
        }
        mp_sessions().push_back( std::move( session ) );
        mp_server_log( std::to_string( joined + 1 ) + " of " + std::to_string( players ) +
                       " connected" );
    }

    // From here on the input and screen modules own the wires through the sessions.
    // The curses interface is initialized by main() after this returns: it falls
    // through to the same init block as single-player, which sets the fixed
    // minimum FULL_SCREEN sizes and calls catacurses::init_interface() -- and the
    // mp curses backend sizes stdscr from the grid the handshake set up.

    return 0;
}

bool mp_server_start_session( std::string &err )
{
    err.clear();
    // The roster is fixed at connection time, so a mismatch here means either
    // fewer clients than bodies (an avatar stalls with no driver) or more
    // clients than bodies (nothing left to drive). Refuse before the name
    // loop rather than let one side of the roster go unrepresented.
    if( mp_sessions().size() != g->player_slot_count() ) {
        err = std::to_string( mp_sessions().size() ) + " clients connected but this world has "
              + std::to_string( g->player_slot_count() ) + " characters";
        return false;
    }
    for( std::unique_ptr<mp_session> &session : mp_sessions() ) {
        bool matched = false;
        for( size_t i = 0; i < g->player_slot_count(); ++i ) {
            if( g->player_at( i ).name == session->name ) {
                session->context_index = i;
                matched = true;
                break;
            }
        }
        if( !matched ) {
            // Refusing with the name in the message is the difference between
            // "it did not work" and "you typed Bob and the save has Bo".
            err = "no character named \"" + session->name + "\" in this world";
            return false;
        }
    }
    // Matching every name is not a one-to-one mapping: two clients that named
    // the same character both bind to its slot, leaving the other avatar with
    // no driver. A second driver for one body would also race its twin for
    // input, so refuse the whole roster and name the character involved.
    for( size_t i = 0; i < mp_sessions().size(); ++i ) {
        for( size_t j = i + 1; j < mp_sessions().size(); ++j ) {
            if( mp_sessions()[i]->context_index == mp_sessions()[j]->context_index ) {
                err = "two clients named \"" + mp_sessions()[i]->name + "\" for one character";
                return false;
            }
        }
    }
    return true;
}

void mp_server_start_player_threads()
{
    // Each player thread activates its own context (mp_activate_player asserts
    // it exists), and the save only ever brought the avatars: align the runtime
    // contexts with the slots before any thread runs.
    mp_sync_context_count();
    // Nothing of a previous session's turn survives into this one: the barrier
    // flags, the open-turn flag and the end request are all about a session
    // that no longer exists.
    mp_turn_reset_all();
    g_sched = std::make_unique<mp_sched>( mp_sessions().size() );
    mp_set_active_sched( g_sched.get() );
    mp_sched_start_watchdog( *g_sched, 30000 );
    for( size_t i = 0; i < mp_sessions().size(); ++i ) {
        g_player_threads.emplace_back( [i, sched = g_sched.get()]() {
            // Held for the whole body, so the unwind right is handed on however
            // the thread ends -- see mp_sched::unwind_if_stopping. A no-op for a
            // thread that never took it.
            on_out_of_scope release_unwind( [i, sched]() {
                sched->release_unwind( i );
            } );
            while( true ) {
                sched->wait_for_turn( i );
                if( sched->unwind_if_stopping( i ) ) {
                    return;
                }
                mp_assert_holds( i );
                mp_activate_player( i );
                avatar &ava = g->player_at( i );
                g->avatar_turn_pre( ava );
                const bool ended = g->player_act( ava );
                // A shutdown during player_act woke this thread without the
                // baton, so there is none to release, and the session is
                // already ending without a request. The right is already held
                // by now if the wake came through an input wait; taking it here
                // covers a player_act that saw the stop some other way.
                if( sched->unwind_if_stopping( i ) ) {
                    return;
                }
                if( ended ) {
                    // The game is over or this player quit. A thread body has
                    // nowhere to return that to, so it is left where the
                    // orchestrator picks it up and turns it into do_turn()'s
                    // `true` for main().
                    mp_request_session_end();
                }
                sched->release_to( i, mp_sched::world );
            }
        } );
    }
}

void mp_server_end_session()
{
    // Close the wires FIRST. shut_down() only wakes threads parked on the
    // baton; a player sitting in a menu is parked on recv() with no timeout,
    // and joining them before their socket goes away hangs the server forever
    // -- on the one path that matters, because ending a session while somebody
    // has a menu open is the normal case, not the exotic one. A closed
    // transport makes mp_server_wait_for_input return a timeout event, the
    // player_act loop sees mp_sched_stopping() and unwinds.
    for( std::unique_ptr<mp_session> &session : mp_sessions() ) {
        if( session->transport ) {
            session->transport->close();
        }
    }
    if( g_sched ) {
        g_sched->shut_down();
    }
    for( std::thread &t : g_player_threads ) {
        t.join();
    }
    // The unwinders ran under their own contexts (mp_sched::unwind_if_stopping),
    // and the last one left its slot active, with its avatar swapped into g->u.
    // Nothing runs any more, so put the process back in the world phase before
    // anything below reads or reshapes the slots.
    mp_activate_world();
    g_player_threads.clear();
    mp_set_active_sched( nullptr );
    g_sched.reset();
    mp_sessions().clear();
    mp_turn_reset_all();
}

#endif // MP_SERVER
