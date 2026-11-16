#pragma once
#ifndef CATA_SRC_MP_MP_SERVER_MAIN_H
#define CATA_SRC_MP_MP_SERVER_MAIN_H

#include <cstddef>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "coordinates.h"

class mp_sched;

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
// Returns 0 when the session is ready to play, non-zero on a failure the
// operator must see: the server could not start and the game must not run. The
// world the clients will play is `world_name`; the caller launches the game
// loop itself after this returns successfully.
int mp_server_main( int port, const std::string &world_name, int players );

// Opens the world named on the command line -- creating it with the default
// mods and options when there is none by that name -- seats one empty slot per
// connected client, raises the player threads, runs every client's join menu
// and places the roster. Returns false with `err` set when the world will not
// load, somebody leaves before choosing, or the roster cannot be placed; the
// caller then ends the session.
//
// Characters no longer have to exist before the server starts: everyone
// either continues as the character their card names or creates one.
bool mp_server_open_world( const std::string &world_name, std::string &err );

// Says that each of these slots' characters was pulled to the group, one line
// per character. The message log is one process-wide log, so this must not be
// called once per player: n players times m moved characters is exactly the
// duplication this function exists to prevent.
void mp_announce_moved_characters( const std::vector<size_t> &moved );

// Raises the baton and one thread per session. The threads park on the baton;
// during a join phase (mp_join_begin) each first runs its player's join menu,
// then game::do_turn() orchestrates turns among them.
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
// character card. Fails with `err` set when somebody quit or dropped, or a
// character would not load or fit.
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

// The two halves of a session's thread lifecycle that every owner of one runs
// the same way -- the server above, and the in-process test harness -- so that
// a fix to one cannot leave the other behind.
//
// mp_run_player_thread: the whole body of player `slot`'s thread. Parks on the
// baton, runs that player's join menu during a join phase and then their turns
// under their own context, hands a "session must end" verdict to the
// orchestrator, and unwinds holding the unwind right once the scheduler stops.
//
// mp_stop_player_threads: closes every session's transport, shuts `sched` down
// (null when none was raised), joins `threads` and leaves the process in the
// world phase. Must be called by the world, while it holds the baton.
void mp_run_player_thread( mp_sched &sched, size_t slot );
void mp_stop_player_threads( mp_sched *sched, std::vector<std::thread> &threads );

// Ends the server session cleanly. Called by the mp curses backend when a
// client disconnects, so the game does not spin on timeout events from a
// dead transport. Defined in main.cpp, which owns the game's exit path; this
// declaration lets the mp layer (which must not know the game's internals)
// ask the entry to tear the whole process down.
void mp_server_request_exit();

#endif // CATA_SRC_MP_MP_SERVER_MAIN_H
