#include "cata_catch.h"

#if defined(MP_SERVER)

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "avatar.h"
#include "calendar.h"
#include "cata_scope_helpers.h"
#include "cursesdef.h"
#include "game.h"
#include "game_constants.h"
#include "game_test_helper.h"
#include "map.h"
#include "map_helpers.h"
#include "map_helpers_tests.h"
#include "mp/mp_cell.h"
#include "mp/mp_context.h"
#include "mp/mp_loopback_transport.h"
#include "mp/mp_protocol.h"
#include "mp/mp_scene.h"
#include "mp/mp_scene_codec.h"
#include "mp/mp_sched.h"
#include "mp/mp_server_input.h"
#include "mp/mp_server_main.h"
#include "mp/mp_server_mask.h"
#include "mp/mp_server_screen.h"
#include "mp/mp_session.h"
#include "mp/mp_transport.h"
#include "mp/mp_turn.h"
#include "output.h"
#include "panels.h"
#include "player_helpers.h"
#include "point.h"
#include "type_id.h"

namespace
{

static const efftype_id effect_boomered( "boomered" );

// Both contexts are static rather than stack locals: the active-context pointer
// must not dangle between tests.
static player_context stream_ctx0;
static player_context stream_ctx1;

struct wire_frame {
    mp_msg_kind kind = mp_msg_kind::control;
    std::vector<uint8_t> body;
};

// A cleared world with a real avatar, installed in the test's own context. The
// context carries its own terrain window, so the map rect the scene flush reads
// is deterministic rather than whatever the previous test left behind.
void prepare_stream_world( player_context &ctx, const int width, const int height,
                           const point &origin )
{
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    clear_creatures();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_activate_player( 0 );

    mp_set_active_context( &ctx );
    ctx.slot_index = 0;
    ctx.input_queue.clear();
    ctx.ter_view_p = get_avatar_legacy().pos_bub();
    ctx.w_terrain = catacurses::newwin( height, width, origin );

    mp_set_active_sched( nullptr );
    mp_sessions_reset_for_test();
    mp_server_reset_for_test();
}

// Gives `ctx` a map window and the avatar's view centre. Slot 1 has no game
// context behind it on purpose: the session layer addresses seats by index and
// the flush only needs the active context's geometry.
void arm_context( player_context &ctx, const size_t slot, const int width, const int height,
                  const point &origin )
{
    ctx.slot_index = slot;
    ctx.input_queue.clear();
    ctx.ter_view_p = get_avatar_legacy().pos_bub();
    ctx.w_terrain = catacurses::newwin( height, width, origin );
}

std::shared_ptr<mp_transport> install_session( const size_t slot )
{
    mp_loopback_pair pair = mp_make_loopback_pair();
    auto session = std::make_unique<mp_session>();
    session->transport = pair.a;
    session->context_index = slot;
    mp_sessions().push_back( std::move( session ) );
    return pair.b;
}

// Drains everything the server has sent so far, in order.
std::vector<wire_frame> drain_frames( mp_transport &transport )
{
    std::vector<wire_frame> frames;
    while( const std::optional<std::vector<uint8_t>> payload = transport.recv( 0 ) ) {
        wire_frame frame;
        REQUIRE( mp_unwrap( *payload, frame.kind, frame.body ) );
        frames.push_back( std::move( frame ) );
    }
    return frames;
}

size_t index_of_kind( const std::vector<wire_frame> &frames, const mp_msg_kind kind )
{
    for( size_t i = 0; i < frames.size(); ++i ) {
        if( frames[i].kind == kind ) {
            return i;
        }
    }
    return frames.size();
}

size_t count_of_kind( const std::vector<wire_frame> &frames, const mp_msg_kind kind )
{
    size_t count = 0;
    for( const wire_frame &frame : frames ) {
        count += frame.kind == kind ? 1 : 0;
    }
    return count;
}

// The one scene frame in `frames`, decoded. Fails the test when there is not
// exactly one.
mp_scene decode_only_scene( const std::vector<wire_frame> &frames )
{
    REQUIRE( count_of_kind( frames, mp_msg_kind::scene ) == 1 );
    const size_t index = index_of_kind( frames, mp_msg_kind::scene );
    REQUIRE( index < frames.size() );
    mp_scene scene;
    REQUIRE( mp_scene_apply( scene, frames[index].body ) );
    return scene;
}

// Runs `mp_server_flush_screen()` on a player thread that actually holds the
// baton. There is no seam that sets the holder directly, so the world yields to
// the slot, the player flushes and hands the baton back, and the world joins.
void flush_scene_under_baton( const size_t slot, const size_t players = 1 )
{
    mp_sched sched( players );
    mp_set_active_sched( &sched );
    std::thread player( [&sched, slot]() {
        sched.wait_for_turn( slot );
        mp_server_flush_screen();
        sched.release_to( slot, mp_sched::world );
    } );
    sched.yield_to( mp_sched::world, slot );
    player.join();
    mp_set_active_sched( nullptr );
}

void leave_world_phase()
{
    mp_ensure_player_context();
    mp_sync_context_count();
    mp_activate_player( 0 );
}

void send_hello_json( mp_transport &transport, const std::string &json )
{
    transport.send( mp_wrap( mp_msg_kind::control,
                             std::vector<uint8_t>( json.begin(), json.end() ) ) );
}

// One row of 80 distinct printable ASCII glyphs, incompressible to the screen
// codec, so each flush queues roughly 11 KB. The backlog test needs that bulk
// to cross MP_SCREEN_BACKLOG_BYTES in a few dozen flushes.
std::vector<mp_cell> pattern_row( const int row, const int seed )
{
    std::vector<mp_cell> cells;
    cells.reserve( 80 );
    for( int x = 0; x < 80; ++x ) {
        mp_cell cell;
        cell.ch = std::string( 1, static_cast<char>( '!' + ( x + 3 * row + seed ) % 94 ) );
        cells.push_back( cell );
    }
    return cells;
}

void blit_full_screen( const int seed )
{
    for( int y = 0; y < 24; ++y ) {
        mp_server_blit_row( 0, y, pattern_row( y, seed ) );
    }
}

// A returning client: a real hello carrying the seat's card, run through the
// handshake and the post-start admission path. Returns the client end of the
// new wire so the caller can drain what the retained seat sends.
std::shared_ptr<mp_transport> admit_with_card( const std::string &card, const uint32_t caps,
        const uint8_t z_levels )
{
    mp_loopback_pair pair = mp_make_loopback_pair();
    auto session = std::make_unique<mp_session>();
    session->transport = pair.a;

    mp_control hello;
    hello.kind = mp_control_kind::hello;
    hello.hello.proto = MP_PROTOCOL_VERSION;
    hello.hello.name = "reconnect";
    hello.hello.width = 80;
    hello.hello.height = 24;
    hello.hello.caps = caps;
    hello.hello.z_levels = z_levels;
    hello.hello.save_id = card;
    send_hello_json( *pair.b, mp_control_to_json( hello ) );

    std::string reason;
    REQUIRE( mp_server_handshake( *pair.a, *session, reason ) );
    CHECK( reason.empty() );
    std::string err;
    REQUIRE( mp_server_accept_new_client( std::move( session ), err ) );
    CHECK( err.empty() );
    return pair.b;
}

} // namespace

TEST_CASE( "mp_scene_stream_hello_without_caps_still_connects", "[mp]" )
{
    mp_loopback_pair pair = mp_make_loopback_pair();
    mp_session session;

    // The shortest hello an old text client can send: no caps, no zl.
    send_hello_json( *pair.b,
                     R"({"t":"hello","proto":2,"name":"text","w":80,"h":24,"c256":false})" );

    std::string reason;
    REQUIRE( mp_server_handshake( *pair.a, session, reason ) );
    CHECK( reason.empty() );
    CHECK( session.scene_caps == 0 );
    CHECK( session.scene_z_levels == 10 );
}

TEST_CASE( "mp_scene_stream_hello_caps_and_z_levels_round_trip", "[mp]" )
{
    mp_control sent;
    sent.kind = mp_control_kind::hello;
    sent.hello.proto = MP_PROTOCOL_VERSION;
    sent.hello.name = "scene";
    sent.hello.width = 80;
    sent.hello.height = 24;
    sent.hello.caps = 1;
    sent.hello.z_levels = 3;

    mp_control got;
    REQUIRE( mp_control_from_json( mp_control_to_json( sent ), got ) );
    CHECK( got.hello.caps == 1 );
    CHECK( got.hello.z_levels == 3 );

    // A request below the floor and one above the ceiling are clamped at parse
    // time; an unknown member is ignored rather than refused.
    REQUIRE( mp_control_from_json(
                 R"({"t":"hello","proto":2,"name":"n","w":80,"h":24,"c256":false,)"
                 R"("caps":1,"zl":0,"future":true})",
                 got ) );
    CHECK( got.hello.caps == 1 );
    CHECK( got.hello.z_levels == 1 );

    REQUIRE( mp_control_from_json(
                 R"({"t":"hello","proto":2,"name":"n","w":80,"h":24,"c256":false,)"
                 R"("caps":1,"zl":99,"future":true})",
                 got ) );
    CHECK( got.hello.z_levels == 10 );

    // The handshake copies what the client negotiated into the session state.
    mp_loopback_pair pair = mp_make_loopback_pair();
    mp_session session;
    sent.hello.caps = 1;
    sent.hello.z_levels = 3;
    send_hello_json( *pair.b, mp_control_to_json( sent ) );
    std::string reason;
    REQUIRE( mp_server_handshake( *pair.a, session, reason ) );
    CHECK( session.scene_caps == 1 );
    CHECK( session.scene_z_levels == 3 );
}

TEST_CASE( "mp_scene_stream_scene_frame_follows_the_screen_frame", "[mp]" )
{
    restore_active_state restore;
    prepare_stream_world( stream_ctx0, 40, 10, point( 10, 0 ) );
    arm_context( stream_ctx1, 1, 20, 12, point( 0, 0 ) );

    std::shared_ptr<mp_transport> first = install_session( 0 );
    std::shared_ptr<mp_transport> second = install_session( 1 );
    REQUIRE( mp_session_of_slot( 0 ) != nullptr );
    REQUIRE( mp_session_of_slot( 1 ) != nullptr );
    mp_session_of_slot( 0 )->scene_caps = 1;
    mp_session_of_slot( 1 )->scene_caps = 1;

    // Mark each grid fully dirty, so every seat also emits a screen frame.
    mp_set_active_context( &stream_ctx0 );
    mp_server_set_terminal_size( 80, 24 );
    flush_scene_under_baton( 0, 2 );

    mp_set_active_context( &stream_ctx1 );
    mp_server_set_terminal_size( 80, 24 );
    flush_scene_under_baton( 1, 2 );

    const std::vector<wire_frame> first_frames = drain_frames( *first );
    const size_t first_screen = index_of_kind( first_frames, mp_msg_kind::screen );
    const size_t first_scene = index_of_kind( first_frames, mp_msg_kind::scene );
    REQUIRE( first_screen < first_frames.size() );
    REQUIRE( first_scene < first_frames.size() );
    CHECK( first_screen < first_scene );

    mp_scene scene0;
    REQUIRE( mp_scene_apply( scene0, first_frames[first_scene].body ) );
    CHECK( scene0.frame_seq == 1 );
    CHECK( scene0.tiles_w == 40 );
    CHECK( scene0.tiles_h == 10 );
    CHECK( scene0.ui_x == 10 );
    CHECK( scene0.ui_y == 0 );
    CHECK( scene0.ui_w == 40 );
    CHECK( scene0.ui_h == 10 );

    const std::vector<wire_frame> second_frames = drain_frames( *second );
    const size_t second_screen = index_of_kind( second_frames, mp_msg_kind::screen );
    const size_t second_scene = index_of_kind( second_frames, mp_msg_kind::scene );
    REQUIRE( second_screen < second_frames.size() );
    REQUIRE( second_scene < second_frames.size() );
    CHECK( second_screen < second_scene );

    mp_scene scene1;
    REQUIRE( mp_scene_apply( scene1, second_frames[second_scene].body ) );
    CHECK( scene1.frame_seq == 1 );
    CHECK( scene1.tiles_w == 20 );
    CHECK( scene1.tiles_h == 12 );
    CHECK( scene1.ui_x == 0 );
    CHECK( scene1.ui_y == 0 );
    CHECK( scene1.ui_w == 20 );
    CHECK( scene1.ui_h == 12 );
}

TEST_CASE( "mp_scene_stream_unchanged_scene_does_not_advance_frame_seq", "[mp]" )
{
    restore_active_state restore;
    prepare_stream_world( stream_ctx0, 40, 10, point( 10, 0 ) );
    std::shared_ptr<mp_transport> wire = install_session( 0 );
    mp_session *const session = mp_session_of_slot( 0 );
    REQUIRE( session != nullptr );
    session->scene_caps = 1;
    mp_server_set_terminal_size( 80, 24 );

    // The first scene is built because the seat's map was just blitted.
    catacurses::mvwprintw( stream_ctx0.w_terrain, point::zero, "." );
    catacurses::wrefresh( stream_ctx0.w_terrain );
    CHECK( session->scene_terrain_dirty );

    flush_scene_under_baton( 0 );
    const std::vector<wire_frame> first = drain_frames( *wire );
    const mp_scene sent = decode_only_scene( first );
    CHECK( sent.frame_seq == 1 );
    CHECK_FALSE( session->scene_terrain_dirty );

    // A second blit of the same map marks the seat dirty again, but the built
    // body hashes the same, so no frame goes out; the completed build still
    // consumes the blit and the idle flushes after it must not rebuild.
    catacurses::mvwprintw( stream_ctx0.w_terrain, point::zero, "." );
    catacurses::wrefresh( stream_ctx0.w_terrain );
    CHECK( session->scene_terrain_dirty );

    flush_scene_under_baton( 0 );
    const std::vector<wire_frame> second = drain_frames( *wire );
    CHECK( count_of_kind( second, mp_msg_kind::scene ) == 0 );
    CHECK_FALSE( session->scene_terrain_dirty );
}

TEST_CASE( "mp_scene_stream_scene_is_not_sent_without_the_baton", "[mp]" )
{
    restore_active_state restore;
    prepare_stream_world( stream_ctx0, 40, 10, point( 10, 0 ) );
    std::shared_ptr<mp_transport> wire = install_session( 0 );
    mp_session *const session = mp_session_of_slot( 0 );
    REQUIRE( session != nullptr );
    session->scene_caps = 1;
    mp_server_set_terminal_size( 80, 24 );

    // The legacy path: a capable session and an active context, but no
    // scheduler at all.
    mp_set_active_sched( nullptr );
    mp_server_flush_screen();
    CHECK( count_of_kind( drain_frames( *wire ), mp_msg_kind::scene ) == 0 );

    // The join-loop redraw: the world holds the baton while a player's context
    // is active, so the seat is not entitled to a frame.
    mp_sched sched( 1 );
    mp_set_active_sched( &sched );
    mp_server_flush_screen();
    CHECK( count_of_kind( drain_frames( *wire ), mp_msg_kind::scene ) == 0 );
    mp_set_active_sched( nullptr );

    // The world phase itself has no active session.
    mp_activate_world();
    mp_server_flush_screen();
    CHECK( count_of_kind( drain_frames( *wire ), mp_msg_kind::scene ) == 0 );

    leave_world_phase();
}

TEST_CASE( "mp_scene_stream_scene_uses_the_requested_z_levels", "[mp]" )
{
    restore_active_state restore;
    prepare_stream_world( stream_ctx0, 40, 10, point( 10, 0 ) );
    std::shared_ptr<mp_transport> wire = install_session( 0 );
    mp_session *const session = mp_session_of_slot( 0 );
    REQUIRE( session != nullptr );
    session->scene_caps = 1;
    session->scene_z_levels = 2;
    mp_server_set_terminal_size( 80, 24 );

    flush_scene_under_baton( 0 );
    const mp_scene scene = decode_only_scene( drain_frames( *wire ) );
    REQUIRE( scene.levels.size() == 2 );
    CHECK( scene.levels[0].z_rel == 0 );
    CHECK( scene.levels[1].z_rel == -1 );
}

TEST_CASE( "mp_scene_stream_scene_is_not_sent_to_a_text_only_client", "[mp]" )
{
    restore_active_state restore;
    prepare_stream_world( stream_ctx0, 40, 10, point( 10, 0 ) );
    std::shared_ptr<mp_transport> wire = install_session( 0 );
    mp_session *const session = mp_session_of_slot( 0 );
    REQUIRE( session != nullptr );
    session->scene_caps = 0;
    mp_server_set_terminal_size( 80, 24 );

    flush_scene_under_baton( 0 );
    const std::vector<wire_frame> frames = drain_frames( *wire );
    CHECK( count_of_kind( frames, mp_msg_kind::scene ) == 0 );
    // The screen still went out: a text client is a full client.
    CHECK( count_of_kind( frames, mp_msg_kind::screen ) == 1 );
}

TEST_CASE( "mp_scene_stream_blank_avatar_sends_no_scene", "[mp]" )
{
    restore_active_state restore;
    prepare_stream_world( stream_ctx0, 40, 10, point( 10, 0 ) );
    std::shared_ptr<mp_transport> wire = install_session( 0 );
    mp_session *const session = mp_session_of_slot( 0 );
    REQUIRE( session != nullptr );
    session->scene_caps = 1;
    mp_server_set_terminal_size( 80, 24 );

    // The join phase hands the baton to a seat whose avatar is still the blank
    // sentinel at (20, 10, -500). The builder anchors at that z, immediately
    // runs out of map, and returns no levels; that body would encode z_count 0,
    // which the project's decoder rejects. set_pos_abs_only moves the avatar
    // without the cache rebuild setpos() would run for such a z.
    avatar &you = get_avatar_legacy();
    const tripoint_abs_ms saved_pos = you.pos_abs();
    you.set_pos_abs_only( get_map().get_abs( tripoint_bub_ms( 0, 0, -500 ) ) );
    REQUIRE( you.posz() == -500 );

    flush_scene_under_baton( 0 );
    const std::vector<wire_frame> blank = drain_frames( *wire );
    CHECK( count_of_kind( blank, mp_msg_kind::scene ) == 0 );
    CHECK_FALSE( session->scene_sent_once );

    // Once placed on the map the seat gets its first real frame, so the skipped
    // build left the send state intact.
    you.set_pos_abs_only( saved_pos );
    flush_scene_under_baton( 0 );
    const mp_scene scene = decode_only_scene( drain_frames( *wire ) );
    CHECK( scene.frame_seq == 1 );
    REQUIRE_FALSE( scene.levels.empty() );
}

TEST_CASE( "mp_scene_stream_backlog_keeps_one_newest_scene_frame", "[mp]" )
{
    restore_active_state restore;
    prepare_stream_world( stream_ctx0, 40, 10, point( 10, 0 ) );
    std::shared_ptr<mp_transport> wire = install_session( 0 );
    mp_session *const session = mp_session_of_slot( 0 );
    REQUIRE( session != nullptr );
    session->scene_caps = 1;
    mp_server_set_terminal_size( 80, 24 );

    // First frame, sequence 1. The peer never reads, so it stays queued.
    flush_scene_under_baton( 0 );
    CHECK( session->scene_seq == 1 );

    // Fill the undelivered history with screen frames until the backlog policy
    // must engage. These flushes run without a baton, so they only queue grid
    // history; the scene frame above is still in the same queue.
    int fills = 0;
    while( session->transport->pending_bytes() <= MP_SCREEN_BACKLOG_BYTES ) {
        REQUIRE( fills < 1000 );
        blit_full_screen( fills );
        mp_server_flush_screen();
        ++fills;
    }
    REQUIRE( session->transport->pending_bytes() > 0 );

    // The next flush sees the over-limit backlog: it must drop the queued
    // screen AND scene history, then rebuild exactly one newest scene frame.
    flush_scene_under_baton( 0 );

    const std::vector<wire_frame> frames = drain_frames( *wire );
    const mp_scene scene = decode_only_scene( frames );
    CHECK( scene.frame_seq == 2 );
    CHECK( session->scene_seq == 2 );
    CHECK( session->scene_sent_once );
}

TEST_CASE( "mp_scene_stream_terrain_blit_marks_the_scene_dirty", "[mp]" )
{
    restore_active_state restore;
    prepare_stream_world( stream_ctx0, 40, 10, point( 10, 0 ) );
    install_session( 0 );
    mp_session *const session = mp_session_of_slot( 0 );
    REQUIRE( session != nullptr );
    session->scene_caps = 1;
    session->scene_terrain_dirty = false;

    catacurses::mvwprintw( stream_ctx0.w_terrain, point::zero, "." );
    catacurses::wrefresh( stream_ctx0.w_terrain );
    CHECK( session->scene_terrain_dirty );
}

TEST_CASE( "activating_a_player_invalidates_the_visibility_cache",
           "[mp]" )
{
    restore_active_state restore;
    prepare_stream_world( stream_ctx0, 40, 10, point( 10, 0 ) );

    // The visibility cache is keyed on the avatar's position AND its character
    // flags, and update_visibility_cache() early-outs on an unchanged position
    // alone. Seat B must therefore sit on seat A's tile, or a positional change
    // would mask the missing invalidation.
    const tripoint_bub_ms seat_pos = get_avatar_legacy().pos_bub();
    g->add_player_slot();
    mp_sync_context_count();
    REQUIRE( g->player_slot_count() == 2 );
    REQUIRE( g->contexts.size() == 2 );
    arm_context( g->contexts[1], 1, 20, 12, point( 0, 0 ) );
    // set_pos_abs_only moves the avatar without the cache rebuild setpos()
    // would run; this seat is not the active one.
    g->player_at( 1 ).set_pos_abs_only( get_map().get_abs( seat_pos ) );

    // Seat A is the active avatar. Have it draw: the cache is built and carries
    // A's boomered flag.
    avatar &you = get_avatar_legacy();
    REQUIRE( &you == &g->player_at( 0 ) );
    you.add_effect( effect_boomered, 1_minutes );
    get_map().update_visibility_cache( you.pos_bub().z() );
    REQUIRE_FALSE( get_map().get_visibility_variables_cache().visibility_cache_dirty );
    REQUIRE( get_map().get_visibility_variables_cache().u_is_boomered );

    // Activating seat B, on the same tile, must mark the cache dirty before B
    // draws: otherwise B's build would read A's computed visibility.
    mp_activate_player( 1 );
    REQUIRE( get_avatar_legacy().pos_bub() == seat_pos );
    CHECK( get_map().get_visibility_variables_cache().visibility_cache_dirty );

    // The draw that follows recomputes with B's own character flags.
    get_map().update_visibility_cache( get_avatar_legacy().pos_bub().z() );
    CHECK_FALSE( get_map().get_visibility_variables_cache().u_is_boomered );

    leave_world_phase();
    // The base avatar outlives the test, so the flag that made the cache
    // observable must not.
    get_avatar_legacy().remove_effect( effect_boomered );
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

TEST_CASE( "two_sessions_get_their_own_scene_rects", "[mp]" )
{
    restore_active_state restore;
    prepare_stream_world( stream_ctx0, 40, 10, point( 10, 0 ) );

    // Two real seats, so the scene's avatar cell comes from the seat's own
    // character rather than from whichever avatar happened to be active.
    const tripoint_abs_ms saved_pos = get_avatar_legacy().pos_abs();
    g->add_player_slot();
    mp_sync_context_count();
    REQUIRE( g->contexts.size() == 2 );
    get_avatar().setpos( get_map(), tripoint_bub_ms( 60, 60, 0 ) );
    stream_ctx0.ter_view_p = get_avatar_legacy().pos_bub();

    mp_activate_player( 1 );
    get_avatar().setpos( get_map(), tripoint_bub_ms( 65, 64, 0 ) );
    arm_context( g->contexts[1], 1, 20, 12, point( 0, 0 ) );

    std::shared_ptr<mp_transport> first = install_session( 0 );
    std::shared_ptr<mp_transport> second = install_session( 1 );
    REQUIRE( mp_session_of_slot( 0 ) != nullptr );
    REQUIRE( mp_session_of_slot( 1 ) != nullptr );
    mp_session_of_slot( 0 )->scene_caps = 1;
    mp_session_of_slot( 1 )->scene_caps = 1;

    // Seat 0 flushes with its own context active, its own map window and its
    // own avatar in u. The blit clears the mask grid, then one region of seat
    // 0's own UI is noted over its map, so its frame and only its frame carries
    // that rect.
    mp_activate_player( 0 );
    mp_set_active_context( &stream_ctx0 );
    mp_server_set_terminal_size( 80, 24 );
    catacurses::mvwprintw( stream_ctx0.w_terrain, point::zero, "." );
    catacurses::wrefresh( stream_ctx0.w_terrain );
    mp_scene_mask_note( 10, 0, 4, 2, 0 );
    flush_scene_under_baton( 0, 2 );
    const mp_scene scene0 = decode_only_scene( drain_frames( *first ) );

    // Seat 1 flushes next, after seat 0's mask exists in the grid. Its own
    // window makes the grid re-sync, so seat 0's UI region must not appear.
    mp_activate_player( 1 );
    mp_server_set_terminal_size( 80, 24 );
    flush_scene_under_baton( 1, 2 );
    const mp_scene scene1 = decode_only_scene( drain_frames( *second ) );

    CHECK( scene0.frame_seq == 1 );
    CHECK( scene0.tiles_w == 40 );
    CHECK( scene0.tiles_h == 10 );
    CHECK( scene0.ui_x == 10 );
    CHECK( scene0.ui_y == 0 );
    CHECK( scene0.ui_w == 40 );
    CHECK( scene0.ui_h == 10 );
    CHECK( scene0.view_dx == -20 );
    CHECK( scene0.view_dy == -5 );
    CHECK( scene0.avatar_dx == 20 );
    CHECK( scene0.avatar_dy == 5 );
    REQUIRE( scene0.masks.size() == 1 );
    CHECK( scene0.masks[0].x == 10 );
    CHECK( scene0.masks[0].y == 0 );
    CHECK( scene0.masks[0].w == 4 );
    CHECK( scene0.masks[0].h == 2 );
    CHECK( scene0.masks[0].role == 0 );

    CHECK( scene1.frame_seq == 1 );
    CHECK( scene1.tiles_w == 20 );
    CHECK( scene1.tiles_h == 12 );
    CHECK( scene1.ui_x == 0 );
    CHECK( scene1.ui_y == 0 );
    CHECK( scene1.ui_w == 20 );
    CHECK( scene1.ui_h == 12 );
    CHECK( scene1.view_dx == -10 );
    CHECK( scene1.view_dy == -6 );
    CHECK( scene1.avatar_dx == 10 );
    CHECK( scene1.avatar_dy == 6 );
    CHECK( scene1.masks.empty() );

    // A second flushed change on seat 0 is seat 0's alone: the other seat's
    // sequence and wire stay where its last frame left them.
    const uint32_t seat1_seq = mp_session_of_slot( 1 )->scene_seq;
    mp_activate_player( 0 );
    mp_set_active_context( &stream_ctx0 );
    stream_ctx0.ter_view_p += tripoint_rel_ms( 1, 0, 0 );
    catacurses::mvwprintw( stream_ctx0.w_terrain, point::zero, "." );
    catacurses::wrefresh( stream_ctx0.w_terrain );
    flush_scene_under_baton( 0, 2 );
    const mp_scene scene0b = decode_only_scene( drain_frames( *first ) );
    CHECK( scene0b.frame_seq == 2 );
    CHECK( scene0b.avatar_dx == 19 );
    CHECK( scene0b.tiles_w == 40 );
    CHECK( mp_session_of_slot( 1 )->scene_seq == seat1_seq );
    CHECK( count_of_kind( drain_frames( *second ), mp_msg_kind::scene ) == 0 );

    leave_world_phase();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    // Back to the position the rest of the suite expects; set_pos_abs_only for
    // the same reason as the placement above.
    get_avatar_legacy().set_pos_abs_only( saved_pos );
}

TEST_CASE( "scene_rect_survives_a_resize", "[mp]" )
{
    restore_active_state restore;
    restore_on_out_of_scope restore_termx( TERMX );
    restore_on_out_of_scope restore_termy( TERMY );
    restore_on_out_of_scope restore_tww( TERRAIN_WINDOW_WIDTH );
    restore_on_out_of_scope restore_twh( TERRAIN_WINDOW_HEIGHT );
    restore_on_out_of_scope restore_twtw( TERRAIN_WINDOW_TERM_WIDTH );
    restore_on_out_of_scope restore_twth( TERRAIN_WINDOW_TERM_HEIGHT );
    restore_on_out_of_scope restore_posx( POSX );
    restore_on_out_of_scope restore_posy( POSY );

    prepare_stream_world( stream_ctx0, 40, 10, point( 10, 0 ) );
    arm_context( stream_ctx1, 1, 20, 12, point( 0, 0 ) );

    std::shared_ptr<mp_transport> first = install_session( 0 );
    std::shared_ptr<mp_transport> second = install_session( 1 );
    REQUIRE( mp_session_of_slot( 0 ) != nullptr );
    REQUIRE( mp_session_of_slot( 1 ) != nullptr );
    mp_session_of_slot( 0 )->scene_caps = 1;
    mp_session_of_slot( 1 )->scene_caps = 1;

    mp_set_active_context( &stream_ctx0 );
    mp_server_set_terminal_size( 80, 24 );
    flush_scene_under_baton( 0, 2 );
    const mp_scene before0 = decode_only_scene( drain_frames( *first ) );

    mp_set_active_context( &stream_ctx1 );
    mp_server_set_terminal_size( 80, 24 );
    flush_scene_under_baton( 1, 2 );
    const mp_scene before1 = decode_only_scene( drain_frames( *second ) );
    REQUIRE( before0.tiles_w == 40 );
    REQUIRE( before0.ui_w == 40 );
    REQUIRE( before1.tiles_w == 20 );
    REQUIRE( before1.ui_w == 20 );

    // The production resize path, driven from the wire: the client's control
    // frame becomes pending_resize, the backend takes it and sizes the grid,
    // and the context's map window is rebuilt at the new metrics.
    mp_set_active_context( &stream_ctx0 );
    const std::string request = R"({"t":"resize","w":100,"h":30})";
    first->send( mp_wrap( mp_msg_kind::control,
                          std::vector<uint8_t>( request.begin(), request.end() ) ) );
    mp_server_pump();
    int width = 0;
    int height = 0;
    REQUIRE( mp_server_take_resize_request( width, height ) );
    CHECK( width == 100 );
    CHECK( height == 30 );
    mp_server_set_terminal_size( width, height );
    TERMX = width;
    TERMY = height;
    mp_resize_active_context();

    const int sidebar_left = panel_manager::get_manager().get_width_left();
    const int new_tiles_w = TERRAIN_WINDOW_WIDTH;
    REQUIRE( new_tiles_w != 40 );

    // Redraw the resized map window, then flush: the frame follows the new
    // geometry instead of reusing the size cached anywhere from before.
    catacurses::mvwprintw( stream_ctx0.w_terrain, point::zero, "." );
    catacurses::wrefresh( stream_ctx0.w_terrain );
    flush_scene_under_baton( 0, 2 );
    const mp_scene after0 = decode_only_scene( drain_frames( *first ) );

    CHECK( after0.frame_seq == 2 );
    CHECK( after0.tiles_w == new_tiles_w );
    CHECK( after0.tiles_h == 30 );
    CHECK( after0.ui_x == sidebar_left );
    CHECK( after0.ui_y == 0 );
    CHECK( after0.ui_w == new_tiles_w );
    CHECK( after0.ui_h == 30 );
    CHECK( after0.view_dx == -new_tiles_w / 2 );
    CHECK( after0.view_dy == -15 );

    // The other seat's last frame and sequence are untouched by that resize.
    CHECK( count_of_kind( drain_frames( *second ), mp_msg_kind::scene ) == 0 );
    CHECK( mp_session_of_slot( 1 )->scene_seq == before1.frame_seq );
}

TEST_CASE( "mp_scene_stream_text_reconnect_clears_the_scene_stream", "[mp]" )
{
    restore_active_state restore;
    prepare_stream_world( stream_ctx0, 40, 10, point( 10, 0 ) );
    install_session( 0 );
    mp_session *const retained = mp_session_of_slot( 0 );
    REQUIRE( retained != nullptr );
    retained->card_save_id = "seat-card";
    retained->scene_caps = 1;
    retained->scene_seq = 7;
    retained->scene_hash = 42;
    retained->scene_mask_hash = 42;
    retained->scene_sent_once = true;
    retained->scene_terrain_dirty = false;
    mp_set_disconnected( 0, true );

    // The newcomer is a text client. The retained seat must forget the previous
    // client's capability and stream position, or it would send scene frames
    // nobody asked for.
    const std::shared_ptr<mp_transport> wire = admit_with_card( "seat-card", 0, 10 );
    CHECK( retained->scene_caps == 0 );
    CHECK( retained->scene_seq == 0 );
    CHECK( retained->scene_hash == 0 );
    CHECK( retained->scene_mask_hash == 0 );
    CHECK_FALSE( retained->scene_sent_once );
    CHECK( retained->scene_terrain_dirty );

    mp_set_active_context( &stream_ctx0 );
    mp_server_set_terminal_size( 80, 24 );
    flush_scene_under_baton( 0 );
    CHECK( count_of_kind( drain_frames( *wire ), mp_msg_kind::scene ) == 0 );
}

TEST_CASE( "mp_scene_stream_scene_reconnect_starts_at_frame_one", "[mp]" )
{
    restore_active_state restore;
    prepare_stream_world( stream_ctx0, 40, 10, point( 10, 0 ) );
    install_session( 0 );
    mp_session *const retained = mp_session_of_slot( 0 );
    REQUIRE( retained != nullptr );
    retained->card_save_id = "seat-card";
    retained->scene_caps = 0;
    retained->scene_seq = 7;
    retained->scene_hash = 42;
    retained->scene_mask_hash = 42;
    retained->scene_sent_once = true;
    retained->scene_terrain_dirty = false;
    mp_set_disconnected( 0, true );

    // A 3D client takes a seat that was text-only: it now gets the depth it
    // asked for, and the stream restarts rather than inheriting the old
    // seat's sequence or send state.
    const std::shared_ptr<mp_transport> wire = admit_with_card( "seat-card", 1, 3 );
    CHECK( retained->scene_caps == 1 );
    CHECK( retained->scene_z_levels == 3 );
    CHECK( retained->scene_seq == 0 );
    CHECK( retained->scene_hash == 0 );
    CHECK( retained->scene_mask_hash == 0 );
    CHECK_FALSE( retained->scene_sent_once );
    CHECK( retained->scene_terrain_dirty );

    mp_set_active_context( &stream_ctx0 );
    mp_server_set_terminal_size( 80, 24 );
    flush_scene_under_baton( 0 );
    const mp_scene scene = decode_only_scene( drain_frames( *wire ) );
    CHECK( scene.frame_seq == 1 );
    REQUIRE( scene.levels.size() == 3 );
    CHECK( scene.levels[0].z_rel == 0 );
    CHECK( scene.levels[2].z_rel == -2 );
}

#endif // MP_SERVER
