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
#include "cursesdef.h"
#include "game.h"
#include "game_constants.h"
#include "game_test_helper.h"
#include "map.h"
#include "map_helpers.h"
#include "map_helpers_tests.h"
#include "mp/mp_context.h"
#include "mp/mp_loopback_transport.h"
#include "mp/mp_protocol.h"
#include "mp/mp_scene.h"
#include "mp/mp_scene_codec.h"
#include "mp/mp_sched.h"
#include "mp/mp_server_input.h"
#include "mp/mp_server_screen.h"
#include "mp/mp_session.h"
#include "mp/mp_transport.h"
#include "player_helpers.h"
#include "point.h"
#include "type_id.h"

namespace
{

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

    flush_scene_under_baton( 0 );
    const std::vector<wire_frame> first = drain_frames( *wire );
    const mp_scene sent = decode_only_scene( first );
    CHECK( sent.frame_seq == 1 );
    CHECK_FALSE( session->scene_terrain_dirty );

    flush_scene_under_baton( 0 );
    const std::vector<wire_frame> second = drain_frames( *wire );
    CHECK( count_of_kind( second, mp_msg_kind::scene ) == 0 );
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

#endif // MP_SERVER
