#pragma once
#ifndef CATA_SRC_MP_MP_SERVER_MAIN_H
#define CATA_SRC_MP_MP_SERVER_MAIN_H

#include <cstddef>
#include <string>
#include <thread>
#include <vector>

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

// Assigns each session the slot whose avatar carries its hello name, so that
// reconnecting in a different order than the slots still puts everyone back
// in their own body. Does nothing else: raising the baton and the player
// threads is mp_server_start_player_threads(). Called after the world has
// loaded, because the avatars do not exist until then. Returns false with
// `err` set when a name matches no avatar in the save.
bool mp_server_start_session( std::string &err );

// Raises the baton and one thread per session, right after
// mp_server_start_session() matched every name to a slot. The threads park on
// the baton until game::do_turn() orchestrates turns among them.
void mp_server_start_player_threads();

// Stops every player thread and clears the sessions. Closes the transports
// first: shut_down() only wakes threads parked on the baton, and a player
// sitting in a menu is parked on the wire until their socket goes away.
void mp_server_end_session();

// The two halves of a session's thread lifecycle that every owner of one runs
// the same way -- the server above, and the in-process test harness -- so that
// a fix to one cannot leave the other behind.
//
// mp_run_player_thread: the whole body of player `slot`'s thread. Parks on the
// baton, runs that player's turn under their own context, hands a "session
// must end" verdict to the orchestrator, and unwinds holding the unwind right
// once the scheduler stops.
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
