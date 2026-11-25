#if defined(MP_SERVER)

#include "cata_catch.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
#endif

#include "calendar.h"
#include "cata_scope_helpers.h"

#include "activity_actor_definitions.h"
#include "avatar.h"
#include "bodypart.h"
#include "catacharset.h"
#include "debug.h"
#include "filesystem.h"
#include "game.h"
#include "game_test_helper.h"
#include "input.h"
#include "item.h"
#include "line.h"
#include "mp_session_harness.h"
#include "map.h"
#include "map_helpers.h"
#include "map_helpers_tests.h"
#include "messages.h"
#include "monster.h"
#include "options_helpers.h"
#include "output.h"
#include "path_info.h"
#include "worldfactory.h"
#include "mp/mp_context.h"
#include "mp/mp_loopback_transport.h"
#include "mp/mp_plan.h"
#include "mp/mp_protocol.h"
#include "mp/mp_scene.h"
#include "mp/mp_sched.h"
#include "mp/mp_server_main.h"
#include "mp/mp_server_mask.h"
#include "mp/mp_session.h"
#include "mp/mp_session_ui.h"
#include "mp/mp_tether.h"
#include "mp/mp_turn.h"
#include "overmapbuffer.h"
#include "overmap.h"
#include "player_difficulty.h"
#include "player_helpers.h"
#include "point.h"
#include "string_formatter.h"
#include "trap.h"
#include "type_id.h"
#include "ui_manager.h"
#include "units.h"

static const vproto_id vehicle_prototype_test_shopping_cart( "test_shopping_cart" );

namespace
{
// Hands the round loop the message a seat's reader would have queued. The wire
// path itself -- a control frame parsed and routed into the seat's inbox -- is
// covered by mp_server_io_test ("a_plan_frame_reaches_the_world_without_the
// _baton"); a session test drives the loop with the message the reader
// delivers, so it is about what the loop does with a plan, not about a second
// protocol client.
void queue_control( const size_t slot, const mp_control &msg )
{
    mp_session *const session = mp_session_of_slot( slot );
    REQUIRE( session != nullptr );
    {
        std::lock_guard<std::mutex> lock( session->inbox_mutex );
        session->control_inbox.push_back( msg );
    }
    session->inbox_cv.notify_all();
}

void queue_plan( const size_t slot, const mp_plan_kind kind, const int cell_x = 0,
                 const int cell_y = 0, const uint32_t seq = 1 )
{
    mp_control msg;
    msg.kind = mp_control_kind::plan;
    msg.plan.kind = kind;
    msg.plan.x = cell_x;
    msg.plan.y = cell_y;
    msg.plan.seq = seq;
    queue_control( slot, msg );
}

void queue_plan_only( const size_t slot, const mp_control_kind kind )
{
    mp_control msg;
    msg.kind = kind;
    queue_control( slot, msg );
}

void queue_plan_ready( const size_t slot )
{
    queue_plan_only( slot, mp_control_kind::plan_ready );
}

void queue_plan_cancel( const size_t slot )
{
    queue_plan_only( slot, mp_control_kind::plan_cancel );
}

void queue_plan_pass( const size_t slot )
{
    queue_plan_only( slot, mp_control_kind::plan_pass );
}

// The scene cell a plan must name for `slot` to reach `tile`. A plan cell is a
// rect cell: a 0-based offset into the seat's own terrain frame, which is what
// the shipped client sends (its avatar cell plus the key's delta). This inverts
// the server's translation -- origin plus the cell -- rather than duplicating
// the terminal-space describe lookup, whose cells also carry the rect's
// position on the terminal.
std::optional<point> plan_cell_for( const size_t slot, const tripoint_bub_ms &tile )
{
    mp_activate_player( slot );
    mp_scene_rect rect;
    const bool have_rect = mp_scene_mask_map_rect( rect );
    const tripoint_bub_ms view = mp_active_context().ter_view_p;
    mp_activate_world();
    if( !have_rect ) {
        return std::nullopt;
    }
    const tripoint_bub_ms origin = mp_scene_origin( rect, view );
    const int x = tile.x() - origin.x();
    const int y = tile.y() - origin.y();
    if( x < 0 || y < 0 || x >= rect.w || y >= rect.h ) {
        return std::nullopt;
    }
    return point( x, y );
}

// Without shoes run_cost adds 16 to a plain step, so the "100 AP for a step on
// pavement" arithmetic (mp_plan_test, mp_resolve_test) only holds for a
// normally shod character.
const itype_id itype_sneakers( "sneakers" );

// The mp_plan_test baseline applied to a running two-seat session: plain
// pavement cannot be assumed from whatever case ran before, and neither can
// the avatars' clothes, so both are reset here. A step then costs exactly 100
// AP, which is what makes the AP arithmetic in these cases exact rather than
// approximate. Each seat then draws once -- a free zoom key, which opens no
// menu and spends nothing -- because a plan's cells are measured in the terrain
// window that seat's own draw produces. The caller owns restore_active_state.
void prepare_two_seat_round( mp_session_harness &harness, const int moves )
{
    // clear_character() parks every test character on the same spot (60,60),
    // so the harness's two-seat layout is read first and put back afterwards.
    // The cases below need two seats at two positions: both for their AP
    // arithmetic and for the party anchor, which must be each seat's own.
    const tripoint_bub_ms seat0_pos = g->player_at( 0 ).pos_bub();
    const tripoint_bub_ms seat1_pos = g->player_at( 1 ).pos_bub();
    REQUIRE( seat0_pos != seat1_pos );
    clear_fields( 0 );
    build_test_map( ter_id( "t_pavement" ) );
    for( size_t slot = 0; slot < 2; ++slot ) {
        clear_character( g->player_at( slot ) );
        g->player_at( slot ).setpos( get_map(), slot == 0 ? seat0_pos : seat1_pos );
        g->player_at( slot ).wear_item( item( itype_sneakers ) );
        g->player_at( slot ).set_moves( moves );
    }
    harness.press( 0, 'z' );
    harness.press( 1, 'z' );
    REQUIRE( harness.run_until( []() {
        return g->contexts[0].w_terrain && g->contexts[1].w_terrain &&
               mp_turn_is_open() && mp_plan_phase() == mp_round_phase::planning;
    }, 20 ) );
}

struct scene_anchor_point {
    int x = 0;
    int y = 0;

    bool operator!=( const scene_anchor_point &other ) const {
        return x != other.x || y != other.y;
    }
};

// The anchor the party header carries for `slot`: that seat's scene origin in
// bubble coordinates, computed from the same rect and view centre the broadcast
// reads. The seat is activated to read its own context and the world is put
// back afterwards, as the broadcast does.
std::optional<scene_anchor_point> party_anchor_for( const size_t slot )
{
    mp_activate_player( slot );
    mp_scene_rect rect;
    const bool have_rect = mp_scene_mask_map_rect( rect );
    std::optional<scene_anchor_point> result;
    if( have_rect ) {
        const tripoint_bub_ms origin = mp_scene_origin( rect, mp_active_context().ter_view_p );
        result = scene_anchor_point{ origin.x(), origin.y() };
    }
    mp_activate_world();
    return result;
}

} // namespace


TEST_CASE( "a_server_debugmsg_reaches_every_player", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();

    // In test_mode debugmsg is already non-blocking, so "does it block" is not
    // observable from a test. What IS observable is the behaviour that replaces
    // the modal window: the error must land in the message log. A build that
    // still opened a window would put it nowhere.
    //
    // The log is process-wide (src/messages.h has no per-player split), so one
    // add_msg reaches everybody and a second would be a duplicate. Per-player
    // message logs are not part of this phase.
    Messages::clear_messages();
    mp_activate_world();
    debugmsg( "a server debugmsg must reach the players" );
    // The call above is intentional, but a bare debugmsg logs at D_ERROR and
    // the harness fails the whole run on any observed error (test_main.cpp).
    // Clear the flag so this test does not fail its own shard.
    debug_reset_error_observed();

    const std::vector<std::pair<std::string, std::string>> log = Messages::recent_messages( 5 );
    int hits = 0;
    for( const auto &entry : log ) {
        hits += entry.second.find( "must reach the players" ) != std::string::npos ? 1 : 0;
    }
    CHECK( hits == 1 );

    mp_activate_player( 0 );
    Messages::clear_messages();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

// The regression guard for the render pipeline itself. The two tests below it
// would notice if the pipeline stopped reaching the grid, but each can fail for
// reasons that are not the pipeline's -- the menus' own teardown, and the hurry
// flag -- so neither can be relied on to report it. This one asserts the thing
// those two only imply.
//
// It also covers the teardown of a single player left inside a menu, which was
// an abort before mp_sched's unwind right (mp_sched.cpp's release_to assert):
// a thread parked INSIDE player_act() wakes on `stopping_` without the baton,
// and its thread body used to release a baton it did not hold. One parked-in-
// menu thread was enough -- no concurrency involved. Do not "fix" a failure
// here by closing the menu with a keystroke first: that dodges the very case
// this leaves open at teardown.
TEST_CASE( "one_players_menu_reaches_their_own_client_screen", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 1 );

    // The harness constructor zeroes every slot's moves, which reads as "already
    // acted": the barrier is satisfied before anyone is handed the baton, so no
    // player_act runs, the key is never read and no menu opens. Give the moves
    // back so this player is actionable this turn.
    g->player_at( 0 ).set_moves( 100 );
    // '@' opens the character sheet, which carries the character's own name.
    // Nothing else on this screen does -- the sidebar of the main screen behind
    // it shows no name at this terminal size. So the name proves the claim
    // the phase rests on: content specific to this player travelled the whole
    // pipeline to this player's client grid. A pipeline that discards writes
    // cannot produce it, and one that carried some other player's screen would
    // not carry this name.
    harness.press( 0, '@' );
    REQUIRE( harness.run_until( [&]() {
        return mp_grid_contains( harness.screen( 0 ), "P0" );
    } ) );

    const mp_screen_grid &screen = harness.screen( 0 );
    CHECK( mp_grid_contains( screen, "P0" ) );
    // A rendered character sheet, not one stray cell. It measured 944 non-blank
    // cells when the pipeline was first made to work at this terminal size; the
    // floor is set far below that so ordinary UI churn does not trip it, and far
    // above what any partial write could reach.
    CHECK( mp_grid_nonblank_cells( screen ) > 200 );
}

TEST_CASE( "every_player_sees_their_own_main_game_screen", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );

    // Both actionable. A seat draws when the world hands it the baton, and the
    // baton is owed for a queued key, so each player presses a free one (zoom,
    // which opens no menu and spends nothing) and then sits on the main screen
    // waiting for their next input -- what a client sees for most of a session.
    // The sidebar does not carry the character's name at this size, but it
    // does carry their strength, so give the two players far-apart ones. The
    // text is read off the character rather than written here: the sidebar
    // shows strength after weight and other penalties.
    g->player_at( 0 ).set_moves( 100 );
    g->player_at( 1 ).set_moves( 100 );
    g->player_at( 1 ).set_str_base( 20 );
    harness.press( 0, 'z' );
    harness.press( 1, 'z' );
    const auto strength = []( const size_t slot ) {
        return string_format( "Str: %d ", g->player_at( slot ).get_str() );
    };
    REQUIRE( harness.run_until( [&]() {
        return mp_grid_contains( harness.screen( 0 ), strength( 0 ) ) &&
               mp_grid_contains( harness.screen( 1 ), strength( 1 ) );
    }, 10 ) );
    REQUIRE( strength( 0 ) != strength( 1 ) );
    // A whole screen, not a stray line: the map and the sidebar drew about 500
    // cells each when this was first made to work at the harness's 80x24.
    CHECK( mp_grid_nonblank_cells( harness.screen( 0 ) ) > 200 );
    CHECK( mp_grid_nonblank_cells( harness.screen( 1 ) ) > 200 );
    // And each player's own: the other's sidebar never lands on it.
    CHECK_FALSE( mp_grid_contains( harness.screen( 0 ), strength( 1 ) ) );
    CHECK_FALSE( mp_grid_contains( harness.screen( 1 ), strength( 0 ) ) );
}

namespace
{
// One of the heavy menus: the key that opens it from the main game screen,
// and text that appears on its screen and on no other menu's here. The texts
// were read off the menus as this harness renders them, at its 80x24.
struct mp_heavy_menu {
    int key;
    const char *marker;
};

const mp_heavy_menu mp_menu_inventory{ 'i', "Item hotkeys assigned" };
const mp_heavy_menu mp_menu_character_sheet{ '@', "ENCUMBRANCE" };
const mp_heavy_menu mp_menu_crafting{ '&', "Craft, D[e]scribe" };
const mp_heavy_menu mp_menu_map{ 'm', "Quick Reference" };
// Examining the one vehicle next to the player auto-selects it and opens the
// vehicle's action menu (a uilist, so this is also the case that has to unwind
// through uilist's own cancel at teardown).
const mp_heavy_menu mp_menu_vehicle{ 'e', "Examine vehicle" };
const mp_heavy_menu mp_menu_construction{ '*', "Construction" };
} // namespace

// The six menus the design names as the heavy ones (inventory, crafting, map,
// character sheet, vehicle, construction): each is a different UI path, and a
// global leaked by any of them shows up as one player seeing the other's
// screen. The keys are the DEFAULTMODE defaults, and every one of them only
// opens its menu, so the planning gate lets it through (mp_action_is_free).
TEST_CASE( "two_players_sit_in_different_heavy_menus", "[mp]" )
{
    restore_active_state restore;
    struct menu_case {
        const char *name;
        mp_heavy_menu first;
        mp_heavy_menu second;
    };
    const std::vector<menu_case> menus = {
        { "inventory vs character sheet", mp_menu_inventory, mp_menu_character_sheet },
        { "crafting vs map", mp_menu_crafting, mp_menu_map },
        { "vehicle vs construction", mp_menu_vehicle, mp_menu_construction },
    };

    for( const menu_case &c : menus ) {
        CAPTURE( c.name );
        // The cart outlives the harness; nothing after this test expects it.
        on_out_of_scope remove_vehicles( []() {
            clear_vehicles();
        } );
        mp_session_harness harness( 2 );
        if( c.first.key == mp_menu_vehicle.key ) {
            // Something to examine: without a vehicle in reach the key opens
            // nothing, and the case would compare a menu with a blank screen.
            REQUIRE( get_map().add_vehicle( vehicle_prototype_test_shopping_cart,
                                            g->player_at( 0 ).pos_bub() + point::east,
                                            0_degrees ) != nullptr );
        }

        // The harness zeroes every slot's moves, which reads as "already acted":
        // the round would settle before either player is handed the baton, and
        // no key would be read. Give both their moves back so both are
        // actionable. Neither key spends time, so both players hold nothing and
        // both stay live -- which is the whole point of the per-player context.
        g->player_at( 0 ).set_moves( 100 );
        g->player_at( 1 ).set_moves( 100 );
        harness.press( 0, c.first.key );
        harness.press( 1, c.second.key );
        REQUIRE( harness.run_until( [&]() {
            return mp_grid_contains( harness.screen( 0 ), c.first.marker ) &&
                   mp_grid_contains( harness.screen( 1 ), c.second.marker );
        } ) );

        // Each player sees their own menu, and nothing of the other's. A
        // leaked global shows one player the other's screen -- the main class
        // of bug this design can have -- and so does a draw that lands in the
        // wrong session's grid.
        CHECK_FALSE( mp_grid_contains( harness.screen( 0 ), c.second.marker ) );
        CHECK_FALSE( mp_grid_contains( harness.screen( 1 ), c.first.marker ) );
    }
}

// The overmap is the one heavy screen that outlives its input loop: with fast
// travel it keeps a ui_adaptor alive while the turn runs, and two seats can
// hold one open at once. That adaptor used to be a single game-wide shared_ptr,
// so the first seat to close ran force_quit() and nulled the pointer under its
// own ui stack. The other seat's next redraw then tripped draw()'s
// `cata_assert( data.ui )` (on the retained fast-travel path, with no local
// copy, ~ui_adaptor's owner == active_stack instead). The adaptor now belongs
// to the player context, so closing one seat's map leaves the other's alone.
TEST_CASE( "two_players_can_hold_the_overmap_open_at_once", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    // Both actionable: opening and reading the map costs no moves, so both
    // stay live and each keeps their own overmap parked at its input wait.
    g->player_at( 0 ).set_moves( 100 );
    g->player_at( 1 ).set_moves( 100 );
    harness.press( 0, mp_menu_map.key );
    harness.press( 1, mp_menu_map.key );
    REQUIRE( harness.run_until( [&]() {
        return mp_grid_contains( harness.screen( 0 ), mp_menu_map.marker ) &&
               mp_grid_contains( harness.screen( 1 ), mp_menu_map.marker );
    } ) );

    // Seat 1 closes its map. Seat 0 keeps reading, and further turns make its
    // retained adaptor redraw under seat 0's own stack.
    harness.press( 1, KEY_ESCAPE );
    REQUIRE( harness.run_until( [&]() {
        return !mp_grid_contains( harness.screen( 1 ), mp_menu_map.marker );
    }, 20 ) );
    // A key wakes seat 0's parked overmap loop, which redraws before reading
    // it. The pre-fix build nulled the shared adaptor above, so this redraw is
    // where the other seat notices.
    harness.press( 0, KEY_RIGHT );
    harness.run_until( []() {
        return false;
    }, 3 );
    CHECK( mp_grid_contains( harness.screen( 0 ), mp_menu_map.marker ) );

    // And the other order: reopen seat 1's map, then seat 0 closes while seat 1
    // still has it up.
    harness.press( 1, mp_menu_map.key );
    REQUIRE( harness.run_until( [&]() {
        return mp_grid_contains( harness.screen( 1 ), mp_menu_map.marker ) &&
               mp_grid_contains( harness.screen( 0 ), mp_menu_map.marker );
    }, 20 ) );
    harness.press( 0, KEY_ESCAPE );
    REQUIRE( harness.run_until( [&]() {
        return !mp_grid_contains( harness.screen( 0 ), mp_menu_map.marker );
    }, 20 ) );
    // Same check in the mirror direction: seat 1's retained map must survive
    // seat 0's force_quit().
    harness.press( 1, KEY_RIGHT );
    harness.run_until( []() {
        return false;
    }, 3 );
    CHECK( mp_grid_contains( harness.screen( 1 ), mp_menu_map.marker ) );
}

// The overmap's view -- where the map is centred and where the cursor is -- is
// per player too. It used to be one process-wide overmap_draw_data_t, so one
// seat's movement keys moved the other seat's crosshair and re-centred its map.
// The cursor's coordinates are drawn in the sidebar, which makes the leak
// visible on the other seat's screen after it redraws.
TEST_CASE( "two_players_overmaps_do_not_share_a_cursor", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    // Opening and reading the map costs no moves, so both seats stay live and
    // each keeps its own overmap parked at its input wait.
    g->player_at( 0 ).set_moves( 100 );
    g->player_at( 1 ).set_moves( 100 );

    // The cursor starts on the player's own OMT, which the sidebar prints.
    const tripoint_abs_omt seat1_origin = g->player_at( 1 ).pos_abs_omt();
    tripoint_abs_omt seat1_one_right = seat1_origin;
    seat1_one_right.x() += 1;
    tripoint_abs_omt seat1_shifted_by_seat0 = seat1_origin;
    seat1_shifted_by_seat0.x() += 4;
    REQUIRE( seat1_origin != seat1_one_right );

    harness.press( 0, mp_menu_map.key );
    harness.press( 1, mp_menu_map.key );
    REQUIRE( harness.run_until( [&]() {
        return mp_grid_contains( harness.screen( 0 ), mp_menu_map.marker ) &&
               mp_grid_contains( harness.screen( 1 ), mp_menu_map.marker ) &&
               mp_grid_contains( harness.screen( 1 ), seat1_origin.to_string() );
    } ) );

    // Move only seat 0's cursor. On a shared view these three presses land on
    // seat 1's cursor too, because seat 1 opened its map last and the open
    // re-centres the one shared cursor on that seat's position.
    for( int i = 0; i < 3; ++i ) {
        harness.press( 0, KEY_RIGHT );
        harness.run_until( []() {
            return false;
        }, 2 );
    }

    // Seat 1 presses right once: that both wakes its parked loop (so its screen
    // is redrawn from the current view) and gives a deterministic expected
    // cursor. On a per-seat view it moves one step from its own origin; on the
    // shared view it continues seat 0's three presses.
    harness.press( 1, KEY_RIGHT );
    // The key is granted on a poll of its own and the redrawn sidebar goes out
    // with the seat's next flush; a fixed number of polls is a grant-count
    // assumption, and the menu-state traffic now shares those polls. Wait on
    // the observable the case is about. A shared cursor shows the shifted cell
    // and never one_right, so this still fails on the bug it guards.
    const bool moved = harness.run_until( [&]() {
        return mp_grid_contains( harness.screen( 1 ), seat1_one_right.to_string() );
    }, 20 );
    CAPTURE( seat1_origin.to_string(), seat1_one_right.to_string(),
             seat1_shifted_by_seat0.to_string() );
    // The shared cursor's position must not reach this seat's screen: that is
    // the leak the manual pass saw.
    CHECK_FALSE( mp_grid_contains( harness.screen( 1 ), seat1_shifted_by_seat0.to_string() ) );
    CHECK( moved );
}

// The teardown half of the same ownership question: a session can end while a
// seat is still in the overmap, and its ui_adaptor must die under that seat's
// stack rather than whichever stack the shutdown happens to run under.
TEST_CASE( "a_session_can_end_with_the_overmap_open", "[mp]" )
{
    restore_active_state restore;
    {
        mp_session_harness harness( 2 );
        g->player_at( 0 ).set_moves( 100 );
        g->player_at( 1 ).set_moves( 100 );
        harness.press( 0, mp_menu_map.key );
        REQUIRE( harness.run_until( [&]() {
            return mp_grid_contains( harness.screen( 0 ), mp_menu_map.marker );
        } ) );
        // The harness destructor unwinds the session with seat 0 still inside
        // display(). Reaching the end of this scope is the assertion.
    }
    SUCCEED( "teardown with an overmap open did not abort" );
}

// A player reading a menu with AP left has not said they are done: their seat
// can still act, so the round cannot settle for however long the partner is
// willing to wait. The partner's pass is not a substitute for this seat's own.
TEST_CASE( "a_player_in_a_menu_holds_the_round", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );

    g->player_at( 0 ).set_moves( 100 );
    g->player_at( 1 ).set_moves( 100 );
    harness.press( 1, mp_menu_inventory.key );
    REQUIRE( harness.run_until( [&]() {
        return mp_grid_contains( harness.screen( 1 ), mp_menu_inventory.marker );
    } ) );

    // Seat 0 is done for the tick; seat 1 is in a menu with AP and no plan.
    // Ten polls take about a second, and the round must still be open at the
    // end of them: a menu is a player thinking, not a player gone.
    const int turn_before = to_turn<int>( calendar::turn );
    queue_plan_pass( 0 );
    CHECK_FALSE( harness.run_until( []() {
        return false;
    }, 10 ) );
    CHECK( mp_turn_is_open() );
    CHECK_FALSE( mp_turn_round_complete() );
    CHECK( mp_plan_is_passed( 0 ) );
    CHECK( to_turn<int>( calendar::turn ) == turn_before );
    CHECK( mp_grid_contains( harness.screen( 1 ), mp_menu_inventory.marker ) );

    // Seat 1 closes the menu and passes: now nobody can act, the round settles
    // and the tick closes -- the world was waiting for this seat, not for the
    // clock.
    harness.press( 1, KEY_ESCAPE );
    REQUIRE( harness.run_until( [&]() {
        return !mp_grid_contains( harness.screen( 1 ), mp_menu_inventory.marker );
    }, 20 ) );
    queue_plan_pass( 1 );
    CHECK( harness.run_until( [turn_before]() {
        return to_turn<int>( calendar::turn ) > turn_before;
    } ) );
}

TEST_CASE( "another_players_key_does_not_close_a_mouse_view", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );

    // Both players' main screens must be up, so the terrain window has its
    // size. A seat draws when it is granted the baton, and a free key is what
    // buys that grant; the mouse position below is measured in that window.
    g->player_at( 0 ).set_moves( 100 );
    g->player_at( 1 ).set_moves( 100 );
    harness.press( 0, 'z' );
    harness.press( 1, 'z' );
    REQUIRE( harness.run_until( [&]() {
        return mp_grid_nonblank_cells( harness.screen( 0 ) ) > 200 &&
               mp_grid_nonblank_cells( harness.screen( 1 ) ) > 200;
    }, 10 ) );
    // A cell inside the map, not in the sidebar: the mouse view only shows for
    // a position inside the terrain window (get_coordinates() returns nullopt
    // otherwise and the hint is hidden).
    const catacurses::window terrain = mp_active_context().w_terrain;
    REQUIRE( terrain );
    const point on_map( getbegx( terrain ) + getmaxx( terrain ) / 2,
                        getbegy( terrain ) + getmaxy( terrain ) / 2 );

    // Player 1 moves the mouse over the map. A TIMEOUT does not hide the hint,
    // so it stays up while they sit at the input wait.
    harness.mouse_move( 1, on_map );
    REQUIRE( harness.run_until( [&]() {
        return mp_grid_contains( harness.screen( 1 ), "Mouse view" );
    } ) );
    // Their own screen only: the hint is not a process-wide overlay.
    CHECK_FALSE( mp_grid_contains( harness.screen( 0 ), "Mouse view" ) );

    // Player 1's terminal goes away with the hint still up...
    harness.drop_client( 1 );

    // ...and player 0 ends their round. Player 0 has no hint of their own, and
    // player 1's adaptor registered in player 1's stack: one shared live_view
    // would destroy it here, under player 0's stack, and abort on
    // ~ui_adaptor's owner assert. Play has to continue instead.
    const int turn_before = to_turn<int>( calendar::turn );
    queue_plan_pass( 0 );
    REQUIRE( harness.run_until( [turn_before]() {
        return to_turn<int>( calendar::turn ) > turn_before;
    }, 20 ) );
    CHECK( harness.client_connected( 0 ) );
}

TEST_CASE( "a_reconnected_seat_does_not_keep_the_old_terminals_mouse_hint", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );

    // Both terminals must have drawn before either seat's viewport geometry is
    // used for the coordinate: the active context here is seat 0's, and only
    // identical harness terminals make its window a valid target for seat 1.
    // A seat draws when it is granted the baton, and a free key is what buys
    // that grant.
    g->player_at( 0 ).set_moves( 100 );
    g->player_at( 1 ).set_moves( 100 );
    harness.press( 0, 'z' );
    harness.press( 1, 'z' );
    REQUIRE( harness.run_until( [&]() {
        return mp_grid_nonblank_cells( harness.screen( 0 ) ) > 200 &&
               mp_grid_nonblank_cells( harness.screen( 1 ) ) > 200;
    }, 10 ) );
    const catacurses::window terrain = mp_active_context().w_terrain;
    REQUIRE( terrain );
    const point on_map( getbegx( terrain ) + getmaxx( terrain ) / 2,
                        getbegy( terrain ) + getmaxy( terrain ) / 2 );
    const std::string save_id = g->player_at( 1 ).get_save_id();

    // Player 1 leaves the hint up on the input wait, then their terminal dies.
    harness.mouse_move( 1, on_map );
    REQUIRE( harness.run_until( [&]() {
        return mp_grid_contains( harness.screen( 1 ), "Mouse view" );
    } ) );
    harness.drop_client( 1 );
    // The seat's own thread notices the closed wire, not the test thread: wait
    // for the server to mark it before offering the card, as every other
    // reconnect test does.
    REQUIRE( harness.run_until( [&]() {
        return mp_player_is_disconnected( 1 );
    }, 10 ) );

    // The same character comes back on a new terminal. Its grid was reset by
    // the reconnect, so the first frame it receives is the first chance to see
    // whether the old terminal's hint survived the handover. A seat is granted
    // the baton for a queued key, so the fresh wire needs one (a free zoom,
    // which opens no menu) before it draws at all.
    REQUIRE( harness.reconnect_client( 1, save_id ) );
    CHECK( g->contexts[1].liveview == nullptr );
    harness.press( 1, 'z' );
    REQUIRE( harness.run_until( [&]() {
        return mp_grid_nonblank_cells( harness.screen( 1 ) ) > 200;
    }, 40 ) );
    CHECK_FALSE( mp_grid_contains( harness.screen( 1 ), "Mouse view" ) );
}

TEST_CASE( "a_yes_no_prompt_unwinds_when_the_session_ends", "[mp]" )
{
    restore_active_state restore;
    {
        mp_session_harness harness( 2 );
        g->player_at( 0 ).set_moves( 100 );
        // 'S' asks "Save and quit?" through query_yn, the prompt most
        // confirmations in the game share. Its loop asks again on every
        // timeout, and a session ending answers a waiting player with one.
        harness.press( 0, 'S' );
        REQUIRE( harness.run_until( [&]() {
            return mp_grid_contains( harness.screen( 0 ), "Save and quit" );
        } ) );
    }
    // Reaching the end of this test is the assertion: the harness's destructor
    // joins every player thread, and a prompt that never closes holds that join
    // for ever.
}

TEST_CASE( "a_voluntary_quit_leaves_the_table_without_a_replacement", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    g->player_at( 1 ).set_moves( 100 );
    // The real save-and-quit action, through its confirmation.
    harness.press( 1, 'S' );
    REQUIRE( harness.run_until( [&harness]() {
        return mp_grid_contains( harness.screen( 1 ), "Save and quit" );
    } ) );
    harness.press( 1, 'Y' );
    REQUIRE( harness.run_until( []() {
        return mp_player_is_disconnected( 1 );
    }, 20 ) );

    // Leaving voluntarily is not dying: the session goes on, this seat leaves
    // like a lost wire, and no death mark, corpse or replacement menu is left
    // behind. The latch it owned is released.
    CHECK_FALSE( mp_session_end_requested() );
    CHECK_FALSE( harness.client_connected( 1 ) );
    CHECK_FALSE( mp_player_needs_replacement( 1 ) );
    CHECK_FALSE( mp_player_corpse_placed( 1 ) );
    CHECK_FALSE( mp_player_owns_quit_latch( 1 ) );
    CHECK( g->uquit == QUIT_NO );

    // The partner still plays: a pass ends their round, and with the other seat
    // gone nothing holds the tick, so it closes and the calendar moves.
    g->player_at( 0 ).set_moves( 100 );
    const int turn_before = to_turn<int>( calendar::turn );
    queue_plan_pass( 0 );
    CHECK( harness.run_until( [turn_before]() {
        return to_turn<int>( calendar::turn ) > turn_before;
    } ) );
}

TEST_CASE( "a_question_the_world_asks_is_answered_without_waiting", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    harness.run_until( []() {
        return false;
    }, 1 );
    // The test thread is the world and holds the baton between turns. Enter
    // the world phase as do_turn() does for world_turn_post(), where an NPC
    // walking up to talk, a monster's question or the death screen asks from.
    // There is no player of the world to answer: every player's input belongs
    // to their own thread, and waiting on one would hold the baton while
    // asking them for it.
    mp_activate_world();
    CHECK_FALSE( query_yn( "A question from the world?" ) );
    mp_activate_player( 0 );
    // The world still runs turns afterwards.
    const int turn_before = to_turn<int>( calendar::turn );
    CHECK( harness.run_until( [turn_before]() {
        return to_turn<int>( calendar::turn ) > turn_before;
    } ) );
}
TEST_CASE( "the_main_screen_outlives_the_end_of_a_session", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    g->add_player_slot().set_body();
    mp_sync_context_count();
    // main() raises context 0's main screen before the session starts and
    // drops it at the end of the same loop iteration, after the session ends.
    shared_ptr_fast<ui_adaptor> ui = g->create_or_get_main_ui_adaptor();

    mp_server_end_session();
    // ~ui_adaptor asserts it dies under the stack it registered in.
    ui.reset();

    // And the menu loop main() goes back to finds the process as it was
    // before the session: player 0's context and stack.
    CHECK( ui_manager::get_active_stack() == &g->contexts[0].ui_stack );
    CHECK( mp_active_context_ptr() == &g->contexts[0] );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}
// A seat inside a menu is parked on its own wire, not on the baton, and the
// world is waiting on somebody else: a key that arrives for the menu seat still
// has to be granted and read, or a menu becomes unclosable while the partner
// takes their time.
TEST_CASE( "a_player_in_a_menu_keeps_receiving_its_keys", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    g->player_at( 0 ).set_moves( 100 );
    g->player_at( 1 ).set_moves( 100 );
    harness.press( 1, mp_menu_inventory.key );
    REQUIRE( harness.run_until( [&]() {
        return mp_grid_contains( harness.screen( 1 ), mp_menu_inventory.marker );
    } ) );

    // Seat 1 is reading its menu; seat 0 is present with AP and no plan, so the
    // round is held open on seat 0 and the world waits on it.
    harness.run_until( []() {
        return false;
    }, 3 );
    CHECK( mp_turn_is_open() );
    CHECK_FALSE( mp_turn_round_complete() );

    // The menu seat's key is read even though the world is waiting on somebody
    // else.
    harness.press( 1, KEY_ESCAPE );
    CHECK( harness.run_until( [&]() {
        return !mp_grid_contains( harness.screen( 1 ), mp_menu_inventory.marker );
    }, 20 ) );
}
TEST_CASE( "every_client_is_told_why_the_session_ended", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_sessions_reset_for_test();
    mp_turn_reset_all();
    const mp_loopback_pair wire = mp_make_loopback_pair();
    mp_sessions().push_back( std::make_unique<mp_session>() );
    mp_sessions().back()->transport = wire.a;

    mp_request_session_end( "Bo disconnected" );
    mp_server_end_session();

    // The client's end of the wire: everything the server said last.
    std::optional<std::string> reason;
    while( std::optional<std::vector<uint8_t>> payload = wire.b->recv( 100 ) ) {
        mp_msg_kind kind = mp_msg_kind::screen;
        std::vector<uint8_t> body;
        mp_control msg;
        if( mp_unwrap( *payload, kind, body ) && kind == mp_msg_kind::control &&
            mp_control_from_json( std::string( body.begin(), body.end() ), msg ) &&
            msg.kind == mp_control_kind::bye ) {
            reason = msg.bye_reason;
        }
    }
    CHECK( reason == std::optional<std::string>( "Bo disconnected" ) );
    mp_turn_reset_all();
}

TEST_CASE( "a_death_opens_a_replacement_instead_of_ending_the_session", "[mp]" )
{
    restore_active_state restore;
    // Both seats: a death in slot 1 used to go unnoticed, and slot 0's used to
    // reach the single-player death screens from the world thread.
    const size_t victim = GENERATE( 1, 0 );
    CAPTURE( victim );
    mp_session_harness harness( 2 );
    const tripoint_bub_ms corpse = g->player_at( victim ).pos_bub();

    g->player_at( victim ).set_part_hp_cur( bodypart_id( "torso" ), 0 );
    harness.run_until( []() {
        return false;
    }, 3 );

    // Death no longer ends anybody's session: the corpse stays, the seat is
    // owed a replacement, and everybody else keeps playing.
    CHECK_FALSE( mp_session_end_requested() );
    // Refetched by slot rather than held as a reference: activation swaps
    // avatar contents between the slot objects while a thread runs.
    CHECK( g->player_at( victim ).is_dead_state() );
    CHECK_FALSE( get_map().i_at( corpse ).empty() );
    REQUIRE( mp_player_needs_replacement( victim ) );

    harness.choose_random_character( victim );
    REQUIRE( harness.run_until( [victim]() {
        // The creator makes the avatar alive before it is placed; the death
        // mark is cleared only once placement has actually happened.
        return !mp_player_needs_replacement( victim ) &&
               !g->player_at( victim ).is_dead_state();
    }, 60 ) );
    CHECK( g->player_at( victim ).pos_bub() != corpse );
    // A placed replacement is a fresh character: the seat's corpse fact must
    // not make its next death skip the corpse.
    CHECK_FALSE( mp_player_corpse_placed( victim ) );
}

TEST_CASE( "an_unplaced_replacement_keeps_the_dead_character_s_tile", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    const size_t victim = 1;
    const tripoint_abs_ms dead_pos = g->player_at( victim ).pos_abs();
    const tripoint_abs_ms partner = g->player_at( 0 ).pos_abs();
    g->player_at( victim ).set_part_hp_cur( bodypart_id( "torso" ), 0 );
    REQUIRE( harness.run_until( [victim]() {
        return mp_player_needs_replacement( victim );
    }, 3 ) );

    // Open the creator but do not finish it. The reset that builds the blank
    // avatar must leave it on the character's last legal tile, not on the
    // invalid sentinel Creature's constructor uses.
    harness.press( victim, 'r' );
    REQUIRE( harness.run_until( [victim]() {
        return !g->player_at( victim ).is_dead_state() &&
               mp_player_holds_barrier( victim );
    }, 20 ) );
    CHECK( g->player_at( victim ).pos_abs() == dead_pos );

    // The client leaves while the creator is still open. The creator unwinds,
    // the menu's back-out reset runs, and the seat is marked disconnected:
    // alive, blank, and -- without the position kept across the reset -- at the
    // sentinel the next closed turn would average into the bubble anchor.
    harness.drop_client( victim );
    REQUIRE( harness.run_until( [victim]() {
        return mp_player_is_disconnected( victim ) &&
               !mp_player_holds_barrier( victim );
    }, 40 ) );
    CHECK_FALSE( g->player_at( victim ).is_dead_state() );
    CHECK( g->player_at( victim ).pos_abs() == dead_pos );

    harness.run_until( []() {
        return false;
    }, 3 );
    // Both living seats still count towards the anchor, so it is the midpoint
    // of their legal tiles. Averaging in the sentinel moves it by thousands of
    // tiles and off the map.
    const tripoint_abs_ms expected( ( partner.x() + dead_pos.x() ) / 2,
                                    ( partner.y() + dead_pos.y() ) / 2,
                                    partner.z() );
    CHECK( mp_bubble_anchor() == expected );
}

TEST_CASE( "a_second_replacement_anchors_on_a_seat_mid_creation", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 3 );
    const tripoint_abs_ms first_dead = g->player_at( 0 ).pos_abs();
    g->player_at( 0 ).set_part_hp_cur( bodypart_id( "torso" ), 0 );
    g->player_at( 2 ).set_part_hp_cur( bodypart_id( "torso" ), 0 );
    REQUIRE( harness.run_until( []() {
        return mp_player_needs_replacement( 0 ) && mp_player_needs_replacement( 2 );
    }, 5 ) );

    // P0 is in its creator, alive and unplaced. Its seat is the first one the
    // anchor loop sees, and it now stands on the tile its dead character held,
    // not on the sentinel where every candidate tile is rejected.
    harness.press( 0, 'r' );
    REQUIRE( harness.run_until( []() {
        return !g->player_at( 0 ).is_dead_state() &&
               mp_player_holds_barrier( 0 );
    }, 20 ) );
    REQUIRE( g->player_at( 0 ).pos_abs() == first_dead );

    harness.choose_random_character( 2 );
    REQUIRE( harness.run_until( []() {
        return !mp_player_needs_replacement( 2 ) &&
               !g->player_at( 2 ).is_dead_state();
    }, 60 ) );
    CHECK( rl_dist( g->player_at( 2 ).pos_abs(), first_dead ) <= MP_TETHER_SOFT_LIMIT );
}

TEST_CASE( "loading_a_preset_keeps_the_seat_on_its_own_tile", "[mp]" )
{
    restore_active_state restore;
    clear_avatar();
    mp_ensure_player_context();
    mp_turn_reset_all();

    avatar &ava = g->player_at( 0 );
    // A template written by a join-phase creator stores the invalid sentinel as
    // its location. Loading it into a seat that is choosing a replacement must
    // not move that seat off its last legal tile. The real flow hands the load
    // a freshly built avatar, so the test does too.
    ava = avatar();
    ava.set_pos_abs_only( tripoint_abs_ms( 20, 10, -500 ) );
    pool_type pool = pool_type::FREEFORM;
    ava.save_template( "mp-sentinel-location", pool );

    const tripoint_abs_ms legal_pos( 62, 61, 0 );
    ava = avatar();
    ava.set_pos_abs_only( legal_pos );
    REQUIRE( ava.load_template( "mp-sentinel-location", pool ) );
    CHECK( ava.pos_abs() == legal_pos );
}

TEST_CASE( "the_session_does_not_end_when_the_last_character_dies", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 1 );
    g->player_at( 0 ).set_part_hp_cur( bodypart_id( "torso" ), 0 );
    harness.run_until( []() {
        return false;
    }, 3 );
    // The session lives as long as the client at the terminal, not as long as
    // the character: an empty table with a client is still a session.
    CHECK_FALSE( mp_session_end_requested() );
}

TEST_CASE( "a_dead_disconnected_avatar_reconnects_into_creation", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    const std::string save_id = g->player_at( 1 ).get_save_id();
    harness.drop_client( 1 );
    g->player_at( 1 ).set_part_hp_cur( bodypart_id( "torso" ), 0 );
    harness.run_until( []() {
        return false;
    }, 3 );

    // A dead seat whose client left is a free seat, not a menu: the returning
    // client is offered creation again.
    CHECK( mp_server_seat_is_free_for_new_client( 1 ) );
    CHECK( harness.reconnect_client( 1, save_id ) );
    REQUIRE( mp_player_needs_replacement( 1 ) );
    harness.choose_random_character( 1 );
    CHECK( harness.run_until( []() {
        return !mp_player_needs_replacement( 1 ) &&
               !g->player_at( 1 ).is_dead_state();
    }, 60 ) );
}

TEST_CASE( "a_new_client_can_take_a_dead_disconnected_seat", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    harness.drop_client( 1 );
    g->player_at( 1 ).set_part_hp_cur( bodypart_id( "torso" ), 0 );
    harness.run_until( []() {
        return false;
    }, 3 );

    // A dead character's seat whose client left is free, and its session and
    // parked thread are still there: a newcomer joins them rather than being
    // refused or given a seat of its own.
    CHECK( mp_server_seat_is_free_for_new_client( 1 ) );
    CHECK( mp_server_free_seat_for_new_client() == std::optional<size_t>( 1 ) );
    harness.connect_replacement_client( 1 );
    harness.choose_random_character( 1 );
    REQUIRE( harness.run_until( []() {
        return !mp_player_needs_replacement( 1 ) &&
               !g->player_at( 1 ).is_dead_state();
    }, 60 ) );

    // The newcomer is a client like any other: on the wire, off the
    // disconnected list, alive, and holding its own character card.
    CHECK( harness.client_connected( 1 ) );
    CHECK( harness.seat_occupied( 1 ) );
    CHECK_FALSE( mp_player_is_disconnected( 1 ) );
    CHECK( g->player_at( 1 ).get_name() != "" );
    const mp_session *const session = mp_session_of_slot( 1 );
    REQUIRE( session != nullptr );
    CHECK( session->card_save_id == g->player_at( 1 ).get_save_id() );
    // Admission announced a new client, not a returning character.
    CHECK( harness.message_log_contains( "joined" ) );

    // And the seat does not hold the party back: with both terminals at the
    // keyboard, a pass each settles the round and the next tick closes and
    // moves the calendar.
    const int turn_before = to_turn<int>( calendar::turn );
    g->player_at( 0 ).set_moves( 100 );
    g->player_at( 1 ).set_moves( 100 );
    queue_plan_pass( 0 );
    queue_plan_pass( 1 );
    CHECK( harness.run_until( [turn_before]() {
        return to_turn<int>( calendar::turn ) > turn_before;
    } ) );
}

TEST_CASE( "a_new_client_is_refused_when_no_seat_is_free", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    const size_t seats = g->player_slot_count();
    std::string err;

    // Both seats have living clients at their terminals, so nobody new can sit
    // down. The refusal comes from the admission path, not from a broken wire.
    CHECK_FALSE( harness.try_admit_new_client( "Cid", err ) );
    CHECK( err.find( "no free seat" ) != std::string::npos );
    CHECK( harness.client_connected( 0 ) );
    CHECK( harness.client_connected( 1 ) );
    CHECK( g->player_slot_count() == seats );
}

TEST_CASE( "a_disconnect_while_creating_a_replacement_frees_the_seat", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    g->player_at( 1 ).set_part_hp_cur( bodypart_id( "torso" ), 0 );
    harness.run_until( []() {
        return false;
    }, 3 );
    harness.drop_client( 1 );
    harness.run_until( []() {
        return false;
    }, 3 );

    // Leaving the replacement menu is the same as leaving the join menu: the
    // session goes on, and the seat waits dead and disconnected for a new
    // client rather than spinning the world.
    CHECK_FALSE( mp_session_end_requested() );
    CHECK( mp_server_seat_is_free_for_new_client( 1 ) );
}

namespace
{
// How many corpses lie on one tile. "Abandon this character" places one, and
// the death bookkeeping must never add a second.
int corpses_at( const tripoint_bub_ms &p )
{
    int corpses = 0;
    for( const item &it : get_map().i_at( p ) ) {
        corpses += it.is_corpse() ? 1 : 0;
    }
    return corpses;
}
} // namespace

TEST_CASE( "abandoning_a_character_opens_a_replacement", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    avatar &quitter = g->player_at( 1 );
    const tripoint_bub_ms corpse = quitter.pos_bub();
    // ACTION_SUICIDE does exactly this and leaves hp alone.
    quitter.set_moves( 100 );
    quitter.place_corpse( &get_map() );
    g->uquit = QUIT_SUICIDE;

    // The seat has to act once for handle_action() to return into
    // is_game_over(); the planning gate refuses the raw wait key, but a refusal
    // still returns into that check, which is all the death screen needs. The
    // latch becoming this seat's is the moment the verdict was recorded.
    harness.press( 1, '.' );
    REQUIRE( harness.run_until( []() {
        return mp_player_owns_quit_latch( 1 );
    }, 10 ) );

    // The in-turn death still shows the single-player death screen. It is a
    // cataimgui window: one frame lays it out and the next one draws it, and a
    // seat is granted the baton once per queued key. Let the first frame happen
    // (a couple of idle polls), then buy the drawing frame with a free key
    // (Task 6 moves the flush to the world side and may drop this press).
    harness.run_until( []() {
        return false;
    }, 2 );
    harness.press( 1, 'z' );
    REQUIRE( harness.run_until( [&harness]() {
        return mp_grid_contains( harness.screen( 1 ), "The End" );
    }, 20 ) );

    // Confirm it so is_game_over() returns and the ended branch routes the
    // abandon.
    harness.press( 1, '\n' );

    // Abandoning is a death, not a disconnect: the one global quit is cleared,
    // the session goes on, and the replacement menu opens for this client.
    REQUIRE( harness.run_until( [&harness]() {
        return mp_grid_contains( harness.screen( 1 ), "Join the game" );
    }, 20 ) );
    CHECK_FALSE( mp_session_end_requested() );
    CHECK( g->uquit == QUIT_NO );
    CHECK( corpses_at( corpse ) == 1 );
    CHECK_FALSE( g->player_at( 1 ).is_dead_state() );

    harness.choose_random_character( 1 );
    CHECK( harness.run_until( [corpse]() {
        return !mp_player_needs_replacement( 1 ) &&
               g->player_at( 1 ).pos_bub() != corpse;
    }, 60 ) );
    CHECK_FALSE( g->player_at( 1 ).is_dead_state() );
}

TEST_CASE( "a_recorded_death_still_clears_a_later_quit_latch", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    const tripoint_bub_ms corpse = g->player_at( 1 ).pos_bub();
    g->player_at( 1 ).set_part_hp_cur( bodypart_id( "torso" ), 0 );
    // The orchestrator records this death and places the corpse while the seat
    // sits in its replacement menu.
    harness.run_until( []() {
        return false;
    }, 3 );

    // The seat's own player_act then reaches is_game_over() after the death was
    // recorded, exactly as when the wire dies under an open menu. The single
    // global quit it latches is not a per-death fact: the seat owns it, and
    // every notify call clears it; a repeat call must not place a second corpse.
    g->uquit = QUIT_DIED;
    mp_claim_quit_latch( 1 );
    mp_server_notify_death( 1 );
    CHECK( g->uquit == QUIT_NO );
    CHECK_FALSE( mp_player_owns_quit_latch( 1 ) );
    mp_server_notify_death( 1 );
    CHECK( corpses_at( corpse ) == 1 );
    CHECK( mp_player_corpse_placed( 1 ) );
    CHECK_FALSE( mp_session_end_requested() );
    CHECK( mp_player_needs_replacement( 1 ) );
}

TEST_CASE( "a_bare_quit_latch_is_not_evidence_for_a_seat", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    // No seat has claimed the latch. A partner's death scan must not read it as
    // its own: the latch is a process-wide value, not a per-seat fact.
    g->uquit = QUIT_SUICIDE;
    mp_server_notify_death( 0 );
    CHECK_FALSE( mp_player_needs_replacement( 0 ) );
    CHECK_FALSE( g->player_at( 0 ).is_dead_state() );
    CHECK( g->uquit == QUIT_SUICIDE );
}

TEST_CASE( "a_latch_owned_by_another_seat_is_not_consumed", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    g->uquit = QUIT_SUICIDE;
    mp_claim_quit_latch( 1 );

    // A notification for the partner is not evidence of the partner's death,
    // and it leaves the owner's latch alone.
    mp_server_notify_death( 0 );
    CHECK_FALSE( mp_player_needs_replacement( 0 ) );
    CHECK( mp_player_owns_quit_latch( 1 ) );
    CHECK( g->uquit == QUIT_SUICIDE );

    // Even a genuinely dead partner is recorded on its own evidence: its
    // corpse is placed while the owner's latch is untouched.
    g->player_at( 0 ).set_part_hp_cur( bodypart_id( "torso" ), 0 );
    mp_server_notify_death( 0 );
    CHECK( mp_player_needs_replacement( 0 ) );
    CHECK( mp_player_corpse_placed( 0 ) );
    CHECK( mp_player_owns_quit_latch( 1 ) );
    CHECK( g->uquit == QUIT_SUICIDE );

    // The owner's own notification consumes the latch and records the death
    // once.
    mp_server_notify_death( 1 );
    CHECK_FALSE( mp_player_owns_quit_latch( 1 ) );
    CHECK( g->uquit == QUIT_NO );
    CHECK( mp_player_needs_replacement( 1 ) );
}

TEST_CASE( "a_partner_does_not_read_a_foreign_quit_latch", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    // The state while seat 1's death screen holds the baton window open: the
    // one global latch is set and owned by seat 1.
    g->uquit = QUIT_SUICIDE;
    mp_claim_quit_latch( 1 );

    // Seat 0 takes a real turn. Its is_game_over() must refuse the foreign
    // latch instead of showing seat 0 a death screen and ending its turn.
    g->player_at( 0 ).set_moves( 100 );
    harness.press( 0, '.' );
    harness.run_until( []() {
        return false;
    }, 3 );
    CHECK_FALSE( g->player_at( 0 ).is_dead_state() );
    CHECK_FALSE( mp_player_needs_replacement( 0 ) );
    CHECK( mp_player_owns_quit_latch( 1 ) );
    CHECK( g->uquit == QUIT_SUICIDE );

    // The partner is still a live, connected player: the foreign latch did not
    // end its turn or hand it to a replacement.
    CHECK( mp_server_any_client_connected() );
    CHECK_FALSE( mp_session_end_requested() );

    // Test cleanup: release the latch the harness did not clear itself.
    mp_clear_quit_latch( 1 );
    g->uquit = QUIT_NO;
}

TEST_CASE( "a_death_recorded_in_a_menu_is_placed_once", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    g->player_at( 1 ).set_moves( 100 );
    harness.press( 1, 'i' );
    // The seat is parked inside its own player_act(), in the inventory menu.
    REQUIRE( harness.run_until( [&harness]() {
        return mp_grid_contains( harness.screen( 1 ), "Item hotkeys assigned" );
    }, 20 ) );
    const tripoint_bub_ms corpse = g->player_at( 1 ).pos_bub();

    // The character dies while the seat cannot act. The orchestrator's death
    // scan records the death and places the corpse while the menu is open.
    g->player_at( 1 ).set_part_hp_cur( bodypart_id( "torso" ), 0 );
    harness.run_until( []() {
        return false;
    }, 3 );
    REQUIRE( mp_player_corpse_placed( 1 ) );
    REQUIRE( corpses_at( corpse ) == 1 );

    // Closing the menu lets player_act reach is_game_over()'s death path. It
    // must see the per-seat fact and not place a second corpse.
    harness.press( 1, KEY_ESCAPE );
    REQUIRE( harness.run_until( [&harness]() {
        return mp_grid_contains( harness.screen( 1 ), "The End" );
    }, 10 ) );
    harness.press( 1, '\n' );
    REQUIRE( harness.run_until( [&harness]() {
        return mp_grid_contains( harness.screen( 1 ), "Join the game" );
    }, 20 ) );
    CHECK( corpses_at( corpse ) == 1 );
}

TEST_CASE( "a_replacement_character_gets_its_own_card", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    g->player_at( 1 ).set_part_hp_cur( bodypart_id( "torso" ), 0 );
    harness.run_until( []() {
        return false;
    }, 3 );
    REQUIRE( mp_player_needs_replacement( 1 ) );
    harness.choose_random_character( 1 );
    REQUIRE( harness.run_until( []() {
        return !mp_player_needs_replacement( 1 ) &&
               !g->player_at( 1 ).is_dead_state();
    }, 60 ) );

    const std::string name = g->player_at( 1 ).get_name();
    const std::string save_id = g->player_at( 1 ).get_save_id();
    REQUIRE( mp_session_of_slot( 1 ) != nullptr );
    // The seat's claim is the character now on it, not the one that died.
    CHECK( mp_session_of_slot( 1 )->card_save_id == save_id );

    harness.drop_client( 1 );
    REQUIRE( harness.run_until( []() {
        return mp_player_is_disconnected( 1 );
    }, 10 ) );
    CHECK( harness.reconnect_client( 1, save_id ) );
    CHECK( g->player_at( 1 ).get_name() == name );
    CHECK( g->player_at( 1 ).get_save_id() == save_id );
}

// Away is "done for this tick", not "hold the world": nobody is at a keyboard,
// so nothing is waiting for a plan. What must not happen is the other extreme --
// the world free-running at CPU speed around absent players. A tick takes a
// tick's time: one poll interval each, and no baton traffic at all.
TEST_CASE( "an_all_away_party_advances_at_a_ticks_pace", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    g->player_at( 0 ).set_moves( 100 );
    g->player_at( 1 ).set_moves( 100 );
    mp_set_away( 0, true );
    mp_set_away( 1, true );
    mp_sched *const sched = mp_active_sched();
    REQUIRE( sched != nullptr );
    const int turn_before = to_turn<int>( calendar::turn );
    const size_t handoffs_before = sched->holder_generation();
    const auto started = std::chrono::steady_clock::now();

    // Ten polls take about half a second. Every one of them opens a tick,
    // resolves an empty round and closes it, so the calendar moves once per
    // poll -- and nobody is granted the baton, because no seat has a key, a
    // replacement or a quit to deliver.
    CHECK_FALSE( harness.run_until( []() {
        return false;
    }, 10 ) );
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started );
    const int advanced = to_turn<int>( calendar::turn ) - turn_before;
    CHECK( advanced > 0 );
    CHECK( advanced <= 12 );
    CHECK( sched->holder_generation() == handoffs_before );
    // One poll interval per tick is the floor. Two intervals of slack cover
    // scheduler jitter; a free run would be milliseconds, not hundreds.
    CHECK( elapsed >= std::chrono::milliseconds( MP_POLL_MS * 8 ) );
}

TEST_CASE( "an_away_player_does_not_slow_a_busy_partner", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    g->player_at( 1 ).assign_activity( wait_activity_actor( 10_minutes ) );
    mp_set_away( 0, true );
    const int turn_before = to_turn<int>( calendar::turn );

    // The away player is not a left-behind one: they cost the partner nothing
    // and, alone, nothing paces the world either.
    CHECK( harness.run_until( [turn_before]() {
        return to_turn<int>( calendar::turn ) > turn_before + 20;
    }, 400 ) );
}

TEST_CASE( "two_busy_players_do_not_hold_the_world", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );

    // The phase's criterion, pinned: a long activity is a player telling the
    // game what to do, and the world is free to run while both do it.
    g->player_at( 0 ).assign_activity( wait_activity_actor( 10_minutes ) );
    g->player_at( 1 ).assign_activity( wait_activity_actor( 10_minutes ) );
    const int turn_before = to_turn<int>( calendar::turn );

    CHECK( harness.run_until( [turn_before]() {
        return to_turn<int>( calendar::turn ) > turn_before + 20;
    }, 400 ) );
}

// The tick and round loop end to end: plans travel the loop, a complete round
// resolves, the round state resets, and the tick closes once no seat can act --
// handing moves back, which is what gives the next tick a round again.
TEST_CASE( "a_tick_runs_a_round_to_resolution_and_closes", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );

    // A seat is handed the baton only for queued input, and the terrain window
    // a plan's scene cells are measured in is produced by the seat's own draw.
    // A zoom key is free and opens no menu, so this runs each seat once without
    // spending the AP the case is about. The pool is generous on purpose: a
    // step costs more than the 100 a turn hands out (the character's own run
    // cost), and what it leaves behind is the next round's business.
    g->player_at( 0 ).set_moves( 1000 );
    g->player_at( 1 ).set_moves( 1000 );
    harness.press( 0, 'z' );
    harness.press( 1, 'z' );
    REQUIRE( harness.run_until( []() {
        return g->contexts[0].w_terrain && g->contexts[1].w_terrain &&
               mp_turn_is_open() && mp_plan_phase() == mp_round_phase::planning;
    }, 10 ) );

    const int tick_turn = to_turn<int>( calendar::turn );
    const tripoint_bub_ms start_a = g->player_at( 0 ).pos_bub();
    const tripoint_bub_ms start_b = g->player_at( 1 ).pos_bub();

    // Both seats want the tile east of them: the round resolves, both take the
    // step, and the AP it leaves keeps the tick going -- banked AP is another
    // round, not the end of the turn.
    for( const size_t slot : { size_t( 0 ), size_t( 1 ) } ) {
        const tripoint_bub_ms target = g->player_at( slot ).pos_bub() + tripoint_rel_ms::east;
        const std::optional<point> cell = plan_cell_for( slot, target );
        REQUIRE( cell );
        queue_plan( slot, mp_plan_kind::move, cell->x, cell->y );
        queue_plan_ready( slot );
    }
    REQUIRE( harness.run_until( [&]() {
        return g->player_at( 0 ).pos_bub() == start_a + tripoint_rel_ms::east &&
               g->player_at( 1 ).pos_bub() == start_b + tripoint_rel_ms::east;
    }, 10 ) );
    CHECK( g->player_at( 0 ).get_moves() < 1000 );
    CHECK( g->player_at( 1 ).get_moves() < 1000 );
    CHECK( mp_turn_is_open() );
    CHECK( mp_plan_phase() == mp_round_phase::planning );
    CHECK_FALSE( mp_turn_round_complete() );
    // One tick so far, and it has not closed: the calendar moved once, at the
    // tick's open, and does not move again while the tick runs.
    CHECK( to_turn<int>( calendar::turn ) == tick_turn );

    // Passing is the other way a seat says it is done for the tick: it spends
    // what is left, the round settles with nothing to run, and the tick closes.
    queue_plan_pass( 0 );
    queue_plan_pass( 1 );
    REQUIRE( harness.run_until( []() {
        return !mp_turn_is_open();
    }, 5 ) );
    CHECK( mp_plan_phase() == mp_round_phase::done );
    CHECK( to_turn<int>( calendar::turn ) == tick_turn );

    // The close handed the moves back, so the next tick opens with a round in
    // it -- and takes exactly one calendar step.
    REQUIRE( harness.run_until( []() {
        return mp_turn_is_open() && mp_plan_phase() == mp_round_phase::planning;
    }, 5 ) );
    CHECK( to_turn<int>( calendar::turn ) == tick_turn + 1 );
    CHECK( g->player_at( 0 ).get_moves() > 0 );
    CHECK( g->player_at( 1 ).get_moves() > 0 );

    // A seat that commits and then cancels within the same round leaves the
    // round waiting for itself; the client's observable state is planning.
    const std::optional<point> cell = plan_cell_for( 0,
                                     g->player_at( 0 ).pos_bub() + tripoint_rel_ms::west );
    REQUIRE( cell );
    queue_plan( 0, mp_plan_kind::ranged, cell->x, cell->y, 2 );
    queue_plan_ready( 0 );
    // The commit has to be observed before the cancel, or a refused
    // mp_plan_ready would satisfy every assertion below the same way.
    REQUIRE( harness.run_until( []() {
        return mp_plan_is_ready( 0 );
    }, 5 ) );
    queue_plan_cancel( 0 );
    harness.run_until( []() {
        return false;
    }, 3 );
    CHECK( mp_plan_phase() == mp_round_phase::planning );
    CHECK_FALSE( mp_plan_is_ready( 0 ) );
    CHECK_FALSE( mp_turn_round_complete() );
    CHECK( mp_turn_is_open() );
    CHECK( to_turn<int>( calendar::turn ) == tick_turn + 1 );

    // A plan whose cells are not in the seat's scene is refused with the reason
    // the party snapshot carries, and nothing is left holding the round.
    queue_plan( 1, mp_plan_kind::move, 100000, 100000, 3 );
    REQUIRE( harness.run_until( []() {
        return !mp_plan_error( 1 ).empty();
    }, 3 ) );
    CHECK( mp_plan_error( 1 ) == "target is off the map" );

    // The pass message, again: both seats are done for the tick, so the tick
    // that has been open since the fresh start closes without another calendar
    // step of its own.
    queue_plan_pass( 0 );
    queue_plan_pass( 1 );
    REQUIRE( harness.run_until( []() {
        return !mp_turn_is_open();
    }, 5 ) );
    CHECK( mp_plan_phase() == mp_round_phase::done );
    CHECK( to_turn<int>( calendar::turn ) == tick_turn + 1 );

    // A pass may also arrive as a plan of kind pass on the wire. It is the same
    // act as the pass control: it spends what the seat has left, so the round
    // settles instead of leaving an eligible seat behind.
    REQUIRE( harness.run_until( []() {
        return mp_turn_is_open() && mp_plan_phase() == mp_round_phase::planning;
    }, 5 ) );
    g->player_at( 0 ).set_moves( 100 );
    g->player_at( 1 ).set_moves( 100 );
    queue_plan( 0, mp_plan_kind::pass, 0, 0, 4 );
    queue_plan( 1, mp_plan_kind::pass, 0, 0, 5 );
    REQUIRE( harness.run_until( []() {
        return !mp_turn_is_open();
    }, 5 ) );
    CHECK( g->player_at( 0 ).get_moves() > 0 );
    CHECK( g->player_at( 1 ).get_moves() > 0 );
    CHECK( mp_plan_phase() == mp_round_phase::done );
}

// Two seats over the real wire. The cases above hand the loop the message a
// seat's reader would deliver (see queue_control) because they are the loop's
// unit tests; these write the control frames a client writes, through
// mp_session_harness's senders, and read the party body the server broadcasts
// back. Nothing here pokes at mp_session state.
TEST_CASE( "both_seats_plan_and_the_round_resolves", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    prepare_two_seat_round( harness, 1000 );

    const tripoint_bub_ms start_a = g->player_at( 0 ).pos_bub();
    const tripoint_bub_ms start_b = g->player_at( 1 ).pos_bub();
    const std::optional<point> cell_a = plan_cell_for( 0, start_a + tripoint_rel_ms::east );
    const std::optional<point> cell_b = plan_cell_for( 1, start_b + tripoint_rel_ms::west );
    REQUIRE( cell_a );
    REQUIRE( cell_b );

    harness.send_plan( 0, 1, mp_plan_kind::move, cell_a->x, cell_a->y );
    harness.send_plan( 1, 2, mp_plan_kind::move, cell_b->x, cell_b->y );
    REQUIRE( harness.run_until( []() {
        return mp_plan_of( 0 ).kind == mp_plan_kind::move &&
               mp_plan_of( 1 ).kind == mp_plan_kind::move;
    }, 10 ) );
    // The cells each client sent were translated under that seat's own view:
    // the loop holds the bubble targets, not the scene cells.
    CHECK( mp_plan_of( 0 ).target == start_a + tripoint_rel_ms::east );
    CHECK( mp_plan_of( 1 ).target == start_b + tripoint_rel_ms::west );

    // Pin each pool to exactly the step's price, so the round spends all of it
    // and the tick has nothing left to run.
    const int cost_a = mp_plan_cost( 0, mp_plan_of( 0 ) );
    const int cost_b = mp_plan_cost( 1, mp_plan_of( 1 ) );
    REQUIRE( cost_a > 0 );
    REQUIRE( cost_b > 0 );
    g->player_at( 0 ).set_moves( cost_a );
    g->player_at( 1 ).set_moves( cost_b );

    harness.send_ready( 0, 3 );
    harness.send_ready( 1, 4 );
    REQUIRE( harness.run_until( []() {
        return mp_plan_phase() == mp_round_phase::done;
    }, 10 ) );

    CHECK( g->player_at( 0 ).pos_bub() == start_a + tripoint_rel_ms::east );
    CHECK( g->player_at( 1 ).pos_bub() == start_b + tripoint_rel_ms::west );
    // The tick closed only because neither pool had anything left. The close
    // then hands each seat a fresh pool of exactly its speed, so a charged step
    // shows up as "nothing carried over" rather than the price still in hand.
    CHECK( g->player_at( 0 ).get_moves() == g->player_at( 0 ).get_speed() );
    CHECK( g->player_at( 1 ).get_moves() == g->player_at( 1 ).get_speed() );
}

TEST_CASE( "a_cancel_returns_the_round_to_planning", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    prepare_two_seat_round( harness, 1000 );

    const tripoint_bub_ms start_a = g->player_at( 0 ).pos_bub();
    const tripoint_bub_ms start_b = g->player_at( 1 ).pos_bub();
    const std::optional<point> cell_a = plan_cell_for( 0, start_a + tripoint_rel_ms::east );
    const std::optional<point> cell_b = plan_cell_for( 1, start_b + tripoint_rel_ms::west );
    REQUIRE( cell_a );
    REQUIRE( cell_b );

    // Seat 1 plans but never commits, so seat 0's ready cannot settle the round.
    harness.send_plan( 0, 1, mp_plan_kind::move, cell_a->x, cell_a->y );
    harness.send_plan( 1, 2, mp_plan_kind::move, cell_b->x, cell_b->y );
    REQUIRE( harness.run_until( []() {
        return mp_plan_of( 0 ).kind == mp_plan_kind::move &&
               mp_plan_of( 1 ).kind == mp_plan_kind::move;
    }, 10 ) );
    harness.send_ready( 0, 3 );
    REQUIRE( harness.run_until( []() {
        return mp_plan_is_ready( 0 ) && mp_plan_phase() == mp_round_phase::planning;
    }, 10 ) );
    CHECK_FALSE( mp_plan_is_ready( 1 ) );

    harness.send_cancel( 0, 4 );
    REQUIRE( harness.run_until( []() {
        return !mp_plan_is_ready( 0 ) && mp_plan_of( 0 ).kind == mp_plan_kind::none;
    }, 10 ) );
    CHECK_FALSE( mp_plan_is_ready( 0 ) );
    CHECK_FALSE( mp_plan_is_ready( 1 ) );
    CHECK_FALSE( mp_turn_round_complete() );
    // A withdrawn intent leaves the round waiting for this seat again, which is
    // the phase the client draws.
    CHECK( mp_plan_phase() == mp_round_phase::planning );

    // And the seat can go through the whole cycle again in the same round.
    harness.send_plan( 0, 5, mp_plan_kind::move, cell_a->x, cell_a->y );
    REQUIRE( harness.run_until( []() {
        return mp_plan_of( 0 ).kind == mp_plan_kind::move;
    }, 10 ) );
    const int cost = mp_plan_cost( 0, mp_plan_of( 0 ) );
    REQUIRE( cost > 0 );
    g->player_at( 0 ).set_moves( cost );
    harness.send_ready( 0, 6 );
    REQUIRE( harness.run_until( []() {
        return mp_plan_is_ready( 0 );
    }, 10 ) );
    CHECK( mp_plan_phase() == mp_round_phase::planning );
}

TEST_CASE( "a_plan_that_costs_more_than_ap_is_refused", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    prepare_two_seat_round( harness, 1000 );

    const tripoint_bub_ms start = g->player_at( 0 ).pos_bub();
    const std::optional<point> cell = plan_cell_for( 0, start + tripoint_rel_ms::east );
    REQUIRE( cell );

    g->player_at( 0 ).set_moves( 65 );
    // Seat 1 has nothing; the refusal is seat 0's alone.
    g->player_at( 1 ).set_moves( 0 );

    // Submitting a plan costs nothing, so it is accepted; the commit is where
    // the AP is checked.
    harness.send_plan( 0, 1, mp_plan_kind::move, cell->x, cell->y );
    REQUIRE( harness.run_until( []() {
        return mp_plan_of( 0 ).kind == mp_plan_kind::move;
    }, 10 ) );
    REQUIRE( mp_plan_cost( 0, mp_plan_of( 0 ) ) == 100 );

    harness.send_ready( 0, 2 );
    REQUIRE( harness.run_until( []() {
        return !mp_plan_error( 0 ).empty();
    }, 10 ) );

    // The refusal is the resolver's own text, unchanged, and the seat kept
    // every point of AP it had.
    CHECK( mp_plan_error( 0 ) == "needs 100 AP, you have 65" );
    CHECK( g->player_at( 0 ).get_moves() == 65 );
    CHECK_FALSE( mp_plan_is_ready( 0 ) );
    CHECK( mp_turn_is_open() );
    CHECK( mp_plan_phase() == mp_round_phase::planning );

    // The client is told by the party snapshot, not left to guess.
    mp_party_msg party;
    REQUIRE( harness.wait_party( 0, 1000, party ) );
    REQUIRE( party.seats.size() == 2 );
    CHECK( party.seats[0].error == "needs 100 AP, you have 65" );
    CHECK( party.seats[0].plan == mp_plan_kind::move );
    CHECK( party.seats[0].pcost == 100 );
}

TEST_CASE( "a_seat_with_no_moves_does_not_hold_the_round", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    prepare_two_seat_round( harness, 1000 );

    const tripoint_bub_ms start_a = g->player_at( 0 ).pos_bub();
    const tripoint_bub_ms start_b = g->player_at( 1 ).pos_bub();
    const std::optional<point> cell = plan_cell_for( 0, start_a + tripoint_rel_ms::east );
    REQUIRE( cell );

    // Seat 1 is empty for this tick: the eligible set is seat 0 alone, so its
    // commit settles the round.
    g->player_at( 1 ).set_moves( 0 );

    harness.send_plan( 0, 1, mp_plan_kind::move, cell->x, cell->y );
    REQUIRE( harness.run_until( []() {
        return mp_plan_of( 0 ).kind == mp_plan_kind::move;
    }, 10 ) );
    const int cost = mp_plan_cost( 0, mp_plan_of( 0 ) );
    REQUIRE( cost > 0 );
    g->player_at( 0 ).set_moves( cost );

    harness.send_ready( 0, 2 );
    REQUIRE( harness.run_until( []() {
        return mp_plan_phase() == mp_round_phase::done;
    }, 10 ) );

    CHECK( g->player_at( 0 ).pos_bub() == start_a + tripoint_rel_ms::east );
    CHECK( g->player_at( 1 ).pos_bub() == start_b );
    CHECK( g->player_at( 0 ).get_moves() == g->player_at( 0 ).get_speed() );
    // The idle seat did not leave a planning phase of its own behind.
    CHECK( mp_plan_phase() == mp_round_phase::done );
}

TEST_CASE( "the_party_broadcast_shows_both_plans", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    prepare_two_seat_round( harness, 1000 );

    const tripoint_bub_ms start_a = g->player_at( 0 ).pos_bub();
    const tripoint_bub_ms start_b = g->player_at( 1 ).pos_bub();
    const std::optional<point> cell_a = plan_cell_for( 0, start_a + tripoint_rel_ms::east );
    const std::optional<point> cell_b = plan_cell_for( 1, start_b + tripoint_rel_ms::west );
    REQUIRE( cell_a );
    REQUIRE( cell_b );

    harness.send_plan( 0, 1, mp_plan_kind::move, cell_a->x, cell_a->y );
    harness.send_plan( 1, 2, mp_plan_kind::move, cell_b->x, cell_b->y );
    // Not ready: the snapshot is read while the round is still planning, which
    // is when a client draws the plans.
    REQUIRE( harness.run_until( []() {
        return mp_plan_of( 0 ).kind == mp_plan_kind::move &&
               mp_plan_of( 1 ).kind == mp_plan_kind::move;
    }, 10 ) );

    mp_party_msg to_seat0;
    mp_party_msg to_seat1;
    REQUIRE( harness.wait_party( 0, 1000, to_seat0 ) );
    REQUIRE( harness.wait_party( 1, 1000, to_seat1 ) );

    // The seats are one shared body: same names, plans, targets and costs.
    REQUIRE( to_seat0.seats.size() == 2 );
    REQUIRE( to_seat1.seats.size() == 2 );
    for( size_t i = 0; i < 2; ++i ) {
        const mp_party_seat &a = to_seat0.seats[i];
        const mp_party_seat &b = to_seat1.seats[i];
        CHECK( a.slot == b.slot );
        CHECK( a.name == b.name );
        CHECK( a.moves == b.moves );
        CHECK( a.ready == b.ready );
        CHECK( a.passed == b.passed );
        CHECK( a.connected == b.connected );
        CHECK( a.away == b.away );
        CHECK( a.plan == b.plan );
        CHECK( a.px == b.px );
        CHECK( a.py == b.py );
        CHECK( a.pcost == b.pcost );
        CHECK( a.menu == b.menu );
        CHECK( a.error == b.error );
    }
    // Targets are bubble tiles, not the cells either client sent: east for
    // seat 0, west for seat 1, and each body carries both.
    CHECK( to_seat0.seats[0].slot == 0 );
    CHECK( to_seat0.seats[0].name == "P0" );
    CHECK( to_seat0.seats[0].plan == mp_plan_kind::move );
    CHECK( to_seat0.seats[0].px == start_a.x() + 1 );
    CHECK( to_seat0.seats[0].py == start_a.y() );
    CHECK( to_seat0.seats[1].slot == 1 );
    CHECK( to_seat0.seats[1].name == "P1" );
    CHECK( to_seat0.seats[1].plan == mp_plan_kind::move );
    CHECK( to_seat0.seats[1].px == start_b.x() - 1 );
    CHECK( to_seat0.seats[1].py == start_b.y() );
    CHECK( to_seat0.phase == static_cast<uint8_t>( mp_round_phase::planning ) );
    CHECK( to_seat1.phase == to_seat0.phase );

    // Each body's header is the recipient's own: its slot and its own scene
    // origin, so a partner's bubble target can be placed through it. The seats
    // stand two tiles apart, so their views differ -- and the headers differ in
    // exactly the same way, while the seats do not.
    CHECK( to_seat0.you == 0 );
    CHECK( to_seat1.you == 1 );
    REQUIRE( to_seat0.view_known );
    REQUIRE( to_seat1.view_known );
    const std::optional<scene_anchor_point> anchor0 = party_anchor_for( 0 );
    const std::optional<scene_anchor_point> anchor1 = party_anchor_for( 1 );
    REQUIRE( anchor0 );
    REQUIRE( anchor1 );
    CHECK( to_seat0.vx == anchor0->x );
    CHECK( to_seat0.vy == anchor0->y );
    CHECK( to_seat1.vx == anchor1->x );
    CHECK( to_seat1.vy == anchor1->y );
    CHECK( *anchor0 != *anchor1 );
    CHECK( ( to_seat0.vx != to_seat1.vx || to_seat0.vy != to_seat1.vy ) );
}

TEST_CASE( "a_waiting_seat_gets_its_scene_after_resolution", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    // A scene frame exists only for a client that asked for the stream; the
    // harness's bare session is a text client until this says otherwise.
    harness.set_client_caps( 0, MP_CAP_SCENE );
    harness.set_client_caps( 1, MP_CAP_SCENE );
    prepare_two_seat_round( harness, 1000 );

    // Let every frame the initial draw queued land before taking the baseline:
    // a frame still on the wire would otherwise satisfy the assertion without a
    // resolution. No seat has input, so these polls only deliver and wait.
    harness.run_until( []() {
        return false;
    }, 3 );
    REQUIRE( harness.last_scene_frame( 1 ) > 0 );
    const uint32_t server_before = mp_session_of_slot( 1 )->scene_seq;

    const tripoint_bub_ms start_a = g->player_at( 0 ).pos_bub();
    const tripoint_bub_ms start_b = g->player_at( 1 ).pos_bub();
    const std::optional<point> cell = plan_cell_for( 0, start_a + tripoint_rel_ms::east );
    REQUIRE( cell );

    // Seat 1 never acts this round: no plan, no ready, no moves.
    g->player_at( 1 ).set_moves( 0 );

    harness.send_plan( 0, 1, mp_plan_kind::move, cell->x, cell->y );
    REQUIRE( harness.run_until( []() {
        return mp_plan_of( 0 ).kind == mp_plan_kind::move;
    }, 10 ) );
    const int cost = mp_plan_cost( 0, mp_plan_of( 0 ) );
    REQUIRE( cost > 0 );
    g->player_at( 0 ).set_moves( cost );

    harness.send_ready( 0, 2 );
    REQUIRE( harness.run_until( []() {
        return mp_plan_phase() == mp_round_phase::done;
    }, 10 ) );

    CHECK( g->player_at( 1 ).pos_bub() == start_b );
    // The all-seat flush built and sent a frame for the seat that was waiting,
    // even though that seat never held the baton this round.
    const uint32_t server_after = mp_session_of_slot( 1 )->scene_seq;
    CHECK( server_after > server_before );
    REQUIRE( harness.wait_scene_frame( 1, server_after, 1000 ) );
    CHECK( harness.last_scene_frame( 1 ) >= server_after );
}

TEST_CASE( "a_session_plan_onto_a_known_trap_runs_without_asking", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    prepare_two_seat_round( harness, 1000 );

    avatar &a = g->player_at( 0 );
    const tripoint_bub_ms start = a.pos_bub();
    const tripoint_bub_ms target = start + tripoint_rel_ms::east;
    map &here = get_map();
    here.trap_set( target, trap_id( "tr_light_snare" ) );
    a.add_known_trap( target, here.tr_at( target ) );
    // The dangerous check reads the active avatar, which a previous poll may
    // have left on the partner's seat; look through seat 0's eyes.
    mp_activate_player( 0 );
    const bool dangerous = !g->get_dangerous_tile( target, 10 ).empty();
    mp_activate_world();
    REQUIRE( dangerous );

    const std::optional<point> cell = plan_cell_for( 0, target );
    REQUIRE( cell );
    // Seat 1 has nothing to do, so seat 0's commit settles the round alone.
    g->player_at( 1 ).set_moves( 0 );

    harness.send_plan( 0, 1, mp_plan_kind::move, cell->x, cell->y );
    REQUIRE( harness.run_until( []() {
        return mp_plan_of( 0 ).kind == mp_plan_kind::move;
    }, 10 ) );
    harness.send_ready( 0, 2 );

    // The resolution asks its dangerous-tile question on the world thread. The
    // committed plan answers it, so the round finishes and the step lands; a
    // prompt that truly waited would hang this run.
    REQUIRE( harness.run_until( [&]() {
        return g->player_at( 0 ).pos_bub() == target;
    }, 10 ) );
    CHECK( mp_plan_error( 0 ).empty() );
}

TEST_CASE( "a_plan_onto_a_ledge_fizzles_and_the_reason_reaches_the_party", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    prepare_two_seat_round( harness, 1000 );

    const tripoint_bub_ms start = g->player_at( 0 ).pos_bub();
    const tripoint_bub_ms target = start + tripoint_rel_ms::east;
    // Open air is passable, so the move reaches the ledge menu.
    get_map().ter_set( target, ter_id( "t_open_air" ) );
    const int moves = g->player_at( 0 ).get_moves();

    const std::optional<point> cell = plan_cell_for( 0, target );
    REQUIRE( cell );
    g->player_at( 1 ).set_moves( 0 );
    harness.send_plan( 0, 1, mp_plan_kind::move, cell->x, cell->y );
    REQUIRE( harness.run_until( []() {
        return mp_plan_of( 0 ).kind == mp_plan_kind::move;
    }, 10 ) );
    harness.send_ready( 0, 2 );

    // The menu cannot open on the world thread. The plan fizzles instead of
    // half-executing, and the reason survives the round reset into the party
    // body the client sees.
    REQUIRE( harness.run_until( []() {
        return !mp_plan_error( 0 ).empty();
    }, 10 ) );
    CHECK( mp_plan_error( 0 ) == "needs an interactive choice" );
    CHECK( g->player_at( 0 ).pos_bub() == start );
    CHECK( g->player_at( 0 ).get_moves() == moves );

    mp_party_msg party;
    REQUIRE( harness.wait_party( 0, 1000, party ) );
    REQUIRE( party.seats.size() == 2 );
    CHECK( party.seats[0].error == "needs an interactive choice" );
}

TEST_CASE( "a_resolver_refusal_reaches_the_party_body", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    prepare_two_seat_round( harness, 1000 );

    const tripoint_bub_ms start = g->player_at( 0 ).pos_bub();
    const std::optional<point> cell = plan_cell_for( 0, start + tripoint_rel_ms::east );
    REQUIRE( cell );
    g->player_at( 1 ).set_moves( 0 );

    // `ranged` is a kind phase 1 cannot run. The submit and the commit both
    // accept it (it costs nothing up front); only the resolver refuses it.
    harness.send_plan( 0, 1, mp_plan_kind::ranged, cell->x, cell->y );
    REQUIRE( harness.run_until( []() {
        return mp_plan_of( 0 ).kind == mp_plan_kind::ranged;
    }, 10 ) );
    harness.send_ready( 0, 2 );
    REQUIRE( harness.run_until( []() {
        return !mp_plan_error( 0 ).empty();
    }, 10 ) );

    // The tick loop resets the round before it broadcasts. The resolver's
    // refusal has to survive that reset, or the client never learns why the
    // committed plan did nothing.
    CHECK( mp_plan_error( 0 ) == "not in this phase" );
    CHECK( g->player_at( 0 ).pos_bub() == start );

    mp_party_msg party;
    REQUIRE( harness.wait_party( 0, 1000, party ) );
    REQUIRE( party.seats.size() == 2 );
    CHECK( party.seats[0].error == "not in this phase" );
    CHECK( party.seats[0].plan == mp_plan_kind::none );

    // The seat's next intent clears the text, as the protocol promises.
    harness.send_plan( 0, 3, mp_plan_kind::move, cell->x, cell->y );
    REQUIRE( harness.run_until( []() {
        return mp_plan_of( 0 ).kind == mp_plan_kind::move;
    }, 10 ) );
    CHECK( mp_plan_error( 0 ).empty() );
}

TEST_CASE( "a_plan_cell_is_a_rect_cell_when_the_sidebar_is_on_the_left", "[mp]" )
{
    restore_active_state restore;
    // The terrain window starts right of the sidebar, so a rect cell and the
    // terminal cell at the same index name different map tiles.
    override_option sidebar( "SIDEBAR_POSITION", "left" );
    mp_session_harness harness( 2 );
    prepare_two_seat_round( harness, 1000 );

    mp_activate_player( 0 );
    mp_scene_rect rect;
    const bool have_rect = mp_scene_mask_map_rect( rect );
    mp_activate_world();
    REQUIRE( have_rect );
    REQUIRE( rect.x > 0 );

    const tripoint_bub_ms start = g->player_at( 0 ).pos_bub();
    const tripoint_bub_ms target = start + tripoint_rel_ms::east;
    const std::optional<point> cell = plan_cell_for( 0, target );
    REQUIRE( cell );
    // A rect cell is an offset inside the frame, never a terminal column.
    CHECK( cell->x >= 0 );
    CHECK( cell->x < rect.w );
    CHECK( cell->y >= 0 );
    CHECK( cell->y < rect.h );

    g->player_at( 1 ).set_moves( 0 );
    harness.send_plan( 0, 1, mp_plan_kind::move, cell->x, cell->y );
    REQUIRE( harness.run_until( []() {
        return mp_plan_of( 0 ).kind == mp_plan_kind::move;
    }, 10 ) );
    // The cell reached the tile the client meant, not a tile shifted by the
    // sidebar's terminal offset.
    CHECK( mp_plan_of( 0 ).target == target );

    const int cost = mp_plan_cost( 0, mp_plan_of( 0 ) );
    REQUIRE( cost > 0 );
    g->player_at( 0 ).set_moves( cost );
    harness.send_ready( 0, 2 );
    REQUIRE( harness.run_until( [&]() {
        return g->player_at( 0 ).pos_bub() == target;
    }, 10 ) );
}

TEST_CASE( "a_seat_s_menu_state_reaches_the_party_body", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    g->player_at( 0 ).set_moves( 100 );
    g->player_at( 1 ).set_moves( 100 );

    harness.press( 1, mp_menu_inventory.key );
    REQUIRE( harness.run_until( [&]() {
        return mp_grid_contains( harness.screen( 1 ), mp_menu_inventory.marker );
    }, 20 ) );
    // The seat reports the depth change when it parks in the menu; one more
    // poll turns that into the party body, with no other seat's control needed.
    harness.run_until( []() {
        return false;
    }, 2 );
    mp_party_msg opened;
    REQUIRE( harness.wait_party( 1, 1000, opened ) );
    REQUIRE( opened.seats.size() == 2 );
    CHECK_FALSE( opened.seats[1].menu.empty() );
    CHECK( opened.seats[0].menu.empty() );

    harness.press( 1, KEY_ESCAPE );
    REQUIRE( harness.run_until( [&]() {
        return !mp_grid_contains( harness.screen( 1 ), mp_menu_inventory.marker );
    }, 20 ) );
    harness.run_until( []() {
        return false;
    }, 2 );
    mp_party_msg closed;
    REQUIRE( harness.wait_party( 1, 1000, closed ) );
    CHECK( closed.seats[1].menu.empty() );
}

TEST_CASE( "a_disconnected_seat_s_committed_plan_does_not_run", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    prepare_two_seat_round( harness, 1000 );

    const tripoint_bub_ms b_start = g->player_at( 1 ).pos_bub();
    const std::optional<point> cell = plan_cell_for( 1, b_start + tripoint_rel_ms::east );
    REQUIRE( cell );

    harness.send_plan( 1, 1, mp_plan_kind::move, cell->x, cell->y );
    REQUIRE( harness.run_until( []() {
        return mp_plan_of( 1 ).kind == mp_plan_kind::move;
    }, 10 ) );
    harness.send_ready( 1, 2 );
    REQUIRE( harness.run_until( []() {
        return mp_plan_is_ready( 1 );
    }, 10 ) );
    // Seat 0 has not committed, so nothing can resolve while the wire is alive.
    CHECK( mp_turn_is_open() );

    harness.drop_client( 1 );
    REQUIRE( harness.run_until( []() {
        return mp_player_is_disconnected( 1 );
    }, 20 ) );
    // Disconnect cancels the committed plan: the round no longer waits on it,
    // and it cannot act for a seat nobody is watching.
    CHECK_FALSE( mp_plan_is_ready( 1 ) );
    CHECK( mp_plan_of( 1 ).kind == mp_plan_kind::none );

    g->player_at( 0 ).set_moves( 100 );
    queue_plan_pass( 0 );
    REQUIRE( harness.run_until( []() {
        return mp_plan_phase() == mp_round_phase::done;
    }, 20 ) );
    CHECK( g->player_at( 1 ).pos_bub() == b_start );
    // The seat's AP may have been refilled by the tick close; what matters is
    // that no action was taken for it.
    CHECK_FALSE( mp_player_is_disconnected( 0 ) );
}

TEST_CASE( "a_session_boundary_forgets_the_round_state", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_turn_reset_all();
    mp_plan_reset_all();
    mp_sessions_reset_for_test();

    avatar &u = g->player_at( 0 );
    u.set_moves( 1000 );
    mp_plan p;
    p.kind = mp_plan_kind::move;
    p.target = u.pos_bub() + tripoint_rel_ms::east;
    std::string err;
    REQUIRE( mp_plan_submit( 0, p, err ) );
    REQUIRE( mp_plan_ready( 0, err ) );
    mp_plan_set_error( 0, "carried over" );

    mp_server_end_session();

    // A session that ends with a stored plan, a readiness or a refusal leaves
    // none of it for the next session to broadcast or execute.
    CHECK( mp_plan_of( 0 ).kind == mp_plan_kind::none );
    CHECK_FALSE( mp_plan_is_ready( 0 ) );
    CHECK_FALSE( mp_plan_is_passed( 0 ) );
    CHECK( mp_plan_error( 0 ).empty() );
    CHECK( mp_plan_phase() == mp_round_phase::planning );
    mp_turn_reset_all();
}

TEST_CASE( "a_monster_takes_its_step_after_the_player_batch", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    prepare_two_seat_round( harness, 1000 );
    set_time_to_day();

    const tripoint_bub_ms start_a = g->player_at( 0 ).pos_bub();
    const tripoint_bub_ms start_b = g->player_at( 1 ).pos_bub();
    const tripoint_abs_ms avatar_abs = g->player_at( 0 ).pos_abs();
    // Three tiles east: in sight, but far enough that the creature's step is a
    // walk rather than a melee attack.
    const tripoint_bub_ms monster_start = start_a + tripoint_rel_ms( 3, 0, 0 );
    monster &zombie = spawn_test_monster( "mon_zombie", monster_start );
    zombie.set_dest( avatar_abs );
    // Exactly one step's AP on pavement: the creature takes its one step in the
    // round that follows the players' batch, and has nothing left for another.
    zombie.set_moves( 100 );

    harness.send_pass( 0, 1 );
    harness.send_pass( 1, 2 );
    REQUIRE( harness.run_until( [&zombie, monster_start]() {
        return zombie.pos_bub() != monster_start;
    }, 10 ) );

    // The players' batch went first and the creature followed it inside the
    // same tick: one round's step, not a tick's worth of walking.
    CHECK( rl_dist( monster_start, zombie.pos_bub() ) <= 1 );
    CHECK( zombie.get_moves() <= 0 );
    CHECK( g->player_at( 0 ).pos_bub() == start_a );
    CHECK( g->player_at( 1 ).pos_bub() == start_b );
    // With the creature's AP gone nothing can act, so the tick closed in the
    // same poll and the client sees the resolver's `done`. A loop that closed
    // before the creature's round would have closed without it ever moving.
    CHECK_FALSE( mp_turn_is_open() );
    CHECK( mp_plan_phase() == mp_round_phase::done );
}

// Temporary Phase 1 rule: outside a menu, a time-costing key is refused while
// the round waits for this seat's plan. The client sends plans instead; a text
// client's raw movement key must not walk the map outside a resolution.
TEST_CASE( "a_raw_costly_key_is_refused_while_a_plan_is_expected", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    g->player_at( 0 ).set_moves( 100 );
    Messages::clear_messages();
    const int turn_before = to_turn<int>( calendar::turn );
    const tripoint_bub_ms before = g->player_at( 0 ).pos_bub();

    // 'h' is the default "move west" key.
    harness.press( 0, 'h' );
    REQUIRE( harness.run_until( [&harness]() {
        return harness.message_log_contains( "use the plan keys: move, attack, 5" );
    }, 5 ) );

    // Refused, not performed: no step, no AP, and the world did not move.
    CHECK( g->player_at( 0 ).pos_bub() == before );
    CHECK( g->player_at( 0 ).get_moves() == 100 );
    CHECK( to_turn<int>( calendar::turn ) == turn_before );
}

TEST_CASE( "an_away_players_activity_advances_while_a_partner_plays", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );

    // The partner's own long activity keeps the world moving without waiting
    // on a key from either player. The away player's activity must spend the
    // moves each new turn hands them, not sit frozen on their input wait.
    avatar &away = g->player_at( 0 );
    away.assign_activity( wait_activity_actor( 30_minutes ) );
    mp_set_away( 0, true );
    g->player_at( 1 ).assign_activity( wait_activity_actor( 30_minutes ) );

    // Let the opening turns settle. The away player's own action loop spends
    // the moves they had when they left, so progress before this point does
    // not prove that the running world kept their activity going.
    harness.run_until( []() { return false; }, 3 );
    const int moves_before = away.activity.moves_left;
    const int turn_before = to_turn<int>( calendar::turn );

    CHECK( harness.run_until( [&]() {
        return to_turn<int>( calendar::turn ) > turn_before + 5 &&
               away.activity.moves_left < moves_before - 100;
    }, 100 ) );
}

TEST_CASE( "an_away_player_carrying_an_activity_can_still_come_back", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );

    // A long activity spends the player's moves before game::player_act()
    // reaches its input wait, so neither player is granted for anything but a
    // key. Mark both away through the same state the ':' key sets; then give
    // their activities a turn to drain. Away is done for the tick, so the world
    // runs -- the subject here is that the way back still works.
    for( size_t i = 0; i < 2; ++i ) {
        g->player_at( i ).assign_activity( wait_activity_actor( 10_minutes ) );
        mp_set_away( i, true );
    }
    harness.run_until( []() { return false; }, 3 );
    REQUIRE( mp_player_is_away( 0 ) );

    // ':' is the documented way back. The key is read on this player's own
    // input loop -- the grant reaches a seat whose activity has not got it to
    // an input wait of its own.
    harness.press( 0, ':' );
    CHECK( harness.run_until( []() {
        return !mp_player_is_away( 0 );
    }, 20 ) );
    CHECK( mp_player_is_away( 1 ) );
}

TEST_CASE( "an_away_player_at_an_input_wait_still_gets_the_baton", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    // Both seats have AP, so both can act and the round waits for a plan: that
    // is the "world is waiting on somebody else" state the away key has to
    // survive. Seat 0 then leaves the keyboard.
    g->player_at( 0 ).set_moves( 100 );
    g->player_at( 1 ).set_moves( 100 );
    mp_set_away( 0, true );
    const int turn_before = to_turn<int>( calendar::turn );

    // Going away is not abandoning the terminal. The away player's key has to
    // reach the game while the round waits for the partner -- otherwise it is
    // read off the wire and never dispatched, and the only way back from
    // "away" is a key that never arrives.
    harness.press( 0, mp_menu_inventory.key );
    REQUIRE( harness.run_until( [&]() {
        return mp_grid_contains( harness.screen( 0 ), mp_menu_inventory.marker );
    } ) );

    harness.press( 0, KEY_ESCAPE );
    REQUIRE( harness.run_until( [&]() {
        return !mp_grid_contains( harness.screen( 0 ), mp_menu_inventory.marker );
    } ) );

    // Reading a menu while away is still not playing: the free key does not
    // bring the seat back, and the round is still open on the partner.
    CHECK( mp_player_is_away( 0 ) );
    CHECK( mp_turn_is_open() );
    CHECK( to_turn<int>( calendar::turn ) == turn_before );
}

TEST_CASE( "the_away_key_toggles_and_a_costly_action_brings_a_player_back", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    g->player_at( 0 ).set_moves( 100 );

    harness.press( 0, ':' );
    REQUIRE( harness.run_until( []() {
        return mp_player_is_away( 0 );
    } ) );

    // The key is a toggle: a second press brings the player back, and a third
    // sends them away again. Both must work before any costly action is taken.
    harness.press( 0, ':' );
    REQUIRE( harness.run_until( []() {
        return !mp_player_is_away( 0 );
    } ) );
    harness.press( 0, ':' );
    REQUIRE( harness.run_until( []() {
        return mp_player_is_away( 0 );
    } ) );
    // Reading is not coming back: a free action must not put a player who is
    // away back into the barrier, or the partner freezes while they read.
    harness.press( 0, mp_menu_inventory.key );
    REQUIRE( harness.run_until( [&]() {
        return mp_grid_contains( harness.screen( 0 ), mp_menu_inventory.marker );
    } ) );
    CHECK( mp_player_is_away( 0 ) );
    harness.press( 0, KEY_ESCAPE );
    REQUIRE( harness.run_until( [&]() {
        return !mp_grid_contains( harness.screen( 0 ), mp_menu_inventory.marker );
    } ) );
    CHECK( mp_player_is_away( 0 ) );

    // A turn action does bring them back.
    harness.press( 0, '.' );
    CHECK( harness.run_until( []() {
        return !mp_player_is_away( 0 );
    } ) );
}

TEST_CASE( "the_away_key_does_nothing_without_a_party", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 1 );
    g->player_at( 0 ).set_moves( 100 );
    harness.press( 0, ':' );
    harness.run_until( []() {
        return false;
    }, 3 );
    CHECK_FALSE( mp_player_is_away( 0 ) );
}

TEST_CASE( "a_death_while_away_still_opens_a_replacement", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    harness.run_until( []() {
        return false;
    }, 1 );
    mp_set_away( 1, true );
    g->player_at( 1 ).set_part_hp_cur( bodypart_id( "torso" ), 0 );
    REQUIRE( g->player_at( 1 ).is_dead_state() );

    // Being away is not being gone: the client is still at its terminal, so the
    // death is a death like any other and the seat is offered a replacement.
    harness.run_until( []() {
        return false;
    }, 3 );
    CHECK_FALSE( mp_session_end_requested() );
    CHECK( g->player_at( 1 ).is_dead_state() );
    REQUIRE( mp_player_needs_replacement( 1 ) );

    harness.choose_random_character( 1 );
    CHECK( harness.run_until( []() {
        return !mp_player_needs_replacement( 1 ) &&
               !g->player_at( 1 ).is_dead_state();
    }, 60 ) );
}

TEST_CASE( "the_session_does_not_end_while_a_client_is_connected", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    CHECK( mp_server_connected_client_count() == 2 );
    CHECK( mp_server_any_client_connected() );
    CHECK_FALSE( mp_session_end_requested() );
}

TEST_CASE( "the_session_ends_when_the_last_client_leaves", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 1 );
    harness.drop_client( 0 );
    CHECK_FALSE( mp_server_any_client_connected() );
    mp_server_end_if_no_clients();
    CHECK( mp_session_end_requested() );
}

TEST_CASE( "a_dead_character_does_not_keep_the_session_alive", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 1 );
    g->player_at( 0 ).set_part_hp_cur( bodypart_id( "torso" ), 0 );
    harness.drop_client( 0 );
    harness.run_until( []() {
        return false;
    }, 3 );
    CHECK_FALSE( mp_server_any_client_connected() );
    mp_server_end_if_no_clients();
    CHECK( mp_session_end_requested() );
    // Not the death: with nobody at a terminal there is no session to play,
    // whatever state the characters are in. The death itself now leaves the
    // corpse and opens a replacement, which is only possible while the client
    // is still connected.
    CHECK( mp_session_end_reason() == "no players connected" );

    // The ending's teardown (turn_handler::cleanup_at_end) empties the map
    // buffer and the overmaps. A test process goes on to the next test rather
    // than back to the menu, so put back the world tests/test_main.cpp starts
    // every run with, exactly as those tests do.
    get_map() = map();
    overmap_special_batch empty_specials( point_abs_om{} );
    overmap_buffer.create_custom_overmap( point_abs_om{}, empty_specials );
    get_map().load( tripoint_abs_sm( get_map().get_abs_sub() ), false );
    get_avatar().move_to( tripoint_abs_ms::zero );
}

TEST_CASE( "a_partner_disconnect_does_not_end_the_session", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    const std::string name = g->player_at( 1 ).get_name();

    harness.drop_client( 1 );
    harness.run_until( []() {
        return false;
    }, 3 );

    // The dropped seat is marked, and everybody still at a terminal is told so
    // through the one message log the session shares. The session itself keeps
    // running: the server no longer asks the game to end it.
    CHECK_FALSE( mp_session_end_requested() );
    CHECK( mp_player_is_disconnected( 1 ) );
    CHECK( harness.message_log_contains( "Player 2 (" + name + ") disconnected" ) );

    // The partner keeps playing: the disconnected seat no longer holds the
    // round, so a pass by the player who is still there settles it and the
    // calendar moves.
    g->player_at( 0 ).set_moves( 100 );
    const int turn_before = to_turn<int>( calendar::turn );
    queue_plan_pass( 0 );
    CHECK( harness.run_until( [turn_before]() {
        return to_turn<int>( calendar::turn ) > turn_before;
    } ) );

}

TEST_CASE( "a_parked_seat_clears_its_quit_mark_when_marked_disconnected", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );

    // Both seats start with no moves, so player_act() returns at once and each
    // thread parks on the baton. Wait for slot 1 to be parked there: that is
    // the case the clear in mp_run_player_thread() covers, and the one a client
    // taking the seat again (Task 4) leaves behind if the mark is still latched.
    REQUIRE( mp_active_sched() != nullptr );
    REQUIRE( harness.run_until( []() {
        return mp_active_sched() != nullptr && mp_active_sched()->waiting_for_baton( 1 );
    }, 5 ) );

    // What the input wait does when it finds the wire gone: mark the seat and
    // latch the scheduler's per-seat quit mark, which is what answers the
    // seat's menus with QUIT. It is not a session-lifetime fact.
    harness.drop_client( 1 );
    mp_server_mark_disconnected( 1 );
    REQUIRE( mp_player_is_disconnected( 1 ) );
    mp_active_sched()->request_quit( 1 );

    // The seat takes the mark on its next grant and must give it back before
    // parking again. Left latched, a client that returns to this seat and only
    // clears `disconnected` would find every menu answered with QUIT, and its
    // first normal baton grant would take the quit path with the baton still
    // held -- stranding the world on its next yield. This fails cleanly, out of
    // turns, if the clear is missing.
    g->player_at( 0 ).set_moves( 100 );
    harness.press( 0, '.' );
    REQUIRE( harness.run_until( []() {
        return mp_active_sched() != nullptr &&
               !mp_active_sched()->quit_requested( 1 ) &&
               mp_active_sched()->holder() == mp_sched::world;
    }, 10 ) );
}

TEST_CASE( "the_last_client_leaving_ends_the_session_by_itself", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 1 );

    // The whole chain, with nobody calling mp_server_end_if_no_clients() by
    // hand: the input wait notices the dead wire, marks the seat, and the seat
    // count takes the session down. The character is alive on purpose -- a dead
    // one would end the session through do_turn()'s death rule, and the
    // assertion would say nothing about the disconnect path.
    harness.drop_client( 0 );
    harness.run_until( []() {
        return false;
    }, 3 );

    CHECK( mp_session_end_requested() );
    CHECK( mp_session_end_reason() == "no players connected" );

    // Ending a session saves it and tears the world down (end_session in
    // src/do_turn.cpp); put back the world tests/test_main.cpp starts every run
    // with, exactly as the death tests above do.
    get_map() = map();
    overmap_special_batch empty_specials( point_abs_om{} );
    overmap_buffer.create_custom_overmap( point_abs_om{}, empty_specials );
    get_map().load( tripoint_abs_sm( get_map().get_abs_sub() ), false );
    get_avatar().move_to( tripoint_abs_ms::zero );
}

namespace
{
// The per-character save inside the active world. A world may store it plain or
// inside `<base>.sav.zzip`, and the world's compression setting decides which;
// the restart scan below lists either, so the test has to accept either too.
bool character_save_exists( const std::string &save_id )
{
    const cata_path base = PATH_INFO::world_base_save_path() /
                           ( base64_encode( save_id ) + SAVE_EXTENSION );
    return file_exist( base ) || file_exist( base + zzip_suffix );
}

// Whether the active world's save list carries `save_id`. That list is what the
// join menu's Continue entry reads, through mp_continue_target().
bool world_lists_save( const std::string &save_id )
{
    const std::vector<save_t> &saves = world_generator->active_world->world_saves;
    return std::any_of( saves.begin(), saves.end(), [&save_id]( const save_t &save ) {
        return save.decoded_name() == save_id;
    } );
}
} // namespace

// A stopped server used to leave nothing behind. The only save was inside
// end_session() (src/do_turn.cpp), which a signal never reaches: the process
// ends from the signal handler, before the turn loop returns. The shutdown path
// is therefore the second save, and this is the half of it a test can drive:
// exit_handler() itself calls exit(), so what it calls is tested here, then the
// restart scan that fills world_saves for the join menu.
TEST_CASE( "a_stopped_server_leaves_its_characters_behind", "[mp]" )
{
    restore_active_state restore;
    const std::string first = "Session Stop One";
    const std::string second = "Session Stop Two";
    {
        mp_session_harness harness( 2 );
        g->player_at( 0 ).name = first;
        g->player_at( 0 ).set_save_id( first );
        g->player_at( 1 ).name = second;
        g->player_at( 1 ).set_save_id( second );
        // No turn has run, so no autosave has written anything: what appears
        // below is the shutdown call's, not the world's own.
        REQUIRE_FALSE( character_save_exists( first ) );
        REQUIRE_FALSE( character_save_exists( second ) );

        mp_server_save_on_shutdown();

        CHECK( character_save_exists( first ) );
        CHECK( character_save_exists( second ) );
        CHECK( world_lists_save( first ) );
        CHECK( world_lists_save( second ) );
    }

    // The restart the user hit: a fresh directory scan, which is what fills
    // world_saves before the join menu can offer Continue. init() rebuilds the
    // world list from disk, so the active world has to be let go first; it is
    // put back by name afterwards, which is the world every later test expects.
    const std::string world_name = world_generator->active_world->world_name;
    world_generator->set_active_world( nullptr );
    world_generator->init();
    WORLD *rescanned = world_generator->get_world( world_name );
    REQUIRE( rescanned != nullptr );
    world_generator->set_active_world( rescanned );
    CHECK( world_lists_save( first ) );
    CHECK( world_lists_save( second ) );
}

TEST_CASE( "a_reconnecting_client_gets_the_same_character_back", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    const std::string name = g->player_at( 1 ).get_name();
    const std::string save_id = g->player_at( 1 ).get_save_id();

    harness.drop_client( 1 );
    harness.run_until( [&]() {
        return mp_player_is_disconnected( 1 );
    } );
    // A key the old wire had already delivered sits in the seat's queue. The
    // returning terminal never pressed it; it must not become its first input.
    g->contexts[1].input_queue.push_back( input_event( 'x', input_event_t::keyboard_char ) );
    CHECK( harness.reconnect_client( 1, save_id ) );

    CHECK_FALSE( mp_player_is_disconnected( 1 ) );
    CHECK( g->contexts[1].input_queue.empty() );
    CHECK( g->player_at( 1 ).get_name() == name );
    CHECK( harness.message_log_contains( "Player 2 (" + name + ") reconnected" ) );
}

TEST_CASE( "a_character_created_this_session_can_be_reclaimed", "[mp]" )
{
    restore_active_state restore;
    // Join mode: neither client brings a card, so the server hands each new
    // character its card after the roster is seated. That card is the only
    // record connecting the character to its seat; a reconnect must find it.
    mp_session_harness harness( 2, 5, mp_harness_mode::join );
    harness.choose_random_character( 0 );
    harness.choose_random_character( 1 );
    REQUIRE( harness.run_until( [&harness]() {
        return harness.seated();
    }, 100 ) );

    const std::string name = g->player_at( 1 ).get_name();
    const std::string save_id = g->player_at( 1 ).get_save_id();
    REQUIRE_FALSE( save_id.empty() );

    harness.drop_client( 1 );
    REQUIRE( harness.run_until( [&]() {
        return mp_player_is_disconnected( 1 );
    } ) );

    CHECK( harness.reconnect_client( 1, save_id ) );
    CHECK_FALSE( mp_player_is_disconnected( 1 ) );
    CHECK( g->player_at( 1 ).get_name() == name );
}

TEST_CASE( "a_connection_without_a_matching_card_is_refused", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    const std::string name = g->player_at( 1 ).get_name();

    // A seat is waiting for its character, so "no free seat" here is not just
    // the every-seat-taken case: the card is what decides, and this one names
    // nobody in the party.
    harness.drop_client( 1 );
    REQUIRE( harness.run_until( []() {
        return mp_player_is_disconnected( 1 );
    } ) );

    std::string refusal;
    CHECK_FALSE( harness.reconnect_client( 0, "not-a-card-in-this-party", &refusal ) );
    CHECK( refusal == "no free seat" );

    // The refusal left the party exactly as it was: the lost seat still waits
    // for its own character, its name did not change and nobody was dropped.
    CHECK( mp_player_is_disconnected( 1 ) );
    CHECK( harness.seat_occupied( 1 ) );
    CHECK_FALSE( harness.client_connected( 1 ) );
    CHECK( g->player_at( 1 ).get_name() == name );
    CHECK_FALSE( mp_session_end_requested() );

    // And play goes on: the partner's pass settles the round, so the tick
    // closes and the calendar moves.
    g->player_at( 0 ).set_moves( 100 );
    const int turn_before = to_turn<int>( calendar::turn );
    queue_plan_pass( 0 );
    CHECK( harness.run_until( [turn_before]() {
        return to_turn<int>( calendar::turn ) > turn_before;
    } ) );
}

// Measurement scaffolding for F1. Kept in the test binary so the numbers in the
// report can be reproduced with one filtered run instead of scraped from a
// terminal. CPU is /proc/self/stat's utime+stime, i.e. the whole test process,
// which here is the harness thread plus every seat thread. Linux-only: the
// report is the only consumer, so elsewhere this returns "unknown".
#if defined(__linux__)
static long long process_cpu_us()
{
    std::ifstream stat( "/proc/self/stat" );
    std::string line;
    std::getline( stat, line );
    const std::size_t close = line.rfind( ')' );
    if( close == std::string::npos ) {
        return -1;
    }
    std::istringstream rest( line.substr( close + 1 ) );
    // The remainder starts at field 3, which is the state letter, so it is read
    // as text; utime and stime are fields 14 and 15.
    std::string state;
    if( !( rest >> state ) ) {
        return -1;
    }
    long long value = 0;
    long long utime = 0;
    long long stime = 0;
    for( int field = 4; field <= 15; ++field ) {
        if( !( rest >> value ) ) {
            return -1;
        }
        if( field == 14 ) {
            utime = value;
        } else if( field == 15 ) {
            stime = value;
        }
    }
    const long ticks = sysconf( _SC_CLK_TCK );
    return ticks > 0 ? ( utime + stime ) * 1000000LL / ticks : -1;
}
#else
static long long process_cpu_us()
{
    return -1;
}
#endif

// F1 guard: a present seat with AP and no plan holds the round, and the world
// must wait for it instead of running its input poll at the world's own pace.
// Under the round model a parked seat is granted for one of three things only
// -- a key to deliver, a replacement to choose, a quit to unwind -- and a seat
// that owes none of them gets no grant at all. What must still hold is the
// waiting side: one poll interval per turn, no baton traffic, and the waiting
// path asleep rather than spinning.
//
// Three bounds, because they see different regressions. No handoff at all
// catches any grant a parked seat should not have got. The elapsed-time floor
// catches a loop that stopped waiting. CPU pins that the wait sleeps.
TEST_CASE( "an_idle_session_does_not_spin_the_baton", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    // Both seats can act and nobody has a plan: the round is open and waits
    // for them, which is the state a session spends most of its life in.
    g->player_at( 0 ).set_moves( 100 );
    g->player_at( 1 ).set_moves( 100 );
    mp_sched *const sched = mp_active_sched();
    REQUIRE( sched != nullptr );
    REQUIRE( harness.run_until( []() {
        return mp_turn_is_open();
    }, 5 ) );

    constexpr int idle_turns = 20;
    const size_t g0 = sched->holder_generation();
    const std::vector<std::string> trace0 = sched->trace_snapshot();
    const int turn0 = to_turn<int>( calendar::turn );
    const auto wall0 = std::chrono::steady_clock::now();
    const long long cpu0 = process_cpu_us();
    CHECK_FALSE( harness.run_until( []() {
        return false;
    }, idle_turns ) );
    const auto wall1 = std::chrono::steady_clock::now();
    const long long cpu1 = process_cpu_us();
    const size_t delta = sched->holder_generation() - g0;
    const int calendar_turns = to_turn<int>( calendar::turn ) - turn0;
    const long long elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     wall1 - wall0 ).count();

    // Reasons and destinations of whatever handoffs landed in the ring, the
    // test-visible copy of what MP_SCHED_TRACE mirrors. Nothing should be here:
    // no seat owes the world a key, a replacement or a quit.
    std::map<std::string, int> by_reason;
    for( const std::string &entry : sched->trace_snapshot() ) {
        std::istringstream in( entry );
        size_t generation = 0;
        std::string holder;
        std::string reason;
        in >> generation >> holder >> reason;
        if( generation <= g0 ) {
            continue;
        }
        ++by_reason[reason];
    }
    std::string reasons;
    for( const auto &entry : by_reason ) {
        reasons += entry.first + "=" + std::to_string( entry.second ) + " ";
    }

    WARN( "idle handoffs: turns=" << idle_turns << " calendar_turns=" << calendar_turns
          << " delta=" << delta << " elapsed_ms=" << elapsed_ms
          << " cpu_ms=" << ( cpu1 - cpu0 ) / 1000 << " trace_entries_before=" << trace0.size() );
    WARN( "idle handoffs by reason: " << reasons );

    INFO( "handoffs=" << delta << " over " << idle_turns << " turns, elapsed_ms="
          << elapsed_ms );
    // The round never settled, so the calendar never moved and the seat was
    // never granted the baton for a poll cycle.
    CHECK( calendar_turns == 0 );
    CHECK( delta == 0 );
    // One poll interval per turn, with two intervals of slack for a loaded
    // machine. A loop that stopped waiting finishes in milliseconds.
    CHECK( elapsed_ms >= MP_POLL_MS * ( idle_turns - 2 ) );
    // The waiting path sleeps. Under 50% still separates "a blocking wait" from
    // any spin by two orders of magnitude. The probe is Linux-only and
    // report-only otherwise; on a platform without it both samples are -1 and
    // the check is skipped.
    if( cpu0 >= 0 && cpu1 >= 0 && elapsed_ms > 0 ) {
        CHECK( ( cpu1 - cpu0 ) * 2 < elapsed_ms * 1000 );
    }
}

// The field sequence that killed the server: open the inventory, press the
// first stack's hotkey, close, and repeat. The crash was
// inventory_entry::get_invlet() reached from find_by_invlet() while
// process_input() looked the pressed key up: a selector held open across
// another seat's turn boundary, whose entries still held the item_location they
// were built from, because the avatar swap that ends the turn move-assigns a
// direct member such as the wielded item and expires its anchor. No keypress is
// needed for the expiry; the next return from the input wait is what resolves
// the key against the dead row.
// Both seats are driven so the round loop and the per-turn context switches
// run exactly as they do on a live server.
TEST_CASE( "inventory_reopen_cycles_do_not_crash", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    // Both seats can act. Seat 0 opens a free menu; closing the tick under it
    // is what the pass below does, and the partner's pass makes the round
    // settle while seat 0 is still reading.
    g->player_at( 0 ).set_moves( 100 );
    g->player_at( 1 ).set_moves( 100 );
    // A wielded item lives directly in the avatar, so it travels with the
    // avatar when the server swaps the active context between turns. The
    // inventory entry built for it holds an item_location into that avatar.
    g->player_at( 0 ).set_wielded_item( item( itype_id( "rock" ) ) );

    const auto inventory_is_open = [&]() {
        return mp_grid_contains( harness.screen( 0 ), mp_menu_inventory.marker );
    };

    for( int cycle = 0; cycle < 4; ++cycle ) {
        CAPTURE( cycle );
        harness.press( 0, mp_menu_inventory.key );
        REQUIRE( harness.run_until( inventory_is_open ) );
        // Seat 0 is done for the tick (its plan is a pass) and so is seat 1,
        // so the round settles and the tick closes while seat 0's menu stays
        // open -- the spec's accepted "your choice may have gone stale" case.
        // The turn boundary swaps the active avatar out and back; the entries
        // built above are not rebuilt by any of it.
        const int turn_before = to_turn<int>( calendar::turn );
        queue_plan_pass( 0 );
        queue_plan_pass( 1 );
        REQUIRE( harness.run_until( [turn_before]() {
            return to_turn<int>( calendar::turn ) > turn_before;
        } ) );
        // The calendar moving only proves the round settled. The pin is only
        // real if seat 0 is still parked in the pick selector when the hotkey
        // is delivered: otherwise the press lands on the map screen and the
        // test goes green while reproducing nothing.
        CHECK( inventory_is_open() );
        // The hotkey lookup is the frame that dereferenced the stale location.
        harness.press( 0, 'a' );
        harness.run_until( []() {
            return false;
        }, 1 );
        harness.press( 0, KEY_ESCAPE );
        REQUIRE( harness.run_until( [&]() {
            return !inventory_is_open();
        }, 20 ) );
    }
    SUCCEED( "four inventory reopen cycles did not crash the server" );
}

// The multiselector branch of the same crash. Marking the wielded item fills
// to_use with an item_location the turn boundary then kills; the drop menu's
// header stats and selection pane dereference to_use, so the post-wait drop has
// to discard the dead pair before it rebuilds the layout. Without that, the
// keypress that ends the wait dies in inventory_drop_selector::get_raw_stats().
TEST_CASE( "a_dead_drop_selection_is_discarded_without_a_crash", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    g->player_at( 0 ).set_moves( 100 );
    g->player_at( 1 ).set_moves( 100 );
    // Direct item member of the avatar: the turn boundary's avatar swap is what
    // expires its item_location.
    g->player_at( 0 ).set_wielded_item( item( itype_id( "rock" ) ) );

    const auto drop_menu_is_open = [&]() {
        return mp_grid_contains( harness.screen( 0 ), "Multidrop" );
    };
    const auto screen_text = [&]() {
        const mp_screen_grid &grid = harness.screen( 0 );
        std::string text;
        for( int y = 0; y < grid.height(); ++y ) {
            for( int x = 0; x < grid.width(); ++x ) {
                text += grid.at( x, y ).ch;
            }
            text += '\n';
        }
        return text;
    };
    // The item row and the selection pane both render the marked item's name, on
    // the same screen row when the columns fit side by side, so count occurrences.
    const auto rock_cells = [&]() {
        const mp_screen_grid &grid = harness.screen( 0 );
        int hits = 0;
        for( int y = 0; y < grid.height(); ++y ) {
            std::string row;
            for( int x = 0; x < grid.width(); ++x ) {
                row += grid.at( x, y ).ch;
            }
            for( std::string::size_type pos = row.find( "rock" ); pos != std::string::npos;
                 pos = row.find( "rock", pos + 1 ) ) {
                ++hits;
            }
        }
        return hits;
    };

    // 'd' opens the multidrop menu with the cleared avatar's one item. The
    // wielded item lives in the gear column, which is not the initially active
    // (empty) inventory column, so step to it first; TOGGLE_ENTRY then marks the
    // highlighted rock and fills to_use.
    harness.press( 0, 'd' );
    REQUIRE( harness.run_until( drop_menu_is_open ) );
    harness.press( 0, 'h' );
    harness.press( 0, 'l' );
    const bool marked = harness.run_until( [&]() {
        return rock_cells() == 2;
    } );
    INFO( screen_text() );
    REQUIRE( marked );

    // The round takes the tick's boundary while the menu stays open: both
    // seats pass, so the round settles with nothing to run, the tick closes
    // and reopens, and the avatar swap that closes it expires the marked row's
    // item_location. A done-for-the-tick seat can still browse.
    const int turn_before = to_turn<int>( calendar::turn );
    queue_plan_pass( 0 );
    queue_plan_pass( 1 );
    REQUIRE( harness.run_until( [turn_before]() {
        return to_turn<int>( calendar::turn ) > turn_before;
    } ) );
    // The world moved under the open menu and the marked row's location died.
    CHECK( drop_menu_is_open() );
    CHECK( g->player_at( 0 ).get_wielded_item() );

    // This keypress is the one that used to dereference the dead to_use pair.
    harness.press( 0, 'l' );
    REQUIRE( harness.run_until( [&]() {
        return rock_cells() == 0;
    }, 20 ) );
    CHECK( drop_menu_is_open() );

    // Cancel: the dead selection is discarded rather than reaching the drop.
    harness.press( 0, KEY_ESCAPE );
    REQUIRE( harness.run_until( [&]() {
        return !drop_menu_is_open();
    }, 20 ) );
    CHECK( g->player_at( 0 ).get_wielded_item() );
}

namespace
{
// Ends the tick while seat 0 sits in a menu: both seats are done for the round
// (a pass each), so the round settles and the tick closes under the open menu --
// the spec's accepted "your choice may have gone stale" case. The turn boundary
// is what expires the wielded item's item_location (a direct avatar member is
// move-assigned by the avatar swap, and a move-assigned item installs a fresh
// safe_reference anchor). Returns false when the boundary did not arrive within
// the budget.
bool take_a_partner_turn( mp_session_harness &harness )
{
    const int turn_before = to_turn<int>( calendar::turn );
    queue_plan_pass( 0 );
    queue_plan_pass( 1 );
    return harness.run_until( [turn_before]() {
        return to_turn<int>( calendar::turn ) > turn_before;
    } );
}
} // namespace

// The corpse is a row in a column that still has a live row below it: a wielded
// item (direct avatar member, dies at the boundary) above a worn one (list node,
// survives). Before the choice was held by location the erased rock's index was
// left pointing at the shirt, and CONFIRM returned an item the player never
// chose. §7.3 promises the ordinary refusal instead: the menu stays open and
// nothing is returned.
TEST_CASE( "a_prune_does_not_move_the_confirmed_row", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    // Non-interactive: wearing through the API otherwise drains the moves the
    // seat needs to be actionable this turn.
    REQUIRE( g->player_at( 0 ).wear_item( item( itype_id( "longshirt" ) ), false ) );
    g->player_at( 0 ).set_wielded_item( item( itype_id( "rock" ) ) );
    g->player_at( 0 ).set_moves( 100 );
    g->player_at( 1 ).set_moves( 100 );

    const auto inventory_is_open = [&]() {
        return mp_grid_contains( harness.screen( 0 ), mp_menu_inventory.marker );
    };

    // The gear column sorts the held weapon before the worn items, so the default
    // highlight is the rock: index 1, right above the shirt.
    harness.press( 0, mp_menu_inventory.key );
    REQUIRE( harness.run_until( inventory_is_open ) );
    REQUIRE( take_a_partner_turn( harness ) );
    CHECK( inventory_is_open() );

    // The rock's row is gone and the shirt is still selectable. With CONFIRM
    // answered by position this opens the shirt's item menu and leaves the
    // inventory; the refusal leaves both where they were.
    harness.press( 0, KEY_ENTER );
    harness.run_until( []() {
        return false;
    }, 2 );
    CHECK( inventory_is_open() );

    harness.press( 0, KEY_ESCAPE );
    REQUIRE( harness.run_until( [&]() {
        return !inventory_is_open();
    }, 20 ) );
}

// Paging walks from the highlight, and the walk used to have no way to notice
// that it had come full circle: PageDown landed on the only highlightable row
// and stayed there, PageUp spun on the SIZE_MAX sentinel, and either way the
// seat never came back to an input wait. The menu must keep answering.
TEST_CASE( "paging_survives_a_cleared_highlight", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    // Non-interactive: wearing through the API otherwise drains the moves the
    // seat needs to be actionable this turn.
    REQUIRE( g->player_at( 0 ).wear_item( item( itype_id( "longshirt" ) ), false ) );
    g->player_at( 0 ).set_wielded_item( item( itype_id( "rock" ) ) );
    g->player_at( 0 ).set_moves( 100 );
    g->player_at( 1 ).set_moves( 100 );

    const auto inventory_is_open = [&]() {
        return mp_grid_contains( harness.screen( 0 ), mp_menu_inventory.marker );
    };

    harness.press( 0, mp_menu_inventory.key );
    REQUIRE( harness.run_until( inventory_is_open ) );

    // The rock dies at the boundary; the prune clears the highlight when the
    // next key arrives. The gear column then has a category row in front of the
    // only highlightable row left (the worn shirt), which is the shape that
    // made the forward page walk repeat on the spot.
    REQUIRE( take_a_partner_turn( harness ) );
    REQUIRE( inventory_is_open() );

    // Paging must not close the menu or stop answering; the wait also gives the
    // walk a turn boundary to be stuck on if it is broken.
    harness.press( 0, KEY_NPAGE );
    REQUIRE_FALSE( harness.run_until( [&]() {
        return !inventory_is_open();
    }, 20 ) );

    // PageUp used to loop here. Bounded, like the close below: a reintroduced
    // loop shows up as a failed case rather than a silent hang.
    harness.press( 0, KEY_PPAGE );
    harness.press( 0, KEY_ESCAPE );
    REQUIRE( harness.run_until( [&]() {
        return !inventory_is_open();
    }, 20 ) );
}

// The filter prompt is a second input wait inside the same menu, and
// query_string() prunes on its way out. The world can take seat 1's turn while
// the prompt is open, so the row that dies is only discovered when the prompt
// closes; the CONFIRM that follows must still refuse rather than take the row
// that moved up.
TEST_CASE( "a_dead_row_is_not_replaced_under_the_filter_prompt", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    // Non-interactive: wearing through the API otherwise drains the moves the
    // seat needs to be actionable this turn.
    REQUIRE( g->player_at( 0 ).wear_item( item( itype_id( "longshirt" ) ), false ) );
    g->player_at( 0 ).set_wielded_item( item( itype_id( "rock" ) ) );
    g->player_at( 0 ).set_moves( 100 );
    g->player_at( 1 ).set_moves( 100 );

    const auto inventory_is_open = [&]() {
        return mp_grid_contains( harness.screen( 0 ), mp_menu_inventory.marker );
    };

    harness.press( 0, mp_menu_inventory.key );
    REQUIRE( harness.run_until( inventory_is_open ) );

    // '/' opens the item-filter prompt; the screen changes when the popup draws,
    // which is the only marker it has. Nothing is typed: the empty filter is
    // confirmed with Enter below.
    const mp_screen_grid before_prompt = harness.screen( 0 );
    harness.press( 0, '/' );
    REQUIRE( harness.run_until( [&]() {
        return !mp_grids_equal( harness.screen( 0 ), before_prompt );
    }, 20 ) );

    // The boundary lands while the prompt is the active wait.
    REQUIRE( take_a_partner_turn( harness ) );

    // Confirm the filter, then CONFIRM the row: the rock died under the prompt,
    // the shirt is alive, and the highlight must not have moved onto it.
    harness.press( 0, KEY_ENTER );
    harness.run_until( []() {
        return false;
    }, 1 );
    harness.press( 0, KEY_ENTER );
    harness.run_until( []() {
        return false;
    }, 2 );
    CHECK( inventory_is_open() );

    harness.press( 0, KEY_ESCAPE );
    REQUIRE( harness.run_until( [&]() {
        return !inventory_is_open();
    }, 20 ) );
}
// The compare selector kept its selection as raw item pointers. An item on the
// map is not an avatar member, so a partner's turn that removes it frees the
// object instead of leaving a moved-from copy behind: the pointer really
// dangles, and it used to be handed straight to compare_item_menu once a second
// item was marked. The selection has to be held as item_location and the dead
// one dropped by the post-wait prune.
TEST_CASE( "a_dead_compare_selection_is_not_compared", "[mp]" )
{
    restore_active_state restore;
    mp_session_harness harness( 2 );
    // Both live items are worn: inventory list nodes survive the avatar swap that
    // ends the partner's turn, unlike the wielded item's direct member. The map
    // rock is then the only selection that can expire under the menu.
    REQUIRE( g->player_at( 0 ).wear_item( item( itype_id( "longshirt" ) ), false ) );
    REQUIRE( g->player_at( 0 ).wear_item( item( itype_id( "socks" ) ), false ) );
    g->player_at( 0 ).set_moves( 100 );
    g->player_at( 1 ).set_moves( 100 );

    // The rock sits on the partner's tile: adjacent to seat 0, so the compare
    // menu's nearby-items pass lists it, and on the tile the partner's turn can
    // remove it from.
    g->player_at( 1 ).setpos( get_map(),
                              g->player_at( 0 ).pos_bub() + tripoint_rel_ms( 1, 0, 0 ) );
    const tripoint_bub_ms rock_pos = g->player_at( 1 ).pos_bub();
    get_map().add_item_or_charges( rock_pos, item( itype_id( "rock" ) ) );

    const auto compare_is_open = [&]() {
        return mp_grid_contains( harness.screen( 0 ), "ITEMS TO COMPARE" );
    };
    // The item row and the selection pane both render the marked item's name, on
    // the same screen row when the columns fit side by side, so count occurrences.
    const auto rock_cells = [&]() {
        const mp_screen_grid &grid = harness.screen( 0 );
        int hits = 0;
        for( int y = 0; y < grid.height(); ++y ) {
            std::string row;
            for( int x = 0; x < grid.width(); ++x ) {
                row += grid.at( x, y ).ch;
            }
            for( std::string::size_type pos = row.find( "rock" ); pos != std::string::npos;
                 pos = row.find( "rock", pos + 1 ) ) {
                ++hits;
            }
        }
        return hits;
    };

    harness.press( 0, 'I' );
    REQUIRE( harness.run_until( compare_is_open ) );

    // At this terminal width the columns are merged into one, and the map item
    // is added after the character's gear, so it is the last row. Mark it.
    harness.press( 0, KEY_END );
    harness.press( 0, 'l' );
    REQUIRE( harness.run_until( [&]() {
        return rock_cells() == 2;
    }, 20 ) );

    // A pickup would move the rock into an inventory and destroy it at the old
    // address just the same, so clearing the tile is the partner's turn as far
    // as the saved location is concerned: it expires while the menu waits. Both
    // seats pass, so the round settles, the tick closes and reopens, and a
    // done-for-the-tick seat can still browse.
    get_map().i_clear( rock_pos );
    REQUIRE( take_a_partner_turn( harness ) );
    CHECK( compare_is_open() );

    // Mark one of the live items. The dead rock must already be gone from the
    // selection; a raw pointer kept across the wait would complete the pair
    // here and open the comparison window instead. The first key after the
    // wait is also the one that runs the prune, so it only sets the highlight.
    harness.press( 0, KEY_HOME );
    harness.press( 0, 'l' );
    harness.run_until( []() {
        return false;
    }, 3 );
    CHECK( compare_is_open() );

    // A second live item completes a live pair, which does open the comparison.
    harness.press( 0, KEY_END );
    harness.press( 0, 'l' );
    REQUIRE( harness.run_until( [&]() {
        return !compare_is_open();
    }, 20 ) );

    // Close the comparison; the compare selector reopens, so Escape twice returns
    // to the main screen. Reaching it proves the pair was usable and the server
    // survived the case.
    harness.press( 0, KEY_ESCAPE );
    REQUIRE( harness.run_until( compare_is_open, 20 ) );
    harness.press( 0, KEY_ESCAPE );
    REQUIRE( harness.run_until( [&]() {
        return !compare_is_open();
    }, 20 ) );
}

TEST_CASE( "mp_session_ui", "[mp]" )
{
    SECTION( "a describe seat is describable only while in game" ) {
        mp_session seat;
        seat.caps = MP_CAP_DESCRIBE;
        CHECK_FALSE( mp_session_ui_active( seat ) );
        CHECK_FALSE( mp_session_can_describe( seat ) );

        mp_session_ui_enter( seat );
        CHECK( mp_session_ui_active( seat ) );
        CHECK( mp_session_can_describe( seat ) );

        seat.describe_pending = mp_describe_request{ 4, 10, 20, 30 };
        seat.describe_last = std::chrono::steady_clock::now();
        mp_session_ui_leave( seat );
        CHECK_FALSE( mp_session_ui_active( seat ) );
        CHECK_FALSE( mp_session_can_describe( seat ) );
        CHECK_FALSE( seat.describe_pending.has_value() );
        CHECK( seat.describe_last == std::chrono::steady_clock::time_point{} );
    }

    SECTION( "no describe capability means no describe even in game" ) {
        mp_session seat;
        // Scene and log but not bit2.
        seat.caps = 3;
        mp_session_ui_enter( seat );
        CHECK( mp_session_ui_active( seat ) );
        CHECK_FALSE( mp_session_can_describe( seat ) );
    }
}

#endif // MP_SERVER
