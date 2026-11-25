#if defined(MP_SERVER)

#include "mp_session_harness.h"

#include "cata_catch.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "avatar.h"
#include "cached_options.h"
#include "cursesdef.h"
#include "filesystem.h"
#include "game.h"
#include "game_constants.h"
#include "game_test_helper.h"
#include "game_ui.h"
#include "input_enums.h"
#include "map.h"
#include "map_helpers.h"
#include "map_helpers_tests.h"
#include "messages.h"
#include "output.h"
#include "player_helpers.h"
#include "type_id.h"
#include "ui_manager.h"
#include "worldfactory.h"
#include "mp/mp_context.h"
#include "mp/mp_loopback_transport.h"
#include "mp/mp_protocol.h"
#include "mp/mp_scene.h"
#include "mp/mp_scene_codec.h"
#include "mp/mp_sched.h"
#include "mp/mp_server_input.h"
#include "mp/mp_server_main.h"
#include "mp/mp_server_screen.h"
#include "mp/mp_session.h"
#include "mp/mp_plan.h"
#include "mp/mp_turn.h"

namespace
{
// The reason a refused connection was given, or empty when the server closed
// the wire without saying anything. The `bye` is sent before the close, so it
// is still queued on the client's side when this drains it.
std::string read_bye_reason( mp_transport &transport, const int timeout_ms )
{
    const std::optional<std::vector<uint8_t>> msg = transport.recv( timeout_ms );
    if( !msg ) {
        return std::string();
    }
    mp_msg_kind kind = mp_msg_kind::control;
    std::vector<uint8_t> body;
    if( !mp_unwrap( *msg, kind, body ) || kind != mp_msg_kind::control ) {
        return std::string();
    }
    mp_control control;
    if( !mp_control_from_json( std::string( body.begin(), body.end() ), control ) ) {
        return std::string();
    }
    return control.kind == mp_control_kind::bye ? control.bye_reason : std::string();
}

// Erases every character save in the active world, from disk and from the
// world's save list. A join harness creates its characters through the join
// menu's random creator, whose names come from the game RNG, and the test
// listener reseeds that RNG to the same `--rng-seed` before every test section.
// Two harnesses can therefore roll the same name, and the second one then meets
// the first one's save: name_is_free() refuses it, settle_name() opens "Choose
// another name:", and the seat -- which has nobody to type into that prompt --
// never joins. Each harness owns the test world, so starting it without any
// survivor an earlier harness left behind is the fix. Characters this harness
// creates are untouched and still save under their own names, which is what its
// reconnect and reclaim paths look up.
//
// The test world is made fresh per run and dropped on success (tests/test_main.cpp),
// so what this removes is this run's earlier harnesses, not a user's game. A
// save-persistence test can still inspect what its own harness wrote: the sweep
// runs at the next harness's start, never at the writing harness's end.
void discard_earlier_world_saves()
{
    WORLD *const world = world_generator->active_world;
    if( world == nullptr ) {
        return;
    }
    for( const save_t &save : world->world_saves ) {
        // The save loop names every file it writes under the save id's base64
        // name (`.sav`, `.sav.zzip`, `.log` and the rest), so this prefix sweep
        // collects those; it is the same prefix game::move_save_to_graveyard()
        // uses. It does not reach map memory, a `<base>.mm1/` directory whose
        // removal fails and whose contents stay, nor the diary
        // `<id>_diary.json`, which does not match the prefix at all. Those are
        // left to the per-run world deletion.
        const std::string prefix = save.base_path() + ".";
        for( const cata_path &file : get_files_from_path( prefix, world->folder_path() ) ) {
            // Best effort. A file that cannot be removed must not fail a test,
            // and the list is cleared below whether or not every file went away:
            // save_exists() reads the list, not the disk, so a surviving file
            // must not make the next harness see a taken name.
            remove_file( file );
        }
    }
    world->world_saves.clear();
}
} // namespace

mp_server_startup_for_test::mp_server_startup_for_test( const int cols, const int rows )
    : prev_stdscr_( catacurses::stdscr ), prev_test_mode_( test_mode ), prev_termx_( TERMX ),
      prev_termy_( TERMY ), prev_full_screen_width_( FULL_SCREEN_WIDTH ),
      prev_full_screen_height_( FULL_SCREEN_HEIGHT ), prev_imclient_( std::move( imclient ) ),
      prev_panels_( panel_manager::get_manager() )
{
    // The same order main() and run_game() use. The handshake sizes the grid
    // before init_interface(), which reads it back to size the root window; with
    // no session attached that is the fallback grid, which is why the write side
    // of mp_server_blit_row() has to reach the same grid the read side does.
    mp_server_set_terminal_size( cols, rows );
    FULL_SCREEN_WIDTH = EVEN_MINIMUM_TERM_WIDTH;
    FULL_SCREEN_HEIGHT = EVEN_MINIMUM_TERM_HEIGHT;
    catacurses::init_interface();
    game_ui::init_ui();
    // game::setup() does this once the world's JSON is loaded; the test binary
    // never runs setup(), so without it the sidebar has no layout to draw and
    // panel_manager::get_current_layout() recurses until the stack runs out.
    panel_manager::get_manager().init();
    test_mode = false;
}

mp_server_startup_for_test::~mp_server_startup_for_test()
{
    test_mode = prev_test_mode_;
    panel_manager::get_manager() = prev_panels_;
    imclient = std::move( prev_imclient_ );
    FULL_SCREEN_HEIGHT = prev_full_screen_height_;
    FULL_SCREEN_WIDTH = prev_full_screen_width_;
    TERMY = prev_termy_;
    TERMX = prev_termx_;
    catacurses::stdscr = prev_stdscr_;
}

mp_session_harness::mp_session_harness( const size_t players, const int latency_ms,
                                        const mp_harness_mode mode )
{
    latency_ms_ = latency_ms;
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    // build_test_map() repaints terrain and leaves the creatures the test world
    // generated. A hostile that can see a player arms the combat hurry
    // countdown on its own (mp_tick_hurry), and a session whose turns can close
    // without anyone hurrying cannot show what a hurry does.
    clear_creatures();
    // clear_creatures() only removes monsters. A scene test that spawns an NPC
    // leaves it in the world; the next harness's world turns then reach that
    // NPC and open a dialogue the server backend was never asked to draw.
    clear_npcs();
    g->ensure_gamemode_for_test();
    // The first do_turn() of a session is the one-time "new_game" pass
    // (weather.on_game_start), which does not advance the calendar. Pin it here
    // so turn counting in the tests is deterministic no matter which tests ran
    // before this one in the same process.
    g->new_game = true;
    game_test_helper::reset_player_slots();
    mp_sessions_reset_for_test();
    // The join-phase characters live in the world, not only in the sessions
    // reset above: their saves outlast the harness that made them, and the
    // next harness's random creator can roll the same name. Start from a world
    // with no earlier harness's survivors.
    discard_earlier_world_saves();

    // Sessions are gone, so the terminal set up below lands in the fallback
    // grid rather than in a leftover session's. From here on the harness is a
    // running server: the screen has a size, a root window and an ImGui client,
    // and redraws are no longer gated off.
    startup_ = std::make_unique<mp_server_startup_for_test>( MP_HARNESS_COLS, MP_HARNESS_ROWS );
    // Again, now that TERMX/TERMY and stdscr are real: the first call above ran
    // before there was a terminal and left context 0 mirroring zeroes. This is
    // the order run_game() uses -- init_ui() first, then the context.
    mp_ensure_player_context();

    const bool join = mode == mp_harness_mode::join;
    // Where the test map put P0: in join mode, where the new characters go, as
    // a played world's state would place them on a real server.
    const tripoint_abs_ms world_anchor = g->player_at( 0 ).pos_abs();
    if( join ) {
        // Every slot starts as the empty avatar the join menu fills. The test
        // avatar slot 0 held is kept aside and put back by the destructor.
        stashed_avatar_ = std::make_unique<avatar>( std::move( g->player_at( 0 ) ) );
        g->player_at( 0 ) = avatar();
        for( size_t i = 1; i < players; ++i ) {
            g->add_player_slot();
        }
    }
    g->player_at( 0 ).name = "P0";
    for( size_t i = 1; i < players && !join; ++i ) {
        avatar &extra = g->add_player_slot();
        extra.set_body();
        extra.name = "P" + std::to_string( i );
        // A new slot is created nowhere in particular -- it was measured at
        // z = -500, off the map -- and the world half of a closed turn then
        // works on that position (scent, fields, caches). Stand every extra
        // player on the test map, a column south of P0 and two tiles apart, so
        // none of them is adjacent to anything a test puts next to P0.
        extra.setpos( get_map(), g->player_at( 0 ).pos_bub() +
                      tripoint_rel_ms( 0, 2 * static_cast<int>( i ), 0 ) );
    }
    // g->add_player_slot() leaves a new avatar at moves == 100, unlike a fresh
    // avatar's zero. Zero every slot here so the "fresh avatars start with zero
    // moves" turn-counting the tests rely on is actually true for every slot,
    // not just slot 0.
    for( size_t i = 0; i < players; ++i ) {
        g->player_at( i ).set_moves( 0 );
    }
    mp_sync_context_count();
    // Fresh session: no leftover barrier state from a previous harness. The
    // input-wait flags, the turn-open flag and the end-of-session request belong
    // to a session, not to a turn, so mp_turn_reset() deliberately leaves them --
    // reset_all is the session/test boundary.
    mp_turn_reset_all();
    // The plan table is session state too; a previous harness's intent must not
    // be visible in this session's first party body.
    mp_plan_reset_all();

    for( size_t i = 0; i < players; ++i ) {
        // The latency is not decoration. Without it, code that only works at
        // zero round-trip time passes here and fails on the first real wire.
        mp_loopback_pair wire = mp_make_loopback_pair( latency_ms );
        auto session = std::make_unique<mp_session>();
        session->transport = wire.a;
        session->context_index = i;
        session->name = "P" + std::to_string( i );
        // What this client's hello carried. A real server sent it the card
        // when its character was seated, and a reconnecting process offers the
        // same id back; without it here a seat could never be reclaimed by a
        // card, because no session would have one to match.
        session->card_save_id = g->player_at( i ).get_save_id();
        session->grid.resize( MP_HARNESS_COLS, MP_HARNESS_ROWS );
        mp_sessions().push_back( std::move( session ) );
        client_wires_.push_back( wire.b );
        client_grids_.emplace_back( MP_HARNESS_COLS, MP_HARNESS_ROWS );
        client_states_.emplace_back();
        client_scene_frames_.push_back( 0 );
        client_party_.emplace_back();
        client_party_generation_.push_back( 0 );
        party_read_generation_.push_back( 0 );
    }

    // Each thread runs its player's join menu first in join mode, and goes
    // straight to its turns otherwise -- which also clears what a previous
    // harness's join phase left behind.
    //
    // The readers go up first: a player thread's very first input wait reads
    // the seat's inbox, and a wire with no reader would never fill it.
    mp_server_start_readers();
    mp_join_begin( join ? players : 0, world_anchor );
    seated_ = !join;

    sched_ = std::make_unique<mp_sched>( players );
    mp_set_active_sched( sched_.get() );
    // The same thread body, per-seat lifecycle and watchdog the server raises
    // (mp_server_start_player_threads), not copies of them that can drift: the
    // harness has to be able to drop a seat and take a replacement in it exactly
    // as the server does.
    mp_sched_start_watchdog( *sched_, MP_WATCHDOG_MS );
    for( size_t i = 0; i < players; ++i ) {
        mp_server_start_player_thread( i );
    }
}

mp_session_harness::~mp_session_harness()
{
    // The server's own teardown: wires first, then the shutdown, then the
    // joins, ending in the world phase. Most harness tests end with a player
    // in a menu, which is the case that ordering exists for.
    mp_stop_player_threads( sched_.get() );
    mp_set_active_sched( nullptr );
    // The readers are the harness's too, and they hold their sessions'
    // addresses: join them before the roster goes away.
    mp_server_stop_readers();
    mp_sessions_reset_for_test();
    mp_join_begin( 0, std::nullopt );
    game_test_helper::reset_player_slots();
    if( stashed_avatar_ ) {
        g->player_at( 0 ) = std::move( *stashed_avatar_ );
        stashed_avatar_.reset();
    }
    mp_sync_context_count();
    mp_turn_reset_all();
    // The harness tears a session down by hand rather than through
    // mp_server_end_session(), so it has to forget the plans too: a case that
    // ends mid-round must not leave an intent or a refusal for the next one.
    mp_plan_reset_all();
    // A session that ended (a death, a quit) leaves the quit status behind,
    // and the next test's first do_turn() would read it as its game being over.
    g->uquit = QUIT_NO;
    // Put the globals back here rather than letting the member die on the way
    // out of the object, because the context has to be re-mirrored from the
    // restored terminal and not from the harness's.
    startup_.reset();
    // Restoring the globals is not enough on its own. mp_ensure_player_context()
    // copied TERMX, TERMY and stdscr into g->contexts[0] when the harness
    // started (src/mp/mp_context.cpp:86-89), and mp_activate_player() publishes
    // that copy straight back out into the same globals (:126, :136). A later
    // test that reaches mp_activate_player() -- directly, or through
    // mp_sync_context_count(), which several [mp] tests do -- without calling
    // ensure itself would therefore restore the harness's terminal over the one
    // just put back. Re-mirroring here leaves the context agreeing with the
    // globals, so that path publishes the pre-harness values instead.
    mp_ensure_player_context();
}

void mp_session_harness::press( const size_t slot, const int key )
{
    mp_control msg;
    msg.kind = mp_control_kind::input;
    msg.event = input_event( key, input_event_t::keyboard_char );
    const std::string json = mp_control_to_json( msg );
    client_wires_[slot]->send( mp_wrap( mp_msg_kind::control,
                                        std::vector<uint8_t>( json.begin(), json.end() ) ) );
}

void mp_session_harness::mouse_move( const size_t slot, const point &pos )
{
    mp_control msg;
    msg.kind = mp_control_kind::input;
    // The same event the curses client's backend turns a bare pointer move
    // into (src/ncurses_def.cpp: KEY_MOUSE without a button), carried in the
    // same control message as a keystroke.
    msg.event = input_event( MouseInput::Move, input_event_t::mouse );
    msg.event.mouse_pos = pos;
    const std::string json = mp_control_to_json( msg );
    client_wires_[slot]->send( mp_wrap( mp_msg_kind::control,
                                        std::vector<uint8_t>( json.begin(), json.end() ) ) );
}

void mp_session_harness::send_plan( const size_t slot, const uint32_t seq,
                                    const mp_plan_kind kind, const int x, const int y )
{
    mp_control msg;
    msg.kind = mp_control_kind::plan;
    msg.plan.seq = seq;
    msg.plan.kind = kind;
    // The seat's own scene cells, one z-level (phase 1). `cost` and `label` are
    // the client's display estimates: the loop recomputes the cost from the
    // world and the plan's label is not part of the round yet.
    msg.plan.x = x;
    msg.plan.y = y;
    const std::string json = mp_control_to_json( msg );
    client_wires_[slot]->send( mp_wrap( mp_msg_kind::control,
                                        std::vector<uint8_t>( json.begin(), json.end() ) ) );
}

namespace
{
// The three round controls that carry nothing but their sequence. Their kind
// is the whole message; the plan body only supplies the echo.
void send_round_control( mp_transport &wire, const mp_control_kind kind, const uint32_t seq )
{
    mp_control msg;
    msg.kind = kind;
    msg.plan.seq = seq;
    const std::string json = mp_control_to_json( msg );
    wire.send( mp_wrap( mp_msg_kind::control,
                        std::vector<uint8_t>( json.begin(), json.end() ) ) );
}
} // namespace

void mp_session_harness::send_ready( const size_t slot, const uint32_t seq )
{
    send_round_control( *client_wires_[slot], mp_control_kind::plan_ready, seq );
}

void mp_session_harness::send_cancel( const size_t slot, const uint32_t seq )
{
    send_round_control( *client_wires_[slot], mp_control_kind::plan_cancel, seq );
}

void mp_session_harness::send_pass( const size_t slot, const uint32_t seq )
{
    send_round_control( *client_wires_[slot], mp_control_kind::plan_pass, seq );
}

void mp_session_harness::receive_client_frame( const size_t slot,
        const std::vector<uint8_t> &payload )
{
    mp_msg_kind kind = mp_msg_kind::control;
    std::vector<uint8_t> body;
    if( mp_unwrap( payload, kind, body ) ) {
        if( kind == mp_msg_kind::scene ) {
            // The text client path has no scene buffer, so a scene frame is
            // otherwise dropped unread. Keep its sequence: it is the only
            // client-side evidence that a seat was sent a new frame.
            mp_scene scene;
            if( mp_scene_apply( scene, body ) ) {
                client_scene_frames_[slot] = scene.frame_seq;
            }
            return;
        }
        if( kind == mp_msg_kind::control ) {
            mp_control msg;
            if( mp_control_from_json( std::string( body.begin(), body.end() ), msg ) &&
                msg.kind == mp_control_kind::party ) {
                client_party_[slot] = std::move( msg.party );
                ++client_party_generation_[slot];
                return;
            }
        }
    }
    mp_client_apply_message( payload, client_grids_[slot], client_states_[slot] );
}

bool mp_session_harness::wait_party( const size_t slot, const int timeout_ms,
                                     mp_party_msg &out )
{
    if( slot >= client_wires_.size() ) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds( timeout_ms );
    // The world is not running while this polls, so every frame that can still
    // arrive was already sent and only needs time to cross the wire. Settle on
    // a quiet gap longer than that latency instead of returning at once: the
    // snapshot from before the caller's run_until() is usually already
    // delivered, and the one the caller observed is still in flight behind it.
    const auto quiet = std::chrono::milliseconds( latency_ms_ * 4 + 10 );
    auto quiet_since = std::chrono::steady_clock::now();
    while( true ) {
        while( const std::optional<std::vector<uint8_t>> msg = client_wires_[slot]->recv( 1 ) ) {
            receive_client_frame( slot, *msg );
            quiet_since = std::chrono::steady_clock::now();
        }
        const auto now = std::chrono::steady_clock::now();
        if( client_party_generation_[slot] > party_read_generation_[slot] &&
            now - quiet_since >= quiet ) {
            out = client_party_[slot];
            party_read_generation_[slot] = client_party_generation_[slot];
            return true;
        }
        if( now >= deadline ) {
            return false;
        }
    }
}

uint32_t mp_session_harness::last_scene_frame( const size_t slot ) const
{
    return slot < client_scene_frames_.size() ? client_scene_frames_[slot] : 0;
}

bool mp_session_harness::wait_scene_frame( const size_t slot, const uint32_t min_frame,
        const int timeout_ms )
{
    if( slot >= client_wires_.size() ) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds( timeout_ms );
    while( true ) {
        if( client_scene_frames_[slot] >= min_frame ) {
            return true;
        }
        if( std::chrono::steady_clock::now() >= deadline ) {
            return false;
        }
        while( const std::optional<std::vector<uint8_t>> msg = client_wires_[slot]->recv( 1 ) ) {
            receive_client_frame( slot, *msg );
        }
    }
}

void mp_session_harness::set_client_caps( const size_t slot, const uint32_t caps )
{
    mp_session *const session = mp_session_of_slot( slot );
    cata_assert( session != nullptr );
    session->caps |= caps;
}

void mp_session_harness::reset_client_view( const size_t slot )
{
    client_grids_[slot] = mp_screen_grid( MP_HARNESS_COLS, MP_HARNESS_ROWS );
    client_states_[slot] = mp_client_state();
    client_scene_frames_[slot] = 0;
    client_party_[slot] = mp_party_msg();
    client_party_generation_[slot] = 0;
    party_read_generation_[slot] = 0;
}

void mp_session_harness::pump_clients()
{
    for( size_t i = 0; i < client_wires_.size(); ++i ) {
        while( const std::optional<std::vector<uint8_t>> msg = client_wires_[i]->recv( 0 ) ) {
            receive_client_frame( i, *msg );
        }
    }
}

void mp_session_harness::choose_random_character( const size_t slot )
{
    // "New character: random", then the creation screen opens on its summary:
    // the next-tab key there finishes, and the confirmation takes a capital Y.
    press( slot, 'r' );
    press( slot, '\t' );
    press( slot, 'Y' );
}

void mp_session_harness::quit_join_menu( const size_t slot )
{
    // The Quit entry's hotkey, which is the key a person presses.
    press( slot, 'q' );
}

void mp_session_harness::drop_client( const size_t slot )
{
    // Closing this end of the loopback pair closes the other end too, so the
    // server's session sees exactly what a client process dying leaves behind:
    // a transport that reports itself disconnected.
    client_wires_[slot]->close();
}

bool mp_session_harness::client_connected( const size_t slot ) const
{
    return slot < client_wires_.size() && client_wires_[slot] &&
           client_wires_[slot]->connected();
}

bool mp_session_harness::seat_occupied( const size_t slot ) const
{
    return mp_session_of_slot( slot ) != nullptr;
}

void mp_session_harness::connect_replacement_client( const size_t slot )
{
    cata_assert( !client_connected( slot ) );
    // A newcomer carries no character card: it is a new person, not a
    // returning character.
    mp_loopback_pair wire = mp_make_loopback_pair( latency_ms_ );
    auto session = std::make_unique<mp_session>();
    session->transport = wire.a;
    session->name = "P" + std::to_string( slot );
    session->grid.resize( MP_HARNESS_COLS, MP_HARNESS_ROWS );
    mp_transport *const admitted = wire.a.get();

    if( seat_occupied( slot ) ) {
        // The seat died and its client left, but the server kept the session
        // and its parked thread. The newcomer's wire is installed on them by
        // the same admission path a real connection takes; the next grant opens
        // the replacement menu because the seat still owes a character.
        std::string err;
        cata_assert( mp_server_accept_new_client( std::move( session ), err ) );
        // Admission takes the first free seat; this helper was asked for a
        // particular one, so a different seat is a test bug, not a re-seat.
        cata_assert( mp_session_of_slot( slot ) != nullptr &&
                     mp_session_of_slot( slot )->transport.get() == admitted );
    } else {
        // Freed during the join phase: no session and no thread survived, so
        // this is the ordinary seating path.
        mp_server_seat_client( slot, std::move( session ) );
    }
    client_wires_[slot] = wire.b;
    reset_client_view( slot );
}

bool mp_session_harness::reconnect_client( const size_t slot, const std::string &save_id,
        std::string *refusal )
{
    // A returning process: a new wire with the same latency as the first ones,
    // and a hello whose card names the character it left. The seat it lands in
    // is the server's to choose, by card -- `slot` only says where the harness
    // keeps this client's wire when it is accepted.
    mp_loopback_pair wire = mp_make_loopback_pair( latency_ms_ );
    auto session = std::make_unique<mp_session>();
    session->transport = wire.a;
    session->name = "P" + std::to_string( slot );
    session->card_save_id = save_id;
    session->grid.resize( MP_HARNESS_COLS, MP_HARNESS_ROWS );

    // The same entry the post-start listener poll uses. A refusal is the
    // server's answer, observed from the wire rather than assumed: the bye is
    // queued before the close.
    std::string err;
    if( !mp_server_accept_new_client( std::move( session ), err ) ) {
        if( refusal != nullptr ) {
            *refusal = read_bye_reason( *wire.b, latency_ms_ + 1000 );
        }
        return false;
    }

    client_wires_[slot] = wire.b;
    reset_client_view( slot );
    return true;
}

bool mp_session_harness::try_admit_new_client( const std::string &username, std::string &err )
{
    // A person who was not here when the party started: a hello with a name
    // and no character card. The server decides which seat, if any, takes it.
    mp_loopback_pair wire = mp_make_loopback_pair( latency_ms_ );
    auto session = std::make_unique<mp_session>();
    session->transport = wire.a;
    session->name = username;
    session->grid.resize( MP_HARNESS_COLS, MP_HARNESS_ROWS );
    mp_transport *const admitted = wire.a.get();

    if( !mp_server_accept_new_client( std::move( session ), err ) ) {
        // The server sent its reason before closing: read why from this end.
        err = read_bye_reason( *wire.b, latency_ms_ + 1000 );
        return false;
    }
    // The server chose the seat. Find the one holding this wire so press() and
    // screen() address the newcomer's client end.
    for( size_t slot = 0; slot < client_wires_.size(); ++slot ) {
        const mp_session *const seated = mp_session_of_slot( slot );
        if( seated != nullptr && seated->transport.get() == admitted ) {
            client_wires_[slot] = wire.b;
            reset_client_view( slot );
            return true;
        }
    }
    cata_assert( false );
    return false;
}

bool mp_session_harness::seated() const
{
    return seated_;
}

bool mp_session_harness::run_until( const std::function<bool()> &predicate, const int max_turns )
{
    for( int turn = 0; turn < max_turns; ++turn ) {
        if( !seated_ ) {
            // The join phase: no turn runs until everyone has chosen.
            std::string err;
            const mp_join_progress progress = mp_join_poll( *sched_, err );
            if( progress == mp_join_progress::failed ) {
                // Reported where a test can see it, then treated as running out
                // of turns.
                UNSCOPED_INFO( "join phase failed: " << err );
                return false;
            }
            seated_ = progress == mp_join_progress::seated;
        } else {
            g->do_turn();
        }
        pump_clients();
        if( predicate() ) {
            return true;
        }
    }
    // Returning false rather than looping forever is the point: a barrier bug
    // shows up as a failed assertion with a message, not as a hung CI job.
    return false;
}

bool mp_session_harness::message_log_contains( const std::string &text ) const
{
    for( const std::pair<std::string, std::string> &entry : Messages::recent_messages( 1000 ) ) {
        if( entry.second.find( text ) != std::string::npos ) {
            return true;
        }
    }
    return false;
}

const mp_screen_grid &mp_session_harness::screen( const size_t slot ) const
{
    return client_grids_[slot];
}

int mp_grid_nonblank_cells( const mp_screen_grid &grid )
{
    int cells = 0;
    for( int y = 0; y < grid.height(); ++y ) {
        for( int x = 0; x < grid.width(); ++x ) {
            cells += grid.at( x, y ).ch != " " ? 1 : 0;
        }
    }
    return cells;
}

bool mp_grid_contains( const mp_screen_grid &grid, const std::string &text )
{
    for( int y = 0; y < grid.height(); ++y ) {
        std::string row;
        for( int x = 0; x < grid.width(); ++x ) {
            row += grid.at( x, y ).ch;
        }
        if( row.find( text ) != std::string::npos ) {
            return true;
        }
    }
    return false;
}

bool mp_grids_equal( const mp_screen_grid &a, const mp_screen_grid &b )
{
    if( a.width() != b.width() || a.height() != b.height() ) {
        return false;
    }
    for( int y = 0; y < a.height(); ++y ) {
        for( int x = 0; x < a.width(); ++x ) {
            if( a.at( x, y ) != b.at( x, y ) ) {
                return false;
            }
        }
    }
    return true;
}

#endif // MP_SERVER
