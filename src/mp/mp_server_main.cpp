#if defined(MP_SERVER)

#include "mp/mp_server_main.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "avatar.h"
#include "cata_scope_helpers.h"
#include "debug.h"
#include "game.h"
#include "game_constants.h"
#include "messages.h"
#include "mod_manager.h"
#include "path_info.h"
#include "translations.h"
#include "worldfactory.h"
#include "mp/mp_context.h"
#include "mp/mp_join_menu.h"
#include "mp/mp_placement.h"
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

// The join phase in progress: one entry per player, empty until they have
// chosen. Sized before the player threads start, and from then on only touched
// by whoever holds the baton.
static std::vector<std::optional<mp_join_choice>> g_join_choices;
static std::optional<tripoint_abs_ms> g_join_world_anchor;

void mp_join_begin( const size_t players, const std::optional<tripoint_abs_ms> world_anchor )
{
    g_join_choices.assign( players, std::nullopt );
    g_join_world_anchor = world_anchor;
    mp_join_reset_claims();
}

static mp_session *session_of_slot( const size_t slot )
{
    for( std::unique_ptr<mp_session> &session : mp_sessions() ) {
        if( session && session->context_index == slot ) {
            return session.get();
        }
    }
    return nullptr;
}

static std::string name_of_slot( const size_t slot )
{
    const mp_session *const session = session_of_slot( slot );
    return session != nullptr ? session->name : "player " + std::to_string( slot + 1 );
}

// Everyone who has chosen sees who is still choosing. Drawn by the world, under
// each of those players' contexts in turn, whenever the list changes.
static void redraw_waiting_screens()
{
    std::vector<std::string> still_choosing;
    for( size_t i = 0; i < g_join_choices.size(); ++i ) {
        if( !g_join_choices[i] ) {
            still_choosing.push_back( name_of_slot( i ) );
        }
    }
    for( size_t i = 0; i < g_join_choices.size(); ++i ) {
        if( g_join_choices[i] ) {
            mp_activate_player( i );
            mp_draw_waiting_screen( still_choosing );
        }
    }
    mp_activate_world();
}

static void send_character_card( mp_session &session, const avatar &ava )
{
    mp_control msg;
    msg.kind = mp_control_kind::character_card;
    msg.character_card.world_name = world_generator->active_world->world_name;
    msg.character_card.save_id = ava.get_save_id();
    msg.character_card.character_name = ava.get_name();
    const std::string json = mp_control_to_json( msg );
    if( session.transport ) {
        session.transport->send( mp_wrap( mp_msg_kind::control,
                                          std::vector<uint8_t>( json.begin(), json.end() ) ) );
    }
}

// The world's most recently written character save: whose game state a session
// takes when nobody continues a character in a world that has been played.
static std::optional<save_t> newest_save()
{
    std::optional<save_t> newest;
    std::filesystem::file_time_type newest_time;
    for( const save_t &save : world_generator->active_world->world_saves ) {
        const cata_path base = PATH_INFO::world_base_save_path() / ( save.base_path() +
                               SAVE_EXTENSION );
        std::error_code ec;
        std::filesystem::file_time_type when = std::filesystem::last_write_time(
                ( base + zzip_suffix ).get_unrelative_path(), ec );
        if( ec ) {
            ec.clear();
            when = std::filesystem::last_write_time( base.get_unrelative_path(), ec );
        }
        if( !ec && ( !newest || when > newest_time ) ) {
            newest = save;
            newest_time = when;
        }
    }
    return newest;
}

// Everyone has chosen: bring the continued characters in, place the roster and
// tell every client who it is. Runs on the world, holding the baton.
static bool seat_roster( std::string &err )
{
    const size_t players = g_join_choices.size();
    std::vector<bool> is_new( players, true );
    bool have_game_state = false;
    for( size_t i = 0; i < players; ++i ) {
        const mp_join_choice &choice = *g_join_choices[i];
        if( choice.decision != mp_join_choice::kind::continued ) {
            continue;
        }
        is_new[i] = false;
        mp_activate_player( i );
        // The first continued character brings the world's game state with it
        // and is the anchor; the others bring only themselves.
        mp_server_log( "loading \"" + choice.save->decoded_name() + "\"" );
        if( !g->load_character( *choice.save, i, /*with_game_state=*/!have_game_state ) ) {
            err = "loading \"" + choice.save->decoded_name() + "\" failed";
            return false;
        }
        have_game_state = true;
    }

    std::optional<tripoint_abs_ms> world_anchor = g_join_world_anchor;
    if( !have_game_state && !world_anchor ) {
        if( const std::optional<save_t> newest = newest_save() ) {
            // The world has been played, but nobody here continues in it. Its
            // clock and its map still stand, so the new characters start where
            // the world last was rather than rebuilding it.
            mp_activate_player( 0 );
            world_anchor = g->load_game_state( *newest );
            if( !world_anchor ) {
                err = "reading the world's state from \"" + newest->decoded_name() + "\" failed";
                return false;
            }
        }
    }

    std::vector<size_t> moved;
    if( !mp_place_roster( is_new, moved, world_anchor ) ) {
        err = "could not find room for everybody near the group";
        return false;
    }
    // Moving somebody's character is said out loud, to them and to everyone:
    // it is that player's save.
    for( size_t i = 0; i < players; ++i ) {
        mp_activate_player( i );
        for( const size_t slot : moved ) {
            if( slot == i ) {
                add_msg( m_info, _( "Your survivor and the others found each other." ) );
            } else {
                add_msg( m_info, _( "%s found the group." ), g->player_at( slot ).get_name() );
            }
        }
    }
    // Nobody has played a turn yet: the session starts, as single player's does,
    // with every character owing nothing. `moves` is what the barrier reads as
    // "still has a turn to play" (mp_player_state_of), and a character that has
    // just been created or loaded has a turn's worth of them, so leaving them
    // there would make the first turn wait for a first action from players whose
    // screens have not been drawn yet -- and, with no countdown armed because
    // nobody is holding anything yet, the world would never tick at all. They
    // are handed a turn's moves back by avatar_turn_post() at the end of that
    // first, closed turn, exactly as an already-played session starts.
    for( size_t i = 0; i < players; ++i ) {
        g->player_at( i ).set_moves( 0 );
    }
    // The card goes out now, not at session end: it does not change during play,
    // and sending it early means it survives the server crashing.
    for( size_t i = 0; i < players; ++i ) {
        if( mp_session *const session = session_of_slot( i ) ) {
            send_character_card( *session, g->player_at( i ) );
        }
        mp_server_log( "\"" + name_of_slot( i ) + "\" plays \"" + g->player_at( i ).get_name() +
                       "\"" + ( is_new[i] ? " (new)" : "" ) );
    }
    mp_activate_world();
    return true;
}

mp_join_progress mp_join_poll( mp_sched &sched, std::string &err )
{
    err.clear();
    mp_activate_world();
    if( mp_session_end_requested() ) {
        // A client dropped: its menu got the cancel every wait gives a dead wire.
        err = mp_session_end_reason();
        return mp_join_progress::failed;
    }
    bool moved = false;
    std::vector<bool> choosing( g_join_choices.size() );
    for( size_t i = 0; i < g_join_choices.size(); ++i ) {
        if( g_join_choices[i] || !sched.waiting_for_baton( i ) ) {
            continue;
        }
        sched.yield_to( mp_sched::world, i );
        mp_activate_world();
        moved = true;
        if( g_join_choices[i] ) {
            if( g_join_choices[i]->decision == mp_join_choice::kind::quit ) {
                err = "\"" + name_of_slot( i ) + "\" left before choosing";
                return mp_join_progress::failed;
            }
            redraw_waiting_screens();
        }
    }
    bool anyone_choosing = false;
    for( size_t i = 0; i < g_join_choices.size(); ++i ) {
        choosing[i] = !g_join_choices[i];
        anyone_choosing = anyone_choosing || choosing[i];
    }
    if( anyone_choosing ) {
        if( moved ) {
            sched.set_world_waiting( false );
        } else {
            // Somebody is reading a menu. Sleep until one of them comes back
            // from their input wait, as a waiting turn does.
            sched.wait_for_request( MP_POLL_MS, choosing );
            sched.set_world_waiting( true );
        }
        return mp_join_progress::choosing;
    }
    sched.set_world_waiting( false );
    return seat_roster( err ) ? mp_join_progress::seated : mp_join_progress::failed;
}

bool mp_server_open_world( const std::string &world_name, std::string &err )
{
    err.clear();
    world_generator->init();
    WORLD *world = world_generator->get_world( world_name );
    if( world == nullptr ) {
        // No world by that name: make one with the defaults, so a server can be
        // started on a fresh world without the game's world menus.
        world = world_generator->make_new_world( world_name,
                world_generator->get_mod_manager().get_default_mods() );
        if( world == nullptr ) {
            err = "no world named \"" + world_name + "\", and creating one failed";
            return false;
        }
        mp_server_log( "created the world \"" + world_name + "\" with the default mods" );
    }
    world_generator->set_active_world( world );
    try {
        g->setup();
    } catch( const std::exception &e ) {
        err = std::string( "setting the world up failed: " ) + e.what();
        return false;
    }
    if( !g->load_world_state() ) {
        err = "loading the world state failed";
        return false;
    }

    // Slots come after setup(), which clears them. An empty avatar is exactly
    // what character creation fills, and it is what gives the join menu a
    // context to draw into -- contexts are parallel to slots, and the menu runs
    // before any character exists.
    g->player_at( 0 ) = avatar();
    while( g->player_slot_count() < mp_sessions().size() ) {
        g->add_player_slot();
    }
    for( size_t i = 0; i < mp_sessions().size(); ++i ) {
        mp_sessions()[i]->context_index = i;
    }
    mp_server_log( "world \"" + world->world_name + "\" is open; waiting for " +
                   std::to_string( mp_sessions().size() ) + " player" +
                   ( mp_sessions().size() == 1 ? "" : "s" ) + " to choose a survivor" );

    mp_join_begin( mp_sessions().size(), std::nullopt );
    mp_server_start_player_threads();
    while( true ) {
        switch( mp_join_poll( *g_sched, err ) ) {
            case mp_join_progress::choosing:
                continue;
            case mp_join_progress::seated:
                return true;
            case mp_join_progress::failed:
                return false;
        }
    }
}

void mp_run_player_thread( mp_sched &sched, const size_t slot )
{
    // Held for the whole body, so the unwind right is handed on however the
    // thread ends -- see mp_sched::unwind_if_stopping. A no-op for a thread that
    // never took it.
    on_out_of_scope release_unwind( [slot, &sched]() {
        sched.release_unwind( slot );
    } );
    // This player's main game screen: the map and sidebar every menu sits on.
    // main() raises one, but before any player thread exists, so only for
    // context 0; without this a client of any other slot sees a blank screen
    // whenever no menu is open. Held for the whole body because the adaptor is
    // the screen's registration in this context's ui stack. It must die under
    // that same stack (~ui_adaptor asserts it), and every way out of the body
    // holds the unwind right, so nobody else is repointing the context then.
    // Declared after release_unwind so it is dropped while the right is still
    // held.
    shared_ptr_fast<ui_adaptor> main_ui;
    on_out_of_scope drop_main_ui( [slot, &main_ui]() {
        if( main_ui ) {
            mp_activate_player( slot );
            main_ui.reset();
        }
    } );
    if( slot < g_join_choices.size() && !g_join_choices[slot] ) {
        // The join phase: this player's menu, on this thread, in this context.
        // Nothing of the main screen yet -- there is no character to draw.
        sched.wait_for_turn( slot );
        if( sched.unwind_if_stopping( slot ) ) {
            return;
        }
        mp_assert_holds( slot );
        mp_activate_player( slot );
        const mp_session *const session = session_of_slot( slot );
        mp_join_choice choice = mp_run_join_menu( session != nullptr ? session->card_save_id :
                                std::string() );
        if( sched.unwind_if_stopping( slot ) ) {
            return;
        }
        g_join_choices[slot] = std::move( choice );
        sched.release_to( slot, mp_sched::world );
    }
    while( true ) {
        sched.wait_for_turn( slot );
        if( sched.unwind_if_stopping( slot ) ) {
            return;
        }
        mp_assert_holds( slot );
        mp_activate_player( slot );
        if( !main_ui ) {
            main_ui = g->create_or_get_active_main_ui_adaptor();
        }
        avatar &ava = g->player_at( slot );
        g->avatar_turn_pre( ava );
        const bool ended = g->player_act( ava );
        // A shutdown during player_act woke this thread without the baton, so
        // there is none to release, and the session is already ending without
        // a request. The right is already held by now if the wake came through
        // an input wait; taking it here covers a player_act that saw the stop
        // some other way.
        if( sched.unwind_if_stopping( slot ) ) {
            return;
        }
        if( ended ) {
            // The game is over or this player quit. A thread body has nowhere
            // to return that to, so it is left where the orchestrator picks it
            // up and turns it into do_turn()'s `true` for main().
            const avatar &ava = g->player_at( slot );
            mp_request_session_end( ava.get_name() +
                                    ( ava.is_dead_state() ? " died" : " left the game" ) );
        }
        sched.release_to( slot, mp_sched::world );
    }
}

void mp_stop_player_threads( mp_sched *const sched, std::vector<std::thread> &threads )
{
    // Close the wires FIRST. shut_down() only wakes threads parked on the
    // baton; a player sitting in a menu is parked on recv() with no timeout,
    // and joining them before their socket goes away hangs the server forever
    // -- on the one path that matters, because ending a session while somebody
    // has a menu open is the normal case, not the exotic one. A closed
    // transport makes the input wait return a timeout event, the player_act
    // loop sees mp_sched_stopping() and unwinds.
    //
    // Say goodbye first, with the reason, so a client shows why its game
    // stopped instead of a dropped connection. The world sends this while a
    // player thread may be parked reading the same transport; transports take
    // a send from any thread.
    mp_control bye;
    bye.kind = mp_control_kind::bye;
    bye.bye_reason = mp_session_end_reason().empty() ? std::string( "the session ended" ) :
                     mp_session_end_reason();
    const std::string bye_json = mp_control_to_json( bye );
    for( std::unique_ptr<mp_session> &session : mp_sessions() ) {
        if( session && session->transport ) {
            if( session->transport->connected() ) {
                session->transport->send( mp_wrap( mp_msg_kind::control,
                                                   std::vector<uint8_t>( bye_json.begin(), bye_json.end() ) ) );
            }
            session->transport->close();
        }
    }
    // Called from the world, which holds the baton: the precondition
    // shut_down() documents for the unwind that follows.
    if( sched ) {
        sched->shut_down();
    }
    for( std::thread &t : threads ) {
        t.join();
    }
    threads.clear();
    // The unwinders ran under their own contexts (mp_sched::unwind_if_stopping),
    // and the last one left its slot active, with its avatar swapped into g->u.
    // Nothing runs any more, so put the process back in the world phase before
    // anything the caller does next reads or reshapes the slots.
    mp_activate_world();
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
    mp_sched_start_watchdog( *g_sched, MP_WATCHDOG_MS );
    for( size_t i = 0; i < mp_sessions().size(); ++i ) {
        g_player_threads.emplace_back( mp_run_player_thread, std::ref( *g_sched ), i );
    }
}

void mp_server_end_session()
{
    mp_stop_player_threads( g_sched.get(), g_player_threads );
    mp_set_active_sched( nullptr );
    g_sched.reset();
    mp_sessions().clear();
    mp_turn_reset_all();
    // Hand main() back the process it started the session in: player 0's
    // context and stack, not the world phase the threads were stopped in.
    // main() still holds context 0's main screen and drops it at the end of
    // this loop iteration, and ~ui_adaptor asserts it dies in that stack.
    mp_activate_player( 0 );
}

#endif // MP_SERVER
