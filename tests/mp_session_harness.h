#pragma once
#ifndef CATA_TESTS_MP_SESSION_HARNESS_H
#define CATA_TESTS_MP_SESSION_HARNESS_H

#if defined(MP_SERVER)

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cata_imgui.h"          // cataimgui::client
#include "cursesdef.h"           // catacurses::window
#include "mp/mp_cell.h"
#include "mp/mp_client_screen.h"
#include "mp/mp_protocol.h"      // mp_plan_kind, mp_party_msg
#include "mp/mp_sched.h"
#include "mp/mp_transport.h"
#include "panels.h"              // panel_manager
#include "point.h"               // point

class avatar;

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
// This is the full enumeration for this build, not a summary -- there is
// nowhere else in the tree that carries it. Sites that only run at data-load
// time, and sites specific to SDL, ncurses, wincurse or sound, are excluded
// because the harness cannot reach them; everything else that the flag gates is
// below.
//
// Turned ON (behaviour the flag was suppressing):
//
//  - src/ui_manager.cpp:383   redraw_invalidated() itself.
//  - src/game.cpp:3340        game::draw() -- the whole main screen: terrain,
//                             sidebar, minimap. This is the callback
//                             create_or_get_main_ui_adaptor() registers, so it
//                             is the largest single thing the flag was hiding.
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
//  - src/output.cpp:2946      scrollingcombattext::add() really queues SCT.
//  - src/animation.cpp:340    draw_custom_explosion() really draws. Unlike the
//                             other five test_mode guards in that file, this one
//                             is NOT inside a #if defined(TILES) block -- it
//                             sits between the #endif at :335 and the #if at
//                             :354, so it compiles in a TILES=0 build. Nothing
//                             explodes in this harness, so it is unreached, but
//                             it is live.
//  - src/activity_actor.cpp:13671,13693,13734,14357, src/character.cpp:7235,
//    src/game.cpp:6659        the zone-sort viewport family starts calling
//                             g->set_zoom() and mark_main_ui_adaptor_resize();
//                             inert while players have no activity.
//
// Turned OFF (the inverted cases -- behaviour the flag was providing):
//
//  - src/character_proficiency.cpp:120  set_proficiency_practice() early-returns
//                             on !test_mode, so it becomes a no-op.
//  - src/map.cpp:8702         the overlapping-map DebugLog info line stops being
//                             emitted.
//
// So the harness exercises a far larger surface than "the redraw", and a hang is
// as plausible a new failure mode as an assertion. Two things it is NOT: five of
// the six test_mode guards in src/animation.cpp (:294, :517, :597, :631, :902)
// are inside #if defined(TILES) and so are inert in this build -- :340 above is
// the exception -- and in an MP_SERVER build debugmsg() returns before it ever
// consults test_mode (src/debug.cpp:566-577), so clearing the flag cannot make
// an error open a modal window.
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
        panel_manager prev_panels_;
};

// A whole server session inside one process: N clients on loopback transports,
// scripted keystrokes, and access to every client's screen for comparison.
//
// The latency is not optional. Without it, code that only works at zero
// round-trip time gets written and passes, and the first real session finds it.
enum class mp_harness_mode {
    // Slots are seeded directly: the session is already playing.
    playing,
    // Slots start empty and each client runs the join menu, as a real session
    // does. run_until() drives the join phase until everyone has chosen and only
    // then plays turns.
    join,
};

class mp_session_harness
{
    public:
        explicit mp_session_harness( size_t players, int latency_ms = 5,
                                     mp_harness_mode mode = mp_harness_mode::playing );
        ~mp_session_harness();

        // Sends one keystroke to the server as if player `slot` had pressed it.
        void press( size_t slot, int key );

        // Sends one mouse movement to the server as if player `slot` had moved
        // the pointer to `pos`. A real client reports the pointer's cell in the
        // same control message a keystroke travels in, so this walks the same
        // wire path the game's mouse view does.
        void mouse_move( size_t slot, const point &pos );

        // One round-control frame, encoded and wrapped exactly as a client
        // does and written to the same wire press() uses. The server's reader,
        // the seat's inbox and the world's drain are all in this path, so an
        // integration case drives the loop with what a client really sends
        // rather than handing the loop a message from its own namespace. A
        // plan's cells are the seat's own scene cells (mp_plan_msg); the
        // server translates them under that seat's context and view, and
        // recomputes the cost, so `seq` is the only other client-side value.
        void send_plan( size_t slot, uint32_t seq, mp_plan_kind kind, int x, int y );
        void send_ready( size_t slot, uint32_t seq );
        void send_cancel( size_t slot, uint32_t seq );
        void send_pass( size_t slot, uint32_t seq );

        // The newest party snapshot this client received and has not yet been
        // asked for, waiting up to `timeout_ms` for one. `pump_clients()` keeps
        // the latest body each client saw, so a caller that ran run_until()
        // first reads the broadcast of the state it just observed. Polls the
        // wire only; it never runs a turn, so an unsent broadcast times out
        // with false instead of deadlocking the world.
        bool wait_party( size_t slot, int timeout_ms, mp_party_msg &out );

        // The frame sequence of the newest scene frame this client received and
        // decoded, 0 when it has seen none. The scene stream has no grid
        // analogue, so this is how a test observes that a seat was sent a new
        // frame.
        uint32_t last_scene_frame( size_t slot ) const;

        // Waits up to `timeout_ms` for this client to receive a scene frame at
        // or past `min_frame`, polling the wire without running the world.
        // False on timeout. The scene stream is delivered a wire latency after
        // the server sends it, so a test that just observed a broadcast has to
        // wait for the frame rather than for a turn.
        bool wait_scene_frame( size_t slot, uint32_t min_frame, int timeout_ms );

        // Makes this seat's client a 3D-capable client for the rest of the
        // session, adding the capability bits a real client's hello asks for.
        // Without MP_CAP_SCENE the server never builds or sends a scene frame
        // for the seat, so a scene test has nothing to observe. Called between
        // turns, while the seat is parked.
        void set_client_caps( size_t slot, uint32_t caps );

        // Answers player `slot`'s join menu with "New character: random" and
        // confirms the creation screen -- keystrokes, not a call into the menu's
        // internals, so the path under test is the one a person takes.
        void choose_random_character( size_t slot );

        // Presses the join menu's Quit entry for `slot` -- keystrokes again, so
        // what happens is whatever the menu and the server do with a real walk
        // away from the table (the seat is freed and the server waits for a
        // replacement).
        void quit_join_menu( size_t slot );

        // Drops `slot`'s wire from the client end, as a client that closed its
        // terminal or lost its network would. The server sees a dead transport
        // and nothing else.
        void drop_client( size_t slot );

        // Whether `slot` has a client on the wire at all. False once it has
        // been dropped, whether by the server at a quit or from this end.
        bool client_connected( size_t slot ) const;

        // Whether the server still holds a session for `slot`. False from the
        // moment the server drops that client until a replacement takes the
        // seat: the roster entry is what a real server frees, not just the
        // socket.
        bool seat_occupied( size_t slot ) const;

        // Seats a replacement client in `slot`, the way a new connection would.
        // When the server still holds a session for the seat (a dead seat whose
        // client left), the newcomer is offered through the same admission path
        // a real connection takes (mp_server_accept_new_client) and its wire is
        // installed on the retained session; the next grant opens the
        // replacement menu. When the seat has no session at all (freed during
        // the join phase), a loopback pair and a fresh player thread are raised
        // for it. The count of seats does not change either way.
        void connect_replacement_client( size_t slot );

        // Offers a reconnecting client carrying the character card `save_id`
        // to the running server, through the same entry a fresh connection
        // takes after the party has started (mp_server_accept_new_client), not
        // by poking the seat's state. A fresh loopback pair with this harness's
        // latency stands in for the new process; on success its wire replaces
        // `slot`'s and the seat keeps its character. Returns false when the
        // server refused the connection -- no disconnected seat carries that
        // card -- and `refusal` then holds the reason from the `bye` it was
        // sent. Nothing in the party changes on a refusal.
        bool reconnect_client( size_t slot, const std::string &save_id,
                               std::string *refusal = nullptr );

        // Offers a client that names no character card, the way a new person
        // connects to a party already in progress: a loopback pair and a hello
        // named `username`, handed to mp_server_accept_new_client. The server
        // chooses the seat; on success this harness adopts that seat's client
        // end, so press()/screen() reach it. Returns false when no seat was
        // free, and `err` then holds the reason read from the `bye` the server
        // sent, leaving the harness and the party untouched.
        bool try_admit_new_client( const std::string &username, std::string &err );

        // Whether the join phase is over and everyone has a character placed.
        // Always true in playing mode.
        bool seated() const;

        // Runs turns until `predicate` holds or `max_turns` pass. Returns false
        // on running out of turns, so a test fails with a message rather than
        // hanging.
        bool run_until( const std::function<bool()> &predicate, int max_turns = 100 );

        // True when any of the last 1000 messages in the process-wide log
        // contains `text`. The log is one per process (src/messages.h has no
        // per-player split), so this is how a session-level announcement such as
        // a disconnect is observed: it is written once, for everybody.
        bool message_log_contains( const std::string &text ) const;

        // The last screen the server sent this client, as the client applied it.
        const mp_screen_grid &screen( size_t slot ) const;

        // Drains every client's socket and applies the deltas, so screen() is
        // current. run_until() calls it after each turn.
        void pump_clients();

    private:
        // Decodes one server frame as this client would: a scene frame updates
        // the frame sequence, a control `party` body becomes the newest
        // snapshot, and everything else goes through the ordinary client apply
        // path. Shared by pump_clients() and wait_party() so neither can
        // consume a frame differently from the other.
        void receive_client_frame( size_t slot, const std::vector<uint8_t> &payload );

        // Forgets everything this client had seen. A replacement arrives on a
        // new wire with a blank terminal: an old grid, scene sequence or party
        // body must not leak into the case that drives it.
        void reset_client_view( size_t slot );

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
        // The newest scene frame sequence each client decoded, and the newest
        // party body it received with a generation counter so wait_party()
        // hands each snapshot out once: a broadcast seen during pump_clients()
        // is not lost, and a second wait does not return it again.
        std::vector<uint32_t> client_scene_frames_;
        std::vector<mp_party_msg> client_party_;
        std::vector<uint64_t> client_party_generation_;
        std::vector<uint64_t> party_read_generation_;

        std::unique_ptr<mp_sched> sched_;
        // How long a loopback wire takes to deliver anything. A replacement
        // client needs a pair of its own, with the same latency as the first
        // ones: code that only works at zero round-trip time must fail here too.
        int latency_ms_ = 0;
        bool seated_ = true;
        // Join mode empties slot 0 for the menu to fill; the test avatar it held
        // is put back afterwards.
        std::unique_ptr<avatar> stashed_avatar_;
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
