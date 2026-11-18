#pragma once
#ifndef CATA_SRC_MP_MP_SERVER_MAIN_H
#define CATA_SRC_MP_MP_SERVER_MAIN_H

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "coordinates.h"

class mp_sched;
class mp_session;

// How long the baton may sit still with one holder before the watchdog logs
// it. Generous: it exists to catch a missed handoff, not a slow turn.
constexpr int MP_WATCHDOG_MS = 30000;

// Binds a TCP listener on `port`, waits for `players` clients, performs the
// connection handshake with every one of them, and attaches each client's
// transport to its session. The roster is fixed: the server waits for exactly
// this many clients, and once play starts nobody joins and nobody leaves --
// joining mid-session is Phase 5. Each handshake sizes the screen grid to that
// client's terminal; the caller (main()) then initializes the curses interface,
// which the mp curses backend builds from those grids.
//
// The listener outlives this function. A client that quits before the party
// starts frees its seat, and the server listens again for a replacement: the
// roster is fixed when the party starts, not when the first client connects.
// The socket is closed when the process ends.
//
// Returns 0 when the session is ready to play, non-zero on a failure the
// operator must see: the server could not start and the game must not run. The
// world the clients will play is `world_name`; the caller launches the game
// loop itself after this returns successfully.
int mp_server_main( int port, const std::string &world_name, int players );

// Opens the world named on the command line -- creating it with the default
// mods and options when there is none by that name -- seats one empty slot per
// connected client, raises the player threads, runs every client's join menu
// and places the roster. Returns false with `err` set when the world will not
// load or the roster cannot be placed; the caller then ends the session.
//
// A client that leaves during the join phase is not a failure: its seat is
// freed, the world stays loaded and the server keeps listening for a
// replacement in that seat, while everyone else keeps the choice they made.
// Only a roster that can be seated ends this.
//
// Characters no longer have to exist before the server starts: everyone
// either continues as the character their card names or creates one.
bool mp_server_open_world( const std::string &world_name, std::string &err );

// Whether the join phase is running. A dropped wire means two different
// things either side of this flag: during the phase it costs that one client
// its seat while the session waits for a replacement, and once the party has
// started it marks that seat disconnected and play goes on for everyone still
// at a terminal (the session itself ends only when the last client is gone).
bool mp_join_in_progress();

// Says that each of these slots' characters was pulled to the group, one line
// per character. The message log is one process-wide log, so this must not be
// called once per player: n players times m moved characters is exactly the
// duplication this function exists to prevent.
void mp_announce_moved_characters( const std::vector<size_t> &moved );

// Raises the baton and one thread per session. The threads park on the baton;
// during a join phase (mp_join_begin) each first runs its player's join menu,
// then game::do_turn() orchestrates turns among them. A seat taken again after
// its client left gets its thread from mp_server_start_player_thread().
void mp_server_start_player_threads();

// The join phase, shared by the server and the in-process test harness.
//
// mp_join_begin: called before the player threads start, so that each runs its
// player's join menu first. `world_anchor` is where new characters go when
// nobody continues one, for a caller that has placed the bubble itself (the
// harness); nullopt for the server, which finds it in the world's saves.
//
// mp_join_poll: one poll of the join phase by the world, which holds the baton.
// Hands the baton to every player still choosing who is ready for it, or sleeps
// one poll interval when none is; there is no barrier and no turn yet. Once the
// last player has chosen it loads the continued characters, places everyone
// (mp_place_roster), tells anybody who was moved, and sends every client its
// character card.
//
// A player who quits in the menu -- by the Quit entry, Escape, or a dropped
// wire -- costs only that client its seat: its transport is closed, its session
// leaves the roster, its thread is joined and the seat's choice is re-armed for
// a replacement, while everyone else keeps the choice they made. When the last
// one quits the phase simply keeps waiting, so the server never exits because a
// client left before the party started. Fails with `err` set only when a
// character would not load or the roster would not fit.
enum class mp_join_progress {
    choosing,
    seated,
    failed,
};
void mp_join_begin( size_t players, std::optional<tripoint_abs_ms> world_anchor );
mp_join_progress mp_join_poll( mp_sched &sched, std::string &err );

// Stops every player thread and clears the sessions. Closes the transports
// first: shut_down() only wakes threads parked on the baton, and a player
// sitting in a menu is parked on the wire until their socket goes away.
void mp_server_end_session();

// The halves of a session's thread lifecycle that every owner of one runs the
// same way -- the server above, and the in-process test harness -- so that a
// fix to one cannot leave the other behind.
//
// mp_run_player_thread: the whole body of player `slot`'s thread. Parks on the
// baton, runs that player's join menu during a join phase and then their turns
// under their own context, hands a "session must end" verdict to the
// orchestrator, and unwinds holding the unwind right once the scheduler stops.
// A player whose menu answered quit leaves instead, so the world can join it.
//
// mp_start_player_thread / mp_join_player_thread: raise and then wait for one
// seat's thread. The seats are the index, so a thread can be raised for a seat
// whose last client left, and joined on its own without touching the others.
// The caller supplies the scheduler through mp_set_active_sched().
//
// mp_server_seat_client: puts a connected client into a free seat -- into the
// roster under that seat and with a thread of its own running its join menu.
//
// mp_stop_player_threads: closes every session's transport, shuts `sched` down
// (null when none was raised), joins every seat's thread and leaves the process
// in the world phase. Must be called by the world, while it holds the baton.
void mp_run_player_thread( mp_sched &sched, size_t slot );
void mp_server_start_player_thread( size_t slot );
void mp_server_join_player_thread( size_t slot );
void mp_server_seat_client( size_t slot, std::unique_ptr<mp_session> session );
void mp_stop_player_threads( mp_sched *sched );

// How many clients the session has on the wire. A client counts while its
// transport is connected, whether its character is alive, away or dead: the
// session lives as long as the people at the terminals, not as long as the
// characters they play. A session whose client has left but whose entry has
// not yet been dropped does not count.
size_t mp_server_connected_client_count();
bool mp_server_any_client_connected();

// Ends the session when no client is connected any more; does nothing while at
// least one remains. The end request carries "no players connected" as its
// reason. Called by mp_server_mark_disconnected() once it has dropped the
// client that was lost.
void mp_server_end_if_no_clients();

// Takes `slot`'s client out of play because its wire is gone: closes the
// session's transport, marks the seat disconnected (so it stops holding the
// barrier and banks no moves) and announces it in the shared message log and on
// the server console. The session keeps running for whoever else is still
// connected; when this was the last client, mp_server_end_if_no_clients() ends
// it.
//
// Idempotent: a second call for a seat that is already disconnected does
// nothing. The input wait that notices a dead wire re-enters on every frame of
// whatever menu the player had open, so without that this would write the
// message and the console line once per frame.
//
// A seat marked here leaves the scheduler's per-seat quit mark set: the call
// site latches it (mp_sched::request_quit) so the open menu unwinds as if
// Escape had been pressed. That mark is not a session-lifetime fact. Any path
// that seats a client in this slot again must call mp_sched::clear_quit( slot )
// together with mp_set_disconnected( slot, false ); otherwise every input wait
// of the returned player answers QUIT and the next normal baton grant takes
// mp_run_player_thread()'s quit path. That thread also clears the mark itself
// once the disconnected seat's player_act() has returned, so a session that
// never reconnects the seat is left clean as well.
void mp_server_mark_disconnected( size_t slot );

// Whether `slot` is a real seat whose connected player is owed a replacement:
// its character died, or "abandon this character" recorded a death (which sets
// no hit points to zero), and its client is still there. A seat whose client
// has left is a free seat (see mp_server_seat_is_free_for_new_client), not a
// menu: there is nobody at the terminal to choose one. Checked by the
// orchestrator to hand the seat the baton and by its thread to decide what to
// do with it.
bool mp_player_needs_replacement( size_t slot );

// Records `slot`'s death: unboards a dying driver, places the corpse exactly
// once, and clears the single-player quit state and message-log switch a death
// or "abandon this character" inside the character's own turn left behind.
// Idempotent per death: a corpse is placed once however many times the
// orchestrator notices the dead slot, while a still-latched quit is cleared on
// every call. The seat's thread is released by the world's next baton grant,
// which mp_player_needs_replacement() makes it owe.
void mp_server_notify_death( size_t slot );

// Runs the replacement menu for `slot`, in that player's own context and
// thread, then places the new character next to the group. `mp_run_join_menu`
// is reused unchanged: an empty character card leaves out its Continue entry.
// The seat holds the baton on entry. A player who quits -- or whose client
// drops -- leaves the table like any other disconnect; a refused placement is
// reported and the menu repeats.
void mp_run_replacement_menu( size_t slot );

// Whether `slot` can take a new client: it is a real seat, its character is
// dead, and no connected client still occupies it. A living character's seat
// is not free -- the player is still at the terminal -- and a dead character
// whose client is still connected is not free either, because the client has
// not left yet.
bool mp_server_seat_is_free_for_new_client( size_t slot );

// The first seat that can take a new client -- see
// mp_server_seat_is_free_for_new_client() -- or nullopt while every seat is
// taken. Never grows the roster and never returns a connected living seat; the
// seats were fixed when the party started.
std::optional<size_t> mp_server_free_seat_for_new_client();

// The first disconnected seat whose session offered `save_id` as its character
// card, or nullopt when no seat is waiting for that character. A seat is
// reclaimed only by the card it was seated with: the character, its name and
// its thread must not change, so a connection that matches nothing is not a
// replacement for anybody.
std::optional<size_t> mp_server_seat_for_card( const std::string &save_id );

// Returns `slot`'s client to the table. `slot` must be a disconnected seat --
// mp_server_seat_for_card() is how a caller finds one. Call after the
// reconnecting client's new transport has been installed on the seat's
// retained session (see mp_server_accept_new_client): clears the seat's
// disconnected state, clears the scheduler's quit mark that marking it left
// latched (Task 3's contract: without it the returned player's every input wait
// answers QUIT), and writes `Player %d (%s) reconnected` to the shared message
// log. The seat keeps its character, its session and its thread; it is runnable
// again from the next turn on.
void mp_server_reconnected( size_t slot );

// Takes one freshly handshaken connection into a session that has already
// started. Admission is decided in this order:
//
// 1. A card naming a disconnected seat returns that seat: the new transport and
//    the client's own hello fields are installed on the retained session, whose
//    character, thread and context stay as they were, and
//    mp_server_reconnected() clears the seat's disconnected state.
// 2. Otherwise the first seat whose dead character's client has left -- see
//    mp_server_free_seat_for_new_client() -- takes the newcomer. Its retained
//    session and parked thread are kept: the newcomer's wire and hello fields
//    are installed on them, the seat stops being disconnected and the next
//    baton grant opens the replacement menu because the seat still owes a
//    character. The shared message log says `Player %d (%s) joined`.
// 3. Otherwise the connection is refused: it is sent a `bye` of `err` ("no free
//    seat") and closed, and the roster stays exactly as it was. A client naming
//    a living connected character matches neither a card nor a free seat and is
//    refused this way.
//
// Returns true when the connection was seated, false otherwise, with `err`
// holding the reason in that case. Must be called by the world, holding the
// baton; the seat it returns to is parked, not running.
bool mp_server_accept_new_client( std::unique_ptr<mp_session> session, std::string &err );

// One non-blocking poll for a connection after the party has started: accepts
// at most one client and hands it to mp_server_accept_new_client(). Called once
// per orchestrator turn by do_turn(). Does nothing when no listener was bound --
// the in-process test harness supplies its wires through
// mp_server_accept_new_client directly.
void mp_server_poll_new_clients();

// Ends the server session cleanly. Called by the mp curses backend when a
// client disconnects, so the game does not spin on timeout events from a
// dead transport. Defined in main.cpp, which owns the game's exit path; this
// declaration lets the mp layer (which must not know the game's internals)
// ask the entry to tear the whole process down.
void mp_server_request_exit();

#endif // CATA_SRC_MP_MP_SERVER_MAIN_H
