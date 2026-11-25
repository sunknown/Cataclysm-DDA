#if defined(MP_SERVER)

#include "mp/mp_server_screen.h"

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "avatar.h"
#include "cached_options.h"
#include "game.h"
#include "mp/mp_context.h"
#include "mp/mp_describe.h"
#include "mp/mp_protocol.h"
#include "mp/mp_scene.h"
#include "mp/mp_scene_codec.h"
#include "mp/mp_sched.h"
#include "mp/mp_screen_codec.h"
#include "mp/mp_session.h"
#include "mp/mp_session_ui.h"
#include "mp/mp_log.h"
#include "mp/mp_server_input.h"
#include "mp/mp_server_log.h"
#include "mp/mp_server_mask.h"
#include "mp/mp_transport.h"

// Defined in mp_server_input.cpp; the screen reset drops the input state too.
void mp_server_input_reset_for_test();

namespace
{

// Before any client is attached (startup, tests without a session) the game
// still asks for a grid to size stdscr from. The fallback keeps that path
// alive without pretending a session exists.
//
// Since mp_server_blit_row() writes through mp_server_grid(), session-less
// draws now land here instead of being discarded. That makes this process-wide
// mutable state shared by every participant, and its safety rests entirely on
// the scheduler's invariant that exactly one of them runs at a time. Nothing
// ever sends this grid to a client, so a mixture of two participants' draws
// would be invisible rather than a screen leak -- but the invariant is what
// keeps it coherent, not the fact that nobody reads it.
mp_screen_grid fallback_grid( 80, 24 );

} // namespace

mp_screen_grid &mp_server_grid()
{
    mp_session *const s = mp_active_session();
    return s ? s->grid : fallback_grid;
}

void mp_server_set_terminal_size( const int width, const int height )
{
    // resize() blanks the screen and marks every row dirty, so the next flush
    // always carries a full redraw.
    mp_server_grid().resize( width, height );
}

void mp_server_blank_screens()
{
    // No game screen is up, and the old sidebar rect is not a hint to keep.
    mp_native_log_clear_rect();
    for( const std::unique_ptr<mp_session> &session : mp_sessions() ) {
        if( !session ) {
            continue;
        }
        // Every seat leaves the UI, connected or not: a disconnected or
        // replacing seat must not keep a stale flag or hover request.
        mp_session_ui_leave( *session );
        if( !session->transport || !session->transport->connected() ) {
            continue;
        }
        // resize() discards the join menu's cells and marks every row dirty,
        // so the next flush carries a full blank redraw; the game's own draw
        // then fills the grid from the windows that outlive the menu.
        session->grid.resize( session->grid.width(), session->grid.height() );
        // The join menu may have left a cached cursor under text that is about
        // to disappear; hide it until the game sets its own.
        if( session->cursor_visible ) {
            session->cursor_visible = false;
            session->cursor_changed = true;
        }
    }
}

bool mp_server_peer_connected()
{
    if( mp_sessions().empty() ) {
        return false;
    }
    for( const std::unique_ptr<mp_session> &s : mp_sessions() ) {
        if( !s || !s->transport || !s->transport->connected() ) {
            // Whole-roster answer: true only while every seat's wire is up. A
            // player leaving no longer ends the session -- that seat is marked
            // disconnected and play continues -- so the caller pairs this with
            // the active seat's own session rather than reading false as its
            // own loss.
            return false;
        }
    }
    return true;
}

void mp_server_blit_row( const int x0, const int y, const std::vector<mp_cell> &cells )
{
    // Through mp_server_grid(), never through the session's grid directly: a
    // write has to land in the grid the matching read comes from. Reads already
    // fall back to fallback_grid when no session is active, so an early return
    // here made writes and reads disagree -- the row arrived, was dropped, and
    // the reader got a blank cell back out of a grid nobody had written to.
    mp_screen_grid &grid = mp_server_grid();
    for( size_t i = 0; i < cells.size(); ++i ) {
        // set() ignores out-of-range writes, which is exactly the clipping
        // curses applies when a window is drawn past the edge of the screen.
        grid.set( x0 + static_cast<int>( i ), y, cells[i] );
    }
}

void mp_server_set_cursor( const int x, const int y, const bool visible )
{
    mp_session *const s = mp_active_session();
    if( s == nullptr ) {
        // Deliberately session-only, unlike the blit above. The asymmetry that
        // bug was is a write with no matching read; the cursor has no read at
        // all without a session. The only reader of mp_session::cursor_x /
        // cursor_y / cursor_visible is mp_server_flush_screen(), which needs a
        // transport and therefore a session. (mp_client_state carries fields of
        // the same names, but those are the client's own copy, decoded off the
        // wire and read by the client -- src/mp/mp_client_main.cpp -- and have
        // nothing to do with this state.) A fallback cursor would be state that
        // can never be observed -- dead by construction -- so there is nothing
        // here to keep symmetric.
        return;
    }
    if( s->cursor_x == x && s->cursor_y == y && s->cursor_visible == visible ) {
        return;
    }
    s->cursor_x = x;
    s->cursor_y = y;
    s->cursor_visible = visible;
    s->cursor_changed = true;
}

void mp_server_flush_screen()
{
    mp_session *const s = mp_active_session();
    if( s == nullptr ) {
        return;
    }
    std::shared_ptr<mp_transport> t = s->transport;
    if( !t ) {
        return;
    }
    if( t->pending_bytes() > MP_SCREEN_BACKLOG_BYTES ) {
        // The peer is not keeping up. Throw away the queued screen and scene
        // history and mark the whole grid dirty so the send below becomes a
        // single full redraw; control messages survive the drop untouched.
        // The scene flush later in this call rebuilds one newest frame: a
        // dropped queued scene may have been the only one the client would
        // otherwise ever receive.
        t->drop_pending_screen_messages();
        s->grid.mark_all_dirty();
        s->scene_sent_once = false;
    }
    if( s->grid.any_dirty() ) {
        t->send( mp_wrap( mp_msg_kind::screen, mp_screen_encode( s->grid ) ) );
        s->grid.clear_dirty();
    }
    if( s->cursor_changed ) {
        mp_control msg;
        msg.kind = mp_control_kind::cursor;
        msg.cursor.x = s->cursor_x;
        msg.cursor.y = s->cursor_y;
        msg.cursor.visible = s->cursor_visible;
        const std::string json = mp_control_to_json( msg );
        t->send( mp_wrap( mp_msg_kind::control,
                          std::vector<uint8_t>( json.begin(), json.end() ) ) );
        s->cursor_changed = false;
    }

    if( ( s->caps & MP_CAP_LOG ) != 0 ) {
        // The snapshot is built once per active-seat flush, the same cadence at
        // which that seat's text screen is drawn. The panel check is here so a
        // toggled-off Log panel cannot hand the client a stale rect.
        mp_scene_rect log_rect;
        if( !mp_native_log_panel_enabled() || !mp_native_log_source_rect( log_rect ) ) {
            log_rect = {};
        }
        // The stable character name is the viewer identity. A blank slot in
        // the join menu has an empty name, which is the primary-text path.
        const std::string viewer_id = g->player_at( s->context_index ).name;
        const std::string body = mp_log_body( log_from_top, mp_session_ui_active( *s ),
                                              log_rect, s->log_seq + 1, viewer_id );
        const uint64_t hash = mp_log_hash( body );
        if( !s->log_sent_once || hash != s->log_hash ) {
            s->log_seq++;
            s->log_sent_once = true;
            s->log_hash = hash;
            t->send( mp_wrap( mp_msg_kind::control,
                              std::vector<uint8_t>( body.begin(), body.end() ) ) );
        }
    }

    // After the grid, so a client can paint the scene under the text it just
    // received. The gates live in the scene flush: the world phase, the join
    // loop (which flushes under the world's baton) and the legacy path all
    // reach here and must leave without building.
    mp_server_flush_scene();

    // The hover answer is last, so the client draws it over the scene it
    // names: `frame` is this seat's scene sequence as it stands after the
    // scene block above. A seat that left the UI, lost the capability, or
    // never had it loses the request here rather than being served text for a
    // screen it is not looking at. Every read and write of the pending request
    // and the interval goes through the input module's locked helpers: the
    // world thread dispatches a hover under the same lock, so touching the
    // fields bare here would be a data race.
    if( !mp_session_can_describe( *s ) ) {
        mp_server_describe_drop_pending( *s );
        return;
    }
    if( !mp_server_describe_pending_due( *s ) ) {
        // Nothing asked, or the previous answer is still inside the interval
        // that stops a peer from driving the build rate. The request stays
        // pending for a later flush.
        return;
    }
    mp_sched *const sched = mp_active_sched();
    // The baton check is the same one the scene flush uses -- the join
    // loop and the world phase draw for a seat that is not entitled to run.
    if( sched != nullptr && !sched->holds( s->context_index ) ) {
        return;
    }
    if( t->pending_bytes() > MP_SCREEN_BACKLOG_BYTES ) {
        // The peer is not draining its socket. A hover answer is a snapshot of
        // the instant the pointer rested, so replaying it after the client
        // catches up would describe a view that is already gone. The request
        // is dropped rather than deferred; the client asks again on its next
        // hover, and its latest request is the only one either way.
        mp_server_describe_drop_pending( *s );
        return;
    }
    mp_describe_request req;
    if( !mp_server_describe_take_due( *s, req ) ) {
        // A newer request won between the check above and this take and is not
        // due yet, or another thread consumed it: the next flush decides.
        return;
    }
    mp_control msg;
    msg.kind = mp_control_kind::describe;
    msg.describe = mp_describe_reply( req.seq, s->scene_seq, req.x, req.y, req.w );
    const std::string json = mp_control_to_json( msg );
    t->send( mp_wrap( mp_msg_kind::control,
                      std::vector<uint8_t>( json.begin(), json.end() ) ) );
}

void mp_server_flush_scene()
{
    mp_session *const s = mp_active_session();
    if( s == nullptr || !s->transport ) {
        return;
    }
    if( ( s->caps & MP_CAP_SCENE ) == 0 ) {
        // A text client. Nothing below this point may run for it.
        return;
    }
    mp_sched *const sched = mp_active_sched();
    if( sched == nullptr ) {
        // No baton exists in single player or in a legacy session.
        return;
    }
    if( !sched->holds( mp_sched::world ) && !sched->holds( s->context_index ) ) {
        // Only the baton holder may send a frame for the active context: the
        // seat itself, or the world building every seat's frame in
        // mp_server_flush_scenes_all(). Anything else is drawing for a seat
        // that is not entitled to run.
        return;
    }

    const player_context *const ctx = mp_active_context_ptr();
    if( ctx == nullptr ) {
        return;
    }
    // The viewport and the map size are the active context's terrain window,
    // read fresh on every flush and never cached on the session.
    mp_scene_rect rect;
    if( !mp_scene_mask_map_rect( rect ) ) {
        return;
    }
    if( rect.w == 0 || rect.h == 0 ) {
        return;
    }

    const uint64_t mask_hash = mp_scene_mask_hash();
    const bool need = s->scene_terrain_dirty || s->scene_mask_hash != mask_hash ||
                      !s->scene_sent_once;
    if( !need ) {
        return;
    }

    mp_scene scene = mp_scene_build( ctx->ter_view_p, rect.w, rect.h,
                                     s->scene_z_levels );
    if( scene.levels.empty() ) {
        // The join phase hands the baton to a seat whose avatar is still the
        // blank sentinel at (20, 10, -500). The builder anchors there and runs
        // out of map at once, returning no levels; encoding that would put a
        // z_count == 0 body on the wire, which the project's decoder rejects.
        // Leave the send state untouched so the first real frame still goes out
        // after the character is placed.
        return;
    }
    scene.ui_x = rect.x;
    scene.ui_y = rect.y;
    scene.ui_w = rect.w;
    scene.ui_h = rect.h;
    scene.masks = mp_scene_mask_rects();
    scene.frame_seq = s->scene_seq + 1;

    const std::vector<uint8_t> body = mp_scene_encode( scene );
    const uint64_t hash = mp_scene_body_hash( body );
    if( s->scene_sent_once && hash == s->scene_hash ) {
        // Nothing the client would see changed, so no frame is sent and the
        // sequence stays put. The blit that forced this build has still been
        // accounted for by it: leaving the dirty flag set would rebuild the
        // whole scene on every idle flush for a frame the hash already proved
        // unchanged.
        s->scene_terrain_dirty = false;
        return;
    }

    s->scene_seq = scene.frame_seq;
    s->transport->send( mp_wrap( mp_msg_kind::scene, body ) );
    s->scene_hash = hash;
    s->scene_mask_hash = mask_hash;
    s->scene_sent_once = true;
    // A completed build consumes the blit, sent or skipped.
    s->scene_terrain_dirty = false;
}

void mp_server_flush_scenes_all()
{
    mp_sched *const sched = mp_active_sched();
    if( sched == nullptr ) {
        // No baton exists in single player or in a legacy session, and there
        // is nobody parked to send for.
        return;
    }
    // One seat at a time, each under its own context: a frame's rect and view
    // centre belong to the seat it is sent to, and a parked seat has no thread
    // of its own running to build it. The per-seat dirty/hash gate inside
    // mp_server_flush_scene() keeps a round with no visual change silent.
    for( size_t slot = 0; slot < g->player_slot_count(); ++slot ) {
        if( mp_session_of_slot( slot ) == nullptr ) {
            continue;
        }
        mp_activate_player( slot );
        mp_server_flush_scene();
    }
    // The caller is the world: put the process back in the phase it entered
    // this function in, never on the last seat visited.
    mp_activate_world();
}

void mp_server_reset_for_test()
{
    fallback_grid.resize( 80, 24 );
    mp_server_input_reset_for_test();
    mp_server_log_reset_for_test();
}

std::shared_ptr<mp_transport> mp_server_shared_transport()
{
    mp_session *const s = mp_active_session();
    if( s == nullptr ) {
        return nullptr;
    }
    return s->transport;
}

#endif // MP_SERVER
