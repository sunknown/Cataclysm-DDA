#pragma once
#ifndef CATA_TESTS_MP_SESSION_HARNESS_H
#define CATA_TESTS_MP_SESSION_HARNESS_H

#if defined(MP_SERVER)

#include <cstddef>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "cata_imgui.h"          // cataimgui::client
#include "cursesdef.h"           // catacurses::window
#include "mp/mp_cell.h"
#include "mp/mp_client_screen.h"
#include "mp/mp_sched.h"
#include "mp/mp_transport.h"

// The terminal every harness client runs at. A real client reports its own size
// in the handshake; the harness picks one and uses it for the server grid, the
// client grids and the root window alike, so a blit is never clipped by a
// mismatch between the two ends.
constexpr int MP_HARNESS_COLS = 80;
constexpr int MP_HARNESS_ROWS = 24;

// The process-wide setup a real server does before the game runs and that no
// test binary does: main() sets the minimum FULL_SCREEN dimensions and calls
// catacurses::init_interface() (root window, ImGui client, colours), then
// run_game() calls game_ui::init_ui() to publish TERMX/TERMY. Without it
// TERMX/TERMY are zero and stdscr is null, and the first menu that renders lays
// itself out at a negative width and throws.
//
// It also clears test_mode, which tests/test_main.cpp sets for the whole run and
// which makes ui_adaptor::redraw_invalidated() return before a single redraw
// callback runs -- a harness that measures what each player sees has to let the
// redraw happen. In an MP_SERVER build debugmsg() returns before it ever
// consults test_mode (src/debug.cpp), so clearing it cannot make an error open a
// modal window.
//
// Everything here is a global, so it is saved and restored as a group: a test
// that runs after the harness must inherit none of it.
class mp_server_startup_for_test
{
    public:
        mp_server_startup_for_test( int cols, int rows );
        ~mp_server_startup_for_test();

        mp_server_startup_for_test( const mp_server_startup_for_test & ) = delete;
        mp_server_startup_for_test( mp_server_startup_for_test && ) = delete;
        mp_server_startup_for_test &operator=( const mp_server_startup_for_test & ) = delete;
        mp_server_startup_for_test &operator=( mp_server_startup_for_test && ) = delete;

    private:
        // The screen grid itself is not restored: every consumer of it
        // (mp_server_backend_test, mp_server_io_test) opens by calling
        // mp_server_reset_for_test() and sizing it, which is the grid's own
        // reset point.
        catacurses::window prev_stdscr_;
        bool prev_test_mode_;
        int prev_termx_;
        int prev_termy_;
        int prev_full_screen_width_;
        int prev_full_screen_height_;
        std::unique_ptr<cataimgui::client> prev_imclient_;
};

// A whole server session inside one process: N clients on loopback transports,
// scripted keystrokes, and access to every client's screen for comparison.
//
// The latency is not optional. Without it, code that only works at zero
// round-trip time gets written and passes, and the first real session finds it.
class mp_session_harness
{
    public:
        explicit mp_session_harness( size_t players, int latency_ms = 5 );
        ~mp_session_harness();

        // Sends one keystroke to the server as if player `slot` had pressed it.
        void press( size_t slot, int key );

        // Runs turns until `predicate` holds or `max_turns` pass. Returns false
        // on running out of turns, so a test fails with a message rather than
        // hanging.
        bool run_until( const std::function<bool()> &predicate, int max_turns = 100 );

        // The last screen the server sent this client, as the client applied it.
        const mp_screen_grid &screen( size_t slot ) const;

        // Drains every client's socket and applies the deltas, so screen() is
        // current. run_until() calls it after each turn.
        void pump_clients();

    private:
        // Declared first so it is destroyed last: the destructor body closes the
        // wires and joins every player thread, and restoring test_mode or the
        // ImGui client while a player thread is still unwinding would pull them
        // out from under a redraw in flight. Held by pointer because it is
        // built in the constructor body, after the leftover sessions of the
        // previous test are cleared, rather than in the initialiser list.
        //
        // One scope for the whole harness rather than one around each rendered
        // turn: a player parked in a menu stays inside player_act() across
        // do_turn() calls, so a narrower scope would be flipping process-wide
        // state that another live thread reads. Setting it before the threads
        // exist and restoring it after they are joined orders both accesses by
        // the thread lifetime instead.
        std::unique_ptr<mp_server_startup_for_test> startup_;
        std::vector<std::shared_ptr<mp_transport>> client_wires_;
        // The client side is free functions over a grid and a state struct
        // (src/mp/mp_client_screen.h), not an object, so the harness holds the
        // two the same way the real client does.
        std::vector<mp_screen_grid> client_grids_;
        std::vector<mp_client_state> client_states_;
        std::unique_ptr<mp_sched> sched_;
        std::vector<std::thread> threads_;
};

// True when two grids hold the same cells. mp_screen_grid has no comparison of
// its own -- it is a wire buffer, not a value type -- and the leak this whole
// phase guards against (one player seeing another's screen) is exactly a
// cell-for-cell equality question.
bool mp_grids_equal( const mp_screen_grid &a, const mp_screen_grid &b );

#endif // MP_SERVER

#endif // CATA_TESTS_MP_SESSION_HARNESS_H
