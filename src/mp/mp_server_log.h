#pragma once
#ifndef CATA_SRC_MP_MP_SERVER_LOG_H
#define CATA_SRC_MP_MP_SERVER_LOG_H

#include <string>

// The operator's log for the dedicated server.
//
// The server owns no terminal: its curses backend draws into a buffer that
// goes out over the wire, so the console it was launched from is free for a
// running commentary. Without one the process looks hung -- it binds a port and
// sits there with nothing on screen until a client shows up, and a failure to
// bind is invisible because the game's debug log filters D_INFO out by default
// and the process exits before anyone thinks to look for the file.
//
// Every call is a no-op until mp_server_main() enables output. That keeps the
// [mp] tests quiet: they drive the same handshake and input code, and a test
// suite should not print a session log.

void mp_server_log_enable();
bool mp_server_log_enabled();

// Writes one line to stderr as "[HH:MM:SS] mp: <line>", and mirrors it into
// the game's debug log so a saved debug.log tells the same story.
void mp_server_log( const std::string &line );

// Stamps the moment the client was accepted, so the disconnect line can say
// how long the session lasted.
void mp_server_log_mark_connected();

// How long ago that stamp was taken, as "7m39s" (or "39s" under a minute, or
// "1h07m39s" past an hour). Empty when no client was ever accepted.
std::string mp_server_log_session_length();

// Drops the connect stamp and disables output again. Only the tests need this:
// they share the process with everything else in the [mp] suite.
void mp_server_log_reset_for_test();

#endif // CATA_SRC_MP_MP_SERVER_LOG_H
