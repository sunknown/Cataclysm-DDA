#if defined(MP_SERVER)

#include "mp/mp_server_main.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "avatar.h"
#include "cata_assert.h"
#include "cata_scope_helpers.h"
#include "debug.h"
#include "game.h"
#include "game_constants.h"
#include "map.h"
#include "messages.h"
#include "mod_manager.h"
#include "path_info.h"
#include "popup.h"
#include "string_formatter.h"
#include "translations.h"
#include "worldfactory.h"
#include "mp/mp_context.h"
#include "mp/mp_character_card.h"
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
// One thread per seat, indexed by seat. A seat whose client left during the
// join phase has its thread joined and the entry cleared, and the replacement
// client that takes the seat gets a new thread in the same place -- which is
// why this cannot be a plain vector of threads: the seats are the index, and
// they outlast the clients in them.
static std::vector<std::optional<std::thread>> g_player_threads;

// The listener the server was started with, kept for the life of the process.
// A client that leaves before the party starts frees its seat, and the server
// listens again rather than shutting down: the roster is fixed at the moment
// the party starts, not at the moment of the first connection. Nothing here
// outlives the process, and the socket closes with it.
static std::optional<mp_listener> g_listener;

// How a connection attempt ended. `no_client` is both "nobody connected within
// the budget" and "the listener could not accept at all": mp_listener::accept
// collapses those two, so the only place they can be told apart is the caller
// that knows whether a timeout was possible. A refused client is not a reason
// to end a session that is still waiting for its other seats.
enum class mp_accept_result {
    seated,
    rejected,
    no_client,
};

// How long the world waits for a post-start peer to say hello. This runs on
// the orchestrator's turn path, so it must be short: a peer that completes TCP
// and then stays silent would otherwise freeze every player for the join
// phase's whole handshake budget, once per turn.
constexpr int MP_POST_START_HANDSHAKE_MS = 250;

// Accepts one connection -- waiting up to `timeout_ms`, a negative value
// meaning as long as it takes -- and shakes hands with it: the half of a
// connection that does not depend on which seat it takes. On success `out`
// holds the session (its seat is the caller's to set) and `why` is empty;
// otherwise `why` says what happened and the client, if one arrived at all,
// has been told. `handshake_timeout_ms` is the budget for the peer's hello.
static mp_accept_result accept_client( const int timeout_ms, std::unique_ptr<mp_session> &out,
                                       std::string &why,
                                       const int handshake_timeout_ms = 5000 )
{
    std::shared_ptr<mp_transport> transport = g_listener->accept( timeout_ms );
    if( !transport ) {
        why = "accepting a client failed";
        return mp_accept_result::no_client;
    }
    mp_server_log( "connection from " + transport->peer_description() + "; shaking hands" );

    auto session = std::make_unique<mp_session>();
    session->transport = transport;
    if( !mp_server_handshake( *transport, *session, why, handshake_timeout_ms ) ) {
        why = "handshake rejected: " + why;
        return mp_accept_result::rejected;
    }
    out = std::move( session );
    return mp_accept_result::seated;
}

int mp_server_main( const int port, const std::string &world_name, const int players )
{
    mp_socket_startup();

    // Everything from here on is also the operator's console log: the server
    // has no screen of its own, and until this point it started up silently.
    mp_server_log_enable();

    std::string err;
    g_listener = mp_listener::bind( port, err );
    if( !g_listener ) {
        mp_server_log( "cannot listen on port " + std::to_string( port ) + ": " + err );
        DebugLog( D_ERROR, D_MAIN ) << "mp server: cannot listen on port " << port << ": " << err;
        return 1;
    }

    // The world is loaded later by main() from the same cli_opts it handed us;
    // name it here so an operator can see what the clients are about to get.
    // An empty name is not a menu and not a choice left to the clients:
    // run_server_session() refuses to start a session without --world and says
    // so on this log, after these clients have connected.
    mp_server_log( "listening on port " + std::to_string( port ) + " (world: " +
                   ( world_name.empty() ? "none given; --world is required" :
                     "\"" + world_name + "\"" ) +
                   ", protocol " + std::to_string( MP_PROTOCOL_VERSION ) + ")" );

    mp_server_log( "waiting for " + std::to_string( players ) + " client" +
                   ( players == 1 ? "" : "s" ) );
    for( int joined = 0; joined < players; ++joined ) {
        // A server waits for its roster as long as it takes. Connection
        // timeouts and grace periods are Phase 5, together with the rest of
        // the session lifecycle.
        std::unique_ptr<mp_session> session;
        std::string why;
        const mp_accept_result result = accept_client( -1, session, why );
        if( result != mp_accept_result::seated ) {
            mp_server_log( why );
            return 1;
        }
        session->context_index = static_cast<size_t>( joined );
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

// Seats whose death has already been recorded, indexed by seat. A corpse is
// placed once per death: the orchestrator notices a dead slot on every turn
// until it is replaced, and the seat's own thread records the same death when
// it happened inside player_act(). Cleared for a seat when its replacement is
// placed, and reset with the join phase -- the session boundary both the
// server and the in-process harness pass through.
static std::vector<bool> g_death_notified;

// Whether the join phase is running. A dropped wire means two different things
// either side of this flag: during the phase it costs that one client its seat
// while the session waits for a replacement, and once the party has started it
// ends the session for everybody (Phase 5). Set by mp_join_begin(), cleared the
// moment the poll seats the roster or gives up on it.
static bool g_join_phase = false;

bool mp_join_in_progress()
{
    return g_join_phase;
}

void mp_join_begin( const size_t players, const std::optional<tripoint_abs_ms> world_anchor )
{
    g_join_choices.assign( players, std::nullopt );
    g_join_world_anchor = world_anchor;
    g_join_phase = players > 0;
    g_death_notified.assign( players, false );
    mp_join_reset_claims();
}

static std::string name_of_slot( const size_t slot )
{
    const mp_session *const session = mp_session_of_slot( slot );
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
            // doupdate() goes nowhere on the server, and the only other senders
            // of a grid are the input waits -- which this player never reaches
            // during the join phase, because a player who has chosen is parked
            // on the baton, not on the wire. Without the flush the screen is
            // drawn and never sent, and that player keeps staring at the
            // creator's last frame until the game starts.
            mp_server_flush_screen();
        }
    }
    mp_activate_world();
}

// Sends one control message to a client. A session whose wire is already gone
// is not an error here: the message is about to have nobody to read it, which
// is the normal case for the last words of a client that left.
static void send_control( mp_session &session, const mp_control &msg )
{
    if( !session.transport || !session.transport->connected() ) {
        return;
    }
    const std::string json = mp_control_to_json( msg );
    session.transport->send( mp_wrap( mp_msg_kind::control,
                                      std::vector<uint8_t>( json.begin(), json.end() ) ) );
}

static void send_character_card( mp_session &session, const avatar &ava )
{
    const std::string &world_name = world_generator->active_world->world_name;
    mp_control msg;
    msg.kind = mp_control_kind::character_card;
    msg.character_card.world_name = world_name;
    msg.character_card.save_id = ava.get_save_id();
    msg.character_card.character_name = ava.get_name();
    send_control( session, msg );
    // The card is the seat's claim to this character: a client that started
    // with no card and created one this session persists this card, and on
    // reconnect offers this save id back. Record it on the session as well, or
    // the matcher has nothing to find and that character can never return.
    session.card_save_id = msg.character_card.save_id;
    session.card_world = world_name;
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

void mp_announce_moved_characters( const std::vector<size_t> &moved )
{
    for( const size_t slot : moved ) {
        add_msg( m_info, _( "%s found the group." ), g->player_at( slot ).get_name() );
    }
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
    const std::string refusal = mp_place_roster( is_new, moved, world_anchor );
    if( !refusal.empty() ) {
        // The phase ends with the refusal itself, so the bye every client gets
        // names the character and the reason rather than the old catch-all
        // "could not find room", which also covered a start_game() failure.
        err = "the party could not be seated: " + refusal;
        return false;
    }
    // Moving somebody's character is said out loud, once per moved character.
    // The message log is one process-wide log (src/messages.cpp), not one per
    // player, so adding it under each player's context repeated every line n
    // times and told the players who had not been moved that it was them.
    mp_activate_world();
    mp_announce_moved_characters( moved );
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
        if( mp_session *const session = mp_session_of_slot( i ) ) {
            send_character_card( *session, g->player_at( i ) );
        }
        mp_server_log( "\"" + name_of_slot( i ) + "\" plays \"" + g->player_at( i ).get_name() +
                       "\"" + ( is_new[i] ? " (new)" : "" ) );
    }
    mp_activate_world();
    return true;
}

// Takes `slot`'s client out of the session during the join phase: says goodbye,
// closes the wire, removes the session from the roster and joins the seat's
// thread, leaving the seat free for a replacement. Called by the world holding
// the baton, after that seat's thread has released it for the last time
// (mp_run_player_thread returns once its menu has answered quit), so the join
// below cannot park the world.
//
// The seat's choice is not touched here: the caller re-arms it, because the
// other seats' choices must survive. Nothing is wiped wholesale -- a phase-level
// reset would send the remaining players back to their menus.
static void drop_seat( const size_t slot )
{
    const std::string who = name_of_slot( slot );
    if( mp_session *const session = mp_session_of_slot( slot ) ) {
        // The same goodbye the session teardown sends, so a real client shows
        // why its wire is about to close instead of treating it as a drop.
        mp_control bye;
        bye.kind = mp_control_kind::bye;
        bye.bye_reason = "left the join menu";
        send_control( *session, bye );
        if( session->transport ) {
            session->transport->close();
        }
        const auto it = std::find_if( mp_sessions().begin(), mp_sessions().end(),
        [session]( const std::unique_ptr<mp_session> &s ) {
            return s.get() == session;
        } );
        mp_sessions().erase( it );
    }
    mp_server_join_player_thread( slot );
    mp_server_log( "\"" + who + "\" left the join menu; the seat is free again" );
}

mp_join_progress mp_join_poll( mp_sched &sched, std::string &err )
{
    err.clear();
    mp_activate_world();
    if( mp_session_end_requested() ) {
        // The whole session is ending -- somebody died or left the game, or the
        // server could not start. A single client dropping is not this: during
        // the join phase it costs that client its seat below.
        err = mp_session_end_reason();
        g_join_phase = false;
        return mp_join_progress::failed;
    }
    // A seat whose wire dropped during the join phase. What has to happen
    // depends on whether its player had chosen yet:
    //
    //  - still choosing: its thread is parked in a menu, which has nothing left
    //    to read. Mark the seat and its next input wait answers QUIT, exactly
    //    as Escape does, so the thread exits through the same path a quitter
    //    takes and the world drops it below when the choice arrives.
    //  - already chosen: its thread is parked on the baton (there is no menu
    //    open), so the QUIT answer reaches nobody. Left alone, seat_roster
    //    would log the dead client as playing and the first turn would end the
    //    session for everybody; the seat is woken and freed here instead, and
    //    the players still choosing keep their choices and their screens.
    bool released = false;
    bool dropped = false;
    for( size_t i = 0; i < g_join_choices.size(); ++i ) {
        const mp_session *const session = mp_session_of_slot( i );
        if( session == nullptr || ( session->transport && session->transport->connected() ) ) {
            continue;
        }
        if( g_join_choices[i] ) {
            sched.abandon_seat( i );
            drop_seat( i );
            g_join_choices[i].reset();
            dropped = true;
            released = true;
            continue;
        }
        sched.request_quit( i );
        released = true;
    }
    if( dropped ) {
        // The freed seat is back in "still choosing" on everybody's screen.
        redraw_waiting_screens();
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
        if( !g_join_choices[i] ) {
            continue;
        }
        if( g_join_choices[i]->decision == mp_join_choice::kind::quit ) {
            // The party has not started and there is nothing to simulate, so
            // the session does not end with the client: its seat is freed and
            // the server waits for a replacement. Everybody else keeps the
            // choice they made and the screen they are on.
            drop_seat( i );
            g_join_choices[i].reset();
            redraw_waiting_screens();
            released = true;
            continue;
        }
        redraw_waiting_screens();
    }
    bool anyone_choosing = false;
    for( size_t i = 0; i < g_join_choices.size(); ++i ) {
        choosing[i] = !g_join_choices[i];
        anyone_choosing = anyone_choosing || choosing[i];
    }
    if( anyone_choosing ) {
        if( moved || released ) {
            // Something changed hands or a seat is waiting to be picked up by a
            // client whose QUIT is on its way: do not sleep on it.
            return mp_join_progress::choosing;
        }
        // Somebody is reading a menu. Sleep until one of them comes back from
        // their input wait, as a waiting turn does. The declared wait spans the
        // wait itself, exactly as in do_turn: a poll that lands inside
        // wait_for_request() must already see the world waiting on purpose.
        sched.set_world_waiting( true );
        sched.wait_for_request( MP_POLL_MS, choosing );
        sched.set_world_waiting( false );
        return mp_join_progress::choosing;
    }
    sched.set_world_waiting( false );
    if( !seat_roster( err ) ) {
        g_join_phase = false;
        return mp_join_progress::failed;
    }
    // The party starts here: from this point a client dropping ends the
    // session, as it always has (Phase 5 makes it survivable).
    g_join_phase = false;
    return mp_join_progress::seated;
}

// "Connected" is a property of the wire, not of the character: a client that
// is still at its terminal counts even when its avatar is dead or away. The
// session outlives every one of them; it ends only when the last transport is
// gone.
size_t mp_server_connected_client_count()
{
    size_t count = 0;
    for( const std::unique_ptr<mp_session> &session : mp_sessions() ) {
        if( session && session->transport && session->transport->connected() ) {
            ++count;
        }
    }
    return count;
}

bool mp_server_any_client_connected()
{
    return mp_server_connected_client_count() > 0;
}

void mp_server_end_if_no_clients()
{
    if( !mp_server_any_client_connected() ) {
        mp_request_session_end( "no players connected" );
    }
}

void mp_server_mark_disconnected( const size_t slot )
{
    // A menu several frames up keeps asking for its next keystroke, and every
    // one of those trips round the input loop re-detects the same dead wire.
    // The seat being marked is the whole record of "already handled": without
    // this the message and the console line would be written once per frame.
    if( mp_player_is_disconnected( slot ) ) {
        return;
    }
    mp_session *const session = mp_session_of_slot( slot );
    // The player the others know is the character, not the terminal's name:
    // the message is about who left the table, and it is the character's name
    // the roster and the join screens use.
    const std::string who = g->player_at( slot ).get_name();
    if( session != nullptr && session->transport ) {
        // The wire is already gone; closing it makes every wait on it return at
        // once rather than sit until a timeout.
        session->transport->close();
    }
    mp_set_disconnected( slot, true );
    Messages::add_msg( string_format( _( "Player %d (%s) disconnected" ),
                                     static_cast<int>( slot ) + 1, who ) );
    const std::string length = mp_server_log_session_length();
    // Built after the close, from who is actually left: the last client must
    // not be told the session continues while the line below ends it.
    mp_server_log( "\"" + who + "\" disconnected" +
                   ( length.empty() ? std::string() : " after " + length ) +
                   ( mp_server_any_client_connected() ? "; the session continues" :
                     "; no players left; ending the session" ) );
    // The last client leaving still ends the session; anything short of that
    // leaves it running for the players who are still at their terminals.
    mp_server_end_if_no_clients();
}

bool mp_player_needs_replacement( const size_t slot )
{
    if( slot >= g->player_slot_count() || mp_player_is_disconnected( slot ) ) {
        return false;
    }
    // "Abandon this character" places the corpse and sets QUIT_SUICIDE without
    // touching hit points, so a seat owed a replacement is not always literally
    // dead: the recorded death is the per-seat fact. A seat whose client has
    // left is a free seat, not a menu -- there is nobody at the terminal to
    // choose a replacement.
    if( slot < g_death_notified.size() && g_death_notified[slot] ) {
        return true;
    }
    return g->player_at( slot ).is_dead_state();
}

void mp_server_notify_death( const size_t slot )
{
    if( slot >= g->player_slot_count() ) {
        return;
    }
    g_death_notified.resize( g->player_slot_count(), false );
    avatar &ava = g->player_at( slot );
    // The latch is process-wide but its owner is per seat. A partner's death
    // or quit must never be evidence for this one; only a dead avatar or this
    // seat's own latch is.
    const bool owns_latch = mp_player_owns_quit_latch( slot );
    if( !ava.is_dead_state() && !owns_latch ) {
        return;
    }
    if( !g_death_notified[slot] ) {
        g_death_notified[slot] = true;
        // An owner's corpse is already on the map: game::is_game_over() placed
        // it for QUIT_DIED (unless the orchestrator got there first, which the
        // per-seat fact records), and ACTION_SUICIDE placed it for
        // QUIT_SUICIDE. A notification for a *different* hp-dead seat, while
        // another seat owns the latch, still places that seat's corpse without
        // touching the owner.
        if( !owns_latch && !mp_player_corpse_placed( slot ) ) {
            if( ava.in_vehicle ) {
                get_map().unboard_vehicle( ava.pos_bub() );
            }
            ava.place_corpse( &get_map() );
            mp_note_corpse_placed( slot );
        }
        Messages::add_msg( string_format( _( "Player %d (%s) died" ),
                                         static_cast<int>( slot ) + 1, ava.get_name() ) );
        mp_server_log( "\"" + ava.get_name() + "\" died; the seat is choosing a replacement" );
    }
    // Only the owner may clear the one global latch, and every call does: the
    // seat can reach is_game_over() after the death was already recorded, so
    // left set it would make the next player_act() on ANY seat read the game as
    // over and place its own corpse. activate() is idempotent and restores the
    // log is_game_over() deactivated.
    if( owns_latch ) {
        g->uquit = QUIT_NO;
        Messages::activate();
        mp_clear_quit_latch( slot );
    }
}

void mp_run_replacement_menu( const size_t slot )
{
    mp_sched *const sched = mp_active_sched();
    cata_assert( sched != nullptr );
    cata_assert( slot < g->player_slot_count() );
    // Where the character fell. Taken before the menu builds a new avatar over
    // the old one, so the fallback anchor still names the group's position.
    const tripoint_abs_ms dead_pos = g->player_at( slot ).pos_abs();
    const std::string dead_name = g->player_at( slot ).get_name();
    while( true ) {
        const mp_join_choice choice = mp_run_join_menu( "" );
        if( sched->unwind_if_stopping( slot ) ) {
            return;
        }
        if( choice.decision != mp_join_choice::kind::created ) {
            // Quit, Escape, or a wire that died while the player was choosing:
            // the seat leaves the table exactly as a disconnect does. It stays
            // dead and disconnected -- a free seat -- until a client takes it.
            mp_server_mark_disconnected( slot );
            return;
        }
        // Anchor on somebody still playing. The corpse is not a place to put
        // the replacement; an away player is still at their terminal, so they
        // count, while a disconnected seat has nobody to anchor to. A seat in
        // its creator counts too: mp_run_join_menu() keeps it on its dead
        // character's tile, so it is as good an anchor as any living one.
        std::optional<tripoint_abs_ms> anchor;
        for( size_t i = 0; i < g->player_slot_count() && !anchor; ++i ) {
            if( i == slot || g->player_at( i ).is_dead_state() ||
                mp_player_is_disconnected( i ) ) {
                continue;
            }
            anchor = g->player_at( i ).pos_abs();
        }
        if( !anchor ) {
            anchor = dead_pos;
        }
        // A free tile is found first so that a full bubble is a refusal to
        // report rather than place_new_character()'s "build the world" path,
        // which only the first character of an empty world may take.
        avatar &fresh = g->player_at( slot );
        const std::optional<tripoint_abs_ms> spot = mp_find_free_tile_near( *anchor );
        const std::string refusal = spot ? place_new_character( fresh, spot ) :
            string_format( _( "no free tile near the group for \"%s\"" ), fresh.name );
        if( !refusal.empty() ) {
            // The player has to see why, then choose again.
            popup( refusal );
            if( sched->unwind_if_stopping( slot ) ) {
                return;
            }
            continue;
        }
        // The new character is at the keyboard by construction: the away flag
        // belonged to the character it replaced.
        mp_set_away( slot, false );
        // The seat's next death is its own: a placed replacement clears both
        // facts the last one left behind.
        mp_clear_corpse_placed( slot );
        if( slot < g_death_notified.size() ) {
            g_death_notified[slot] = false;
        }
        // The card is the seat's claim to its character, and a replacement is a
        // different character: without this the session keeps the dead one's id
        // and the client can never reclaim the character it is playing.
        if( mp_session *const session = mp_session_of_slot( slot ) ) {
            send_character_card( *session, fresh );
        }
        mp_server_log( "\"" + fresh.name + "\" replaced \"" + dead_name + "\"" );
        return;
    }
}

std::optional<size_t> mp_server_seat_for_card( const std::string &save_id )
{
    // A client with no card claims no seat. An empty id would otherwise match
    // every seat whose original hello carried no card as well, and the first
    // disconnected character would go to a stranger.
    if( save_id.empty() ) {
        return std::nullopt;
    }
    for( size_t slot = 0; slot < g->player_slot_count(); ++slot ) {
        if( !mp_player_is_disconnected( slot ) ) {
            continue;
        }
        const mp_session *const session = mp_session_of_slot( slot );
        if( session != nullptr && session->card_save_id == save_id ) {
            return slot;
        }
    }
    return std::nullopt;
}

void mp_server_reconnected( const size_t slot )
{
    mp_set_disconnected( slot, false );
    // Keys the old wire delivered before it died are still in the seat's
    // queue, and the returning client would otherwise receive them as its
    // first inputs. The terminal that pressed them is gone; the new one starts
    // with a clean queue.
    if( slot < g->contexts.size() ) {
        g->contexts[slot].input_queue.clear();
    }
    // The quit mark was latched by the input wait that noticed the loss (see
    // mp_server_mark_disconnected) and is not a session-lifetime fact. Left
    // latched, the returned player's next normal baton grant takes the quit
    // path and strands the world on its next yield; cleared here together with
    // `disconnected`, it is exactly the pair Task 3's contract requires.
    if( mp_sched *const sched = mp_active_sched() ) {
        sched->clear_quit( slot );
    }
    // The table knows the character, not the terminal's name.
    const std::string who = g->player_at( slot ).get_name();
    Messages::add_msg( string_format( _( "Player %d (%s) reconnected" ),
                                     static_cast<int>( slot ) + 1, who ) );
    mp_server_log( "\"" + who + "\" reconnected" );
}

// Moves a connecting client's fresh wire and hello fields onto a session the
// server already holds for a seat. The seat is not replaced: its context index
// -- and so its character and its thread -- is untouched, and its own game
// state is not re-created. The grid is the new terminal's, because the
// handshake sized it to what this client actually runs, and any pending resize
// belonged to the terminal that left.
static void install_client_on_seat( mp_session &seat, std::unique_ptr<mp_session> fresh )
{
    cata_assert( fresh != nullptr );
    seat.transport = std::move( fresh->transport );
    seat.grid = std::move( fresh->grid );
    seat.cursor_x = fresh->cursor_x;
    seat.cursor_y = fresh->cursor_y;
    seat.cursor_visible = fresh->cursor_visible;
    seat.cursor_changed = fresh->cursor_changed;
    seat.pending_resize = std::move( fresh->pending_resize );
    seat.name = std::move( fresh->name );
    seat.card_save_id = std::move( fresh->card_save_id );
    seat.card_world = std::move( fresh->card_world );
}

bool mp_server_accept_new_client( std::unique_ptr<mp_session> session, std::string &err )
{
    cata_assert( session != nullptr );
    err.clear();
    // 1. A card names the character it left. Only a disconnected seat keeps a
    // card, so a living connected character is never mistaken for a reconnect.
    const std::optional<size_t> card_seat = mp_server_seat_for_card( session->card_save_id );
    if( card_seat ) {
        mp_session *const retained = mp_session_of_slot( *card_seat );
        cata_assert( retained != nullptr );
        install_client_on_seat( *retained, std::move( session ) );
        mp_server_reconnected( *card_seat );
        return true;
    }
    // 2. Otherwise a seat whose dead character's client has left. Its session
    // and parked thread survive the disconnect, so the newcomer joins them
    // instead of getting a seat of its own. The seat still owes a replacement,
    // and the next baton grant opens the creation menu for it.
    const std::optional<size_t> free_seat = mp_server_free_seat_for_new_client();
    if( free_seat ) {
        mp_session *const retained = mp_session_of_slot( *free_seat );
        if( retained == nullptr ) {
            // Freed during the join phase, before any session was seated: the
            // ordinary seating path raises the seat's thread.
            mp_server_seat_client( *free_seat, std::move( session ) );
            return true;
        }
        install_client_on_seat( *retained, std::move( session ) );
        mp_set_disconnected( *free_seat, false );
        // Keys the dead client queued before its wire died and the quit mark
        // its input wait latched are not the newcomer's: both are dropped along
        // with the seat's disconnected state, exactly as a reconnect does.
        if( *free_seat < g->contexts.size() ) {
            g->contexts[*free_seat].input_queue.clear();
        }
        if( mp_sched *const sched = mp_active_sched() ) {
            sched->clear_quit( *free_seat );
        }
        // A different person sits here now, so this is not a reconnect: name
        // the newcomer, not the dead character it replaced. The replacement
        // menu sends the new character card once the newcomer has chosen.
        const std::string who = retained->name;
        Messages::add_msg( string_format( _( "Player %d (%s) joined" ),
                                         static_cast<int>( *free_seat ) + 1, who ) );
        mp_server_log( "\"" + who + "\" joined" );
        return true;
    }
    // 3. No seat: say why before closing, so the client shows a refusal rather
    // than a wire that died, and keep the seat roster exactly as it was.
    err = "no free seat";
    mp_control bye;
    bye.kind = mp_control_kind::bye;
    bye.bye_reason = err;
    const std::string peer = session->transport ? session->transport->peer_description() :
                             "a client";
    send_control( *session, bye );
    if( session->transport ) {
        session->transport->close();
    }
    mp_server_log( "refused " + peer + ": " + err );
    return false;
}

void mp_server_poll_new_clients()
{
    // The listener belongs to the server process; the in-process harness has
    // none and supplies its wires through mp_server_accept_new_client directly.
    if( !g_listener ) {
        return;
    }
    std::unique_ptr<mp_session> session;
    std::string why;
    // Short budget: this poll is on the orchestrator's turn path, and a peer
    // that is accepted but never says hello must not freeze the party. The
    // refusal is retryable -- the party is untouched and the next turn polls
    // again -- and logged once by the rejected branch below.
    switch( accept_client( 0, session, why, MP_POST_START_HANDSHAKE_MS ) ) {
        case mp_accept_result::seated:
            // A refusal ("no free seat") is the client's answer, sent from the
            // admission path itself; the party is untouched.
            mp_server_accept_new_client( std::move( session ), why );
            return;
        case mp_accept_result::rejected:
            // A stranger whose hello was refused. Say why and leave the party
            // alone; a bad handshake is not a reason to end a session.
            mp_server_log( why );
            return;
        case mp_accept_result::no_client:
            // Almost always "nobody is connecting", since the attempt does not
            // wait.
            return;
    }
}

bool mp_server_seat_is_free_for_new_client( const size_t slot )
{
    if( slot >= g->player_slot_count() || !g->player_at( slot ).is_dead_state() ) {
        return false;
    }
    // Addressed through the seat, not through the sessions vector: a
    // replacement may sit anywhere in that vector once one has connected.
    const mp_session *const session = mp_session_of_slot( slot );
    return session == nullptr || !session->transport || !session->transport->connected();
}

std::optional<size_t> mp_server_free_seat_for_new_client()
{
    for( size_t slot = 0; slot < g->player_slot_count(); ++slot ) {
        if( mp_server_seat_is_free_for_new_client( slot ) ) {
            return slot;
        }
    }
    return std::nullopt;
}

// The lowest seat with no client in it, or nullopt while every seat is taken.
// After mp_join_begin the seats are fixed, so this is the whole record of who
// has left and not yet been replaced.
static std::optional<size_t> free_seat()
{
    for( size_t i = 0; ; ++i ) {
        if( i >= g_join_choices.size() ) {
            return std::nullopt;
        }
        if( mp_session_of_slot( i ) == nullptr ) {
            return i;
        }
    }
}

// Puts a connected client in `slot`: into the roster under that seat, and with
// a thread of its own running that client's join menu. Called by the world
// holding the baton, when the seat has no thread (it was dropped and joined)
// and no session.
void mp_server_seat_client( const size_t slot, std::unique_ptr<mp_session> session )
{
    mp_sched *const sched = mp_active_sched();
    cata_assert( sched != nullptr );
    cata_assert( mp_session_of_slot( slot ) == nullptr );
    session->context_index = slot;
    const std::string who = session->name;
    mp_sessions().push_back( std::move( session ) );
    // The seat is the replacement's now: the Quit mark the last occupant left
    // behind must not cancel its menu before it has drawn one.
    sched->clear_quit( slot );
    mp_server_start_player_thread( slot );
    mp_server_log( "\"" + who + "\" took the free seat" );
    // The seat is back in the roster, so the players already on the waiting
    // screen have one more name to wait for. Without this they sit on the list
    // from before the previous occupant left.
    redraw_waiting_screens();
}

// One attempt at offering `slot` to a new connection. Not blocking: the world
// keeps polling between attempts, so the clients still in their menus are
// served while a replacement is on its way. There is no timeout to report,
// because a seat waits for as long as it takes (Phase 5 adds grace periods).
static void accept_replacement( const size_t slot )
{
    std::unique_ptr<mp_session> session;
    std::string why;
    switch( accept_client( 0, session, why ) ) {
        case mp_accept_result::seated:
            mp_server_seat_client( slot, std::move( session ) );
            return;
        case mp_accept_result::rejected:
            // A stranger whose hello was refused. Say why and keep the seat
            // free; a bad handshake is not a reason to end a session that is
            // still waiting for its other seats.
            mp_server_log( why );
            return;
        case mp_accept_result::no_client:
            // Almost always "nobody has connected yet", since the attempt does
            // not wait. A listener that has genuinely failed looks the same and
            // leaves the seat waiting as well; the blocking accepts in
            // mp_server_main() are where that is reported as fatal.
            return;
    }
}

bool mp_server_open_world( const std::string &world_name, std::string &err )
{
    err.clear();
    world_generator->init();
    // has_world() first: get_world() reports a missing world as a debugmsg, and creating it
    // from --world is a supported flow, not an error.
    WORLD *world = world_generator->has_world( world_name )
                   ? world_generator->get_world( world_name ) : nullptr;
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
        // A seat a client left is offered again before anything else: accepting
        // is the only thing that can fill it, and until it is filled the phase
        // cannot seat. The attempt itself does not block -- the world keeps
        // polling, so the other clients' menus are served while a replacement
        // is on its way.
        if( const std::optional<size_t> seat = free_seat() ) {
            accept_replacement( *seat );
        }
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
        const mp_session *const session = mp_session_of_slot( slot );
        // The card is honoured only in the world it was written in: see
        // mp_card_save_id_in_world(). A mismatch is not an error -- this client
        // merely has no character here -- so it is offered creation.
        const std::string card_save_id = session != nullptr ?
                                         mp_card_save_id_in_world( session->card_save_id,
                                                 session->card_world,
                                                 world_generator->active_world->world_name ) :
                                         std::string();
        mp_join_choice choice = mp_run_join_menu( card_save_id );
        if( sched.unwind_if_stopping( slot ) ) {
            return;
        }
        const bool quit = choice.decision == mp_join_choice::kind::quit;
        g_join_choices[slot] = std::move( choice );
        sched.release_to( slot, mp_sched::world );
        if( quit ) {
            // Nothing of this seat is left to run: the world drops the client,
            // joins this thread and offers the seat again. Returning here --
            // rather than falling into the turn loop, where the thread would
            // park on the baton -- is what lets that join complete.
            return;
        }
    }
    while( true ) {
        sched.wait_for_turn( slot );
        if( sched.unwind_if_stopping( slot ) ) {
            return;
        }
        if( mp_player_is_disconnected( slot ) ) {
            // The client is gone. There is nobody to act for, and this seat no
            // longer holds the barrier, so hand the baton straight back and
            // park for the next turn. Checked before quit_requested(): this
            // seat's own quit mark is set by the input wait that noticed the
            // loss, and taking the join phase's "the world is waiting to join
            // me" path would return holding the baton the world must get back.
            //
            // The quit mark has done its job by now -- this branch is only
            // reached once player_act() has returned, so the menu it was
            // unwinding is gone -- and it must not outlive the episode: it is
            // latched for the rest of the session otherwise, answering every
            // later input wait with QUIT and taking the quit path on the next
            // normal grant once a reconnecting client clears `disconnected`.
            // A seat taken again clears it as well; see
            // mp_server_mark_disconnected().
            sched.clear_quit( slot );
            sched.release_to( slot, mp_sched::world );
            continue;
        }
        if( mp_player_needs_replacement( slot ) ) {
            // The character died but its player is still at the terminal. Do
            // not run player_act(): game::is_game_over() latches the one global
            // quit for the whole session, and every other seat would read the
            // world as over. This branch is what the orchestrator's grant
            // reaches -- it owes the turn for exactly this -- and it either
            // returns with a fresh character or leaves the table.
            mp_assert_holds( slot );
            mp_activate_player( slot );
            // The creator draws its own screen; the main game screen must not
            // be underneath it, exactly as during the join phase: the menu and
            // the creator own the terminal until the seat has a placed
            // character. The next player_act() recreates it.
            main_ui.reset();
            mp_run_replacement_menu( slot );
            if( sched.unwind_if_stopping( slot ) ) {
                return;
            }
            sched.release_to( slot, mp_sched::world );
            continue;
        }
        if( sched.quit_requested( slot ) ) {
            // This seat was released while it waited for the party to start
            // (mp_sched::abandon_seat, after the client's wire died): there is
            // no menu to cancel and no turn to run, and the world is waiting to
            // join this thread before it offers the seat again. Return without
            // the baton -- none was handed over -- exactly as the quit path
            // above does after releasing it.
            //
            // Defence in depth: a mark that outlived the join phase (a
            // disconnect during play latches one, and a client returning to
            // that seat may clear `disconnected` without clearing it) reaches
            // this branch with a NORMAL grant. Returning then would leave the
            // baton on a dead thread and the world would block on the next
            // yield, so hand it back before leaving.
            if( sched.holds( slot ) ) {
                sched.release_to( slot, mp_sched::world );
            }
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
            // Death bookkeeping is per seat and must never be skipped because
            // the wire died first. The latch itself is process-wide, so the
            // question is whether THIS seat owns it: a partner's death or quit
            // is not this seat's verdict.
            avatar &ending = g->player_at( slot );
            const bool owns_latch = mp_player_owns_quit_latch( slot );
            // Ownership alone is not a death: QUIT_SAVED/QUIT_NOSAVED are a
            // living player leaving the table. Only a dead avatar or an owned
            // death/suicide verdict takes the replacement path.
            const bool death_verdict = g->uquit == QUIT_DIED || g->uquit == QUIT_SUICIDE;
            if( ending.is_dead_state() || ( owns_latch && death_verdict ) ) {
                // Death or "abandon this character". Record it and reset the
                // latch; the seat keeps its client and the next grant reaches
                // the replacement branch above. A wire that died under the menu
                // has already marked the seat, which notify/mark treat as a
                // no-op; the seat then stays a free seat.
                mp_server_notify_death( slot );
                if( mp_player_is_disconnected( slot ) ) {
                    // mark_disconnected() is idempotent for a seat the input
                    // wait already marked, and makes an unmarked dead wire
                    // explicit. A connected player is deliberately left
                    // connected so the replacement menu can still reach them.
                    mp_server_mark_disconnected( slot );
                }
            } else {
                // A voluntary quit leaves the table exactly like a lost wire:
                // no death mark, no corpse, no replacement menu. Only its owner
                // may release the latch: a foreign latch here would mean the
                // gate in is_game_over() failed.
                mp_server_mark_disconnected( slot );
                if( owns_latch ) {
                    g->uquit = QUIT_NO;
                    mp_clear_quit_latch( slot );
                }
            }
        }
        sched.release_to( slot, mp_sched::world );
    }
}

void mp_stop_player_threads( mp_sched *const sched )
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
    for( std::unique_ptr<mp_session> &session : mp_sessions() ) {
        if( session ) {
            send_control( *session, bye );
            if( session->transport ) {
                session->transport->close();
            }
        }
    }
    // Called from the world, which holds the baton: the precondition
    // shut_down() documents for the unwind that follows.
    if( sched ) {
        sched->shut_down();
    }
    for( std::optional<std::thread> &t : g_player_threads ) {
        if( t ) {
            t->join();
            t.reset();
        }
    }
    g_player_threads.clear();
    // The unwinders ran under their own contexts (mp_sched::unwind_if_stopping),
    // and the last one left its slot active, with its avatar swapped into g->u.
    // Nothing runs any more, so put the process back in the world phase before
    // anything the caller does next reads or reshapes the slots.
    mp_activate_world();
}

void mp_server_start_player_thread( const size_t slot )
{
    mp_sched *const sched = mp_active_sched();
    cata_assert( sched != nullptr );
    if( g_player_threads.size() <= slot ) {
        g_player_threads.resize( slot + 1 );
    }
    cata_assert( !g_player_threads[slot] );
    g_player_threads[slot].emplace( mp_run_player_thread, std::ref( *sched ), slot );
}

void mp_server_join_player_thread( const size_t slot )
{
    if( slot >= g_player_threads.size() || !g_player_threads[slot] ) {
        return;
    }
    g_player_threads[slot]->join();
    g_player_threads[slot].reset();
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
        mp_server_start_player_thread( i );
    }
}

void mp_server_end_session()
{
    mp_stop_player_threads( g_sched.get() );
    mp_set_active_sched( nullptr );
    g_sched.reset();
    g_join_phase = false;
    mp_sessions().clear();
    mp_turn_reset_all();
    // Hand main() back the process it started the session in: player 0's
    // context and stack, not the world phase the threads were stopped in.
    // main() still holds context 0's main screen and drops it at the end of
    // this loop iteration, and ~ui_adaptor asserts it dies in that stack.
    mp_activate_player( 0 );
}

#endif // MP_SERVER
