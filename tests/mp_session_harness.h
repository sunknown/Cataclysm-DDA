#pragma once
#ifndef CATA_TESTS_MP_SESSION_HARNESS_H
#define CATA_TESTS_MP_SESSION_HARNESS_H

#if defined(MP_SERVER)

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
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
// It also clears test_mode, which tests/test_main.cpp sets for the whole run.
// The flag has to go for the pipeline to run at all -- both
// ui_adaptor::redraw_invalidated() (src/ui_manager.cpp:383) and the main-UI
// redraw callback game::draw() (src/game.cpp:3340) return on it -- but it gates
// roughly twenty other behaviours, and clearing it turns all of them on at once.
// The ones that are live on the paths this harness walks:
//
//  - src/game.cpp:3340        the whole main screen: terrain, sidebar, minimap.
//  - src/ui_manager.cpp:559   background_pane really registers a redraw that
//                             erases and repaints the shared stdscr.
//  - src/uilist.cpp:438       cata_assert( !test_mode ) -- clearing the flag is
//                             what makes uilist-based menus legal at all.
//  - src/recipe_dictionary.cpp:482  the crafting search calls handle_input(1)
//                             every 250 ms, consuming input on the player
//                             thread. This is the '&' menu.
//  - src/character_crafting.cpp:188  the recipe-subset cache is now used rather
//                             than bypassed, so crafting results depend on
//                             calendar::turn caching. Also the '&' menu.
//  - src/popup.cpp:288        query_popup::query_once() really blocks on input
//                             instead of returning ERROR at once.
//  - src/cata_utility.cpp:308,338  a failed write_to_file opens a modal popup
//                             instead of logging. The harness saves, so this is
//                             on a path it actually walks.
//  - src/game.cpp:422         a completed achievement can open a popup.
//  - src/do_turn.cpp:239      handle_key_blocking_activity() really reads input;
//                             inert only while harness players have no activity
//                             and no destination.
//  - src/loading_ui.cpp:241   show() draws into the shared stdscr and calls
//                             inp_mngr.pump_events().
//  - src/map_memory.cpp:368   load_submap() does real disk I/O instead of
//                             returning nullptr.
//  - src/character_inventory.cpp:874, src/crafting.cpp:1056,
//    src/handle_liquid.cpp:274, src/iuse.cpp:1114, src/advanced_inv.cpp:2351
//                             each stops auto-answering and opens its real
//                             interactive prompt.
//  - src/character_proficiency.cpp:120  the one inverted case:
//                             set_proficiency_practice() becomes a no-op.
//
// So the harness exercises a far larger surface than "the redraw", and a hang is
// as plausible a new failure mode as an assertion. Two things it is NOT: every
// test_mode guard in src/animation.cpp is inside #if defined(TILES) and so is
// inert in this build, and in an MP_SERVER build debugmsg() returns before it
// ever consults test_mode (src/debug.cpp:566-577), so clearing the flag cannot
// make an error open a modal window.
//
// Each of these globals is saved and restored, so a later test inherits none of
// them. That is not the whole story: mp_ensure_player_context() copies TERMX,
// TERMY and stdscr into g->contexts[0] (src/mp/mp_context.cpp:86-89), and this
// class does not own that copy. Whoever runs this startup owns re-mirroring the
// context afterwards -- see ~mp_session_harness.
//
// Precondition on the imclient save/restore: it is only sound because imclient
// is null in a test binary. Moving a live one aside would leave its ImGui
// context allocated but displaced by the next CreateContext(), and this
// destructor's DestroyContext() would then re-install a pointer to no current
// context.
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
        // Held by pointer for both ends of its life. It is built in the
        // constructor body, after the leftover sessions of the previous test are
        // cleared, so the terminal it sets up lands in the fallback grid rather
        // than in a stale session's; and it is reset explicitly in the
        // destructor body, after the threads are joined but before the context
        // is re-mirrored, because that re-mirror has to copy the restored
        // terminal rather than the harness's.
        //
        // Declared first anyway, so that a constructor that throws part-way
        // still unwinds it last.
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

// How many cells hold something other than a blank. The render pipeline either
// carries a screen or carries nothing, so "how much is on it" is the honest
// measure of whether it ran -- and it distinguishes a real frame from the one
// stray cell that a "not all spaces" check would accept.
int mp_grid_nonblank_cells( const mp_screen_grid &grid );

// True when `text` appears inside a single row. Rows are read as plain runs of
// glyphs, which is all a test needs to find a name or a label on a screen; the
// empty second column of a double-width character is passed through exactly as
// the grid stores it rather than being interpreted.
bool mp_grid_contains( const mp_screen_grid &grid, const std::string &text );

#endif // MP_SERVER

#endif // CATA_TESTS_MP_SESSION_HARNESS_H
