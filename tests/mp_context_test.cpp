#include "cata_catch.h"

#include <memory>
#include <vector>

#include "cata_scope_helpers.h"
#include "game.h"
#include "game_test_helper.h"
#include "live_view.h"
#include "map.h"
#include "mp/mp_context.h"
#include "output.h"
#include "panels.h"
#include "popup.h"
#include "ui_manager.h"

TEST_CASE( "mp_apply_viewport_params_computes_metrics_from_the_context", "[mp]" )
{
    // The metrics are globals; save and restore all of them to avoid polluting neighboring tests.
    restore_on_out_of_scope restore_termx( TERMX );
    restore_on_out_of_scope restore_termy( TERMY );
    restore_on_out_of_scope restore_tww( TERRAIN_WINDOW_WIDTH );
    restore_on_out_of_scope restore_twh( TERRAIN_WINDOW_HEIGHT );
    restore_on_out_of_scope restore_twtw( TERRAIN_WINDOW_TERM_WIDTH );
    restore_on_out_of_scope restore_twth( TERRAIN_WINDOW_TERM_HEIGHT );
    restore_on_out_of_scope restore_posx( POSX );
    restore_on_out_of_scope restore_posy( POSY );

    // Terminal 120x40, default side panels. We verify that the metrics are a
    // pure function of the viewport, with no hidden sources.
    player_context ctx;
    ctx.viewport.term_cols = 120;
    ctx.viewport.term_rows = 40;

    mp_apply_viewport_params( ctx );

    CHECK( TERMX == 120 );
    CHECK( TERMY == 40 );
    // Terrain window: the full width minus the side panels.
    const int sidebar = panel_manager::get_manager().get_width_left() +
                        panel_manager::get_manager().get_width_right();
    CHECK( TERRAIN_WINDOW_WIDTH + sidebar == 120 );
    CHECK( TERRAIN_WINDOW_HEIGHT == 40 );
    CHECK( POSX == TERRAIN_WINDOW_WIDTH / 2 );
    CHECK( POSY == TERRAIN_WINDOW_HEIGHT / 2 );
}

namespace {

// Restores the active context to the previous one (may be nullptr).
class restore_active_context
{
        player_context *const saved_;
    public:
        restore_active_context() : saved_( mp_active_context_ptr() ) {}
        ~restore_active_context() { mp_set_active_context( saved_ ); }
};

} // namespace

TEST_CASE( "mp_active_context_follows_the_last_set_context", "[mp]" )
{
    restore_active_context restore;
    player_context a;
    player_context b;

    mp_set_active_context( &a );
    CHECK( &mp_active_context() == &a );
    mp_set_active_context( &b );
    CHECK( &mp_active_context() == &b );
    mp_set_active_context( &a );
    CHECK( &mp_active_context() == &a );
}

TEST_CASE( "view_state_written_under_one_context_is_invisible_to_another", "[mp]" )
{
    restore_active_context restore;
    player_context a;
    player_context b;

    mp_set_active_context( &a );
    mp_active_context().ter_view_p = tripoint_bub_ms( 5, 6, 0 );
    mp_active_context().driving_view_offset = point_rel_ms( 1, 2 );

    mp_set_active_context( &b );
    // B's view state is not touched by A's activation.
    CHECK( mp_active_context().ter_view_p == tripoint_bub_ms() );
    CHECK( mp_active_context().driving_view_offset == point_rel_ms() );

    mp_set_active_context( &a );
    CHECK( mp_active_context().ter_view_p == tripoint_bub_ms( 5, 6, 0 ) );
    CHECK( mp_active_context().driving_view_offset == point_rel_ms( 1, 2 ) );
}

// ter_view_p is a cached bubble cell, and the map shift relocates creatures
// but not that cache. Both seats have to move by the delta the shift applied,
// from their own centres, or every seat's frame names a tile one submap away.
TEST_CASE( "mp_shift_context_views_moves_every_seat_by_the_map_delta", "[mp][view]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    g->add_player_slot();
    mp_sync_context_count();
    REQUIRE( g->contexts.size() == 2 );

    // The seats stand at different places and different z: a shift has to be
    // applied to each centre on its own, not to one spread across the others.
    g->contexts[0].ter_view_p = tripoint_bub_ms( 10, 20, -1 );
    g->contexts[1].ter_view_p = tripoint_bub_ms( 70, 30, 0 );

    // One submap west and two south: the map's origin moves that way, so every
    // bubble cell loses a submap -- x grows by SEEX, y drops by 2*SEEY.
    mp_shift_context_views( point_rel_sm( -1, 2 ) );

    CHECK( g->contexts[0].ter_view_p == tripoint_bub_ms( 10 + SEEX, 20 - 2 * SEEY, -1 ) );
    CHECK( g->contexts[1].ter_view_p == tripoint_bub_ms( 70 + SEEX, 30 - 2 * SEEY, 0 ) );

    // A zero shift moved nothing: the no-op lives in the function, not in the
    // callers, so a caller that forwards every computed shift is safe.
    const tripoint_bub_ms kept_0 = g->contexts[0].ter_view_p;
    const tripoint_bub_ms kept_1 = g->contexts[1].ter_view_p;
    mp_shift_context_views( point_rel_sm::zero );
    CHECK( g->contexts[0].ter_view_p == kept_0 );
    CHECK( g->contexts[1].ter_view_p == kept_1 );

    mp_activate_player( 0 );
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

// The view centre only leaves the character while look mode runs. A tick opens
// with the world holding the baton, so the tick start puts every seat back on
// its own cell before the preamble reads it.
TEST_CASE( "mp_recenter_context_views_puts_each_seat_on_its_character", "[mp][view]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    g->add_player_slot();
    mp_sync_context_count();
    REQUIRE( g->contexts.size() == 2 );

    avatar &seat0 = g->player_at( 0 );
    avatar &seat1 = g->player_at( 1 );
    // Seat 0 stays where the test harness put it; only seat 1 is placed, so
    // nothing outside this case inherits a moved leader.
    seat1.setpos( get_map(), seat0.pos_bub() + tripoint_rel_ms( 2, 1, 0 ) );
    REQUIRE( seat1.pos_bub() != seat0.pos_bub() );

    g->contexts[0].ter_view_p = seat0.pos_bub();
    g->contexts[1].ter_view_p = seat1.pos_bub() + tripoint_rel_ms( 6, 6, 0 );

    mp_recenter_context_views();

    CHECK( g->contexts[0].ter_view_p == seat0.pos_bub() );
    CHECK( g->contexts[1].ter_view_p == seat1.pos_bub() );

    mp_activate_player( 0 );
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

TEST_CASE( "each_context_keeps_its_own_overmap_view", "[mp]" )
{
    restore_active_context restore;
    player_context a;
    player_context b;

    const tripoint_abs_omt default_origin( -1, -1, -1 );
    const point_omt_ms default_remainder( SEEX, SEEY );

    mp_set_active_context( &a );
    overmap_view_state &view_a = mp_active_overmap_view();
    // A fresh context starts from the defaults the shared struct used to have.
    CHECK( view_a.origin_pos == default_origin );
    CHECK( view_a.origin_remainder == default_remainder );
    CHECK( view_a.cursor_pos == default_origin );

    const tripoint_abs_omt a_origin( 10, 20, 0 );
    const point_omt_ms a_remainder( 3, 4 );
    const tripoint_abs_omt a_cursor( 11, 21, 0 );
    view_a.origin_pos = a_origin;
    view_a.origin_remainder = a_remainder;
    view_a.cursor_pos = a_cursor;

    mp_set_active_context( &b );
    overmap_view_state &view_b = mp_active_overmap_view();
    // Activating another seat must not carry seat A's view across.
    CHECK( view_b.origin_pos == default_origin );
    CHECK( view_b.origin_remainder == default_remainder );
    CHECK( view_b.cursor_pos == default_origin );
    CHECK( view_b.cursor_pos != a_cursor );
    view_b.origin_pos = tripoint_abs_omt( 30, 40, 0 );
    view_b.cursor_pos = tripoint_abs_omt( 31, 41, 0 );

    // And seat A's view survived while B wrote its own.
    mp_set_active_context( &a );
    CHECK( mp_active_overmap_view().origin_pos == a_origin );
    CHECK( mp_active_overmap_view().origin_remainder == a_remainder );
    CHECK( mp_active_overmap_view().cursor_pos == a_cursor );
}

TEST_CASE( "ui_adaptors_register_into_the_active_stack", "[mp]" )
{
    restore_active_state restore;
    ui_stack_t stack_a;
    ui_stack_t stack_b;

    ui_manager::set_active_stack( &stack_a );
    {
        ui_adaptor ui;
        CHECK( ui_adaptor::ui_stack_size() == 1 );
    }
    // Destructor ran under the same active stack - the ownership invariant holds.
    CHECK( ui_adaptor::ui_stack_size() == 0 );

    ui_manager::set_active_stack( &stack_b );
    {
        ui_adaptor ui;
        CHECK( ui_adaptor::ui_stack_size() == 1 );
        CHECK( stack_a.size() == 0 );
        CHECK( stack_b.size() == 1 );
    }
    CHECK( ui_adaptor::ui_stack_size() == 0 );
    // restore_active_state's destructor restores the previous active stack.
}

TEST_CASE( "ensure_creates_the_single_context_and_activates_it", "[mp]" )
{
    mp_ensure_player_context();
    CHECK( g->contexts.size() == 1 );
    CHECK( &mp_active_context() == &g->contexts[0] );
    CHECK( g->contexts[0].viewport.term_cols == TERMX );
}

TEST_CASE( "activating_slots_in_sequence_preserves_each_slots_content", "[mp]" )
{
    restore_active_state restore;
    game_test_helper::reset_player_slots();
    // Do not clear g->contexts: the active pointer would then point at a freed
    // element, and restore_active_state would restore that dangling pointer.
    // ensure reuses the existing contexts[0] or creates a new one and in any
    // case makes it active.
    mp_ensure_player_context();

    g->player_at( 0 ).name = "SlotZero";
    avatar &second = g->add_player_slot();
    second.name = "SlotOne";
    avatar &third = g->add_player_slot();
    third.name = "SlotTwo";
    // Callers that add a slot must sync contexts before activating it:
    // mp_activate_player() indexes contexts[idx] directly now and asserts idx
    // is in range, instead of always using contexts[0] as it did in Phase 2.
    mp_sync_context_count();

    mp_activate_player( 1 );
    CHECK( get_avatar().name == "SlotOne" );
    CHECK( mp_active_context_ptr() == &g->contexts[1] );
    get_avatar().name = "SlotOneEdited";
    // Content is the context's own state, not just the swapped-in avatar:
    // write through slot 1's context so the later re-activation can prove it
    // survives, not merely that the avatar swap does.
    mp_active_context().driving_view_offset = point_rel_ms( 3, 5 );

    mp_activate_player( 2 );
    CHECK( get_avatar().name == "SlotTwo" );
    CHECK( mp_active_context_ptr() == &g->contexts[2] );
    // Slot 2's context is a separate object: it must not see what was just
    // written through slot 1's.
    CHECK( mp_active_context().driving_view_offset != point_rel_ms( 3, 5 ) );
    mp_active_context().driving_view_offset = point_rel_ms( 8, 1 );

    // Back to slot 1: both the avatar swap and the context content are
    // preserved (an unpaired swap, or activation that still reads contexts[0]
    // regardless of idx, would break this).
    mp_activate_player( 1 );
    CHECK( get_avatar().name == "SlotOneEdited" );
    CHECK( mp_active_context().driving_view_offset == point_rel_ms( 3, 5 ) );

    mp_activate_player( 2 );
    CHECK( mp_active_context().driving_view_offset == point_rel_ms( 8, 1 ) );

    mp_activate_player( 0 );
    CHECK( get_avatar().name == "SlotZero" );

    // Restore the single-slot layout. The extra slots created here are
    // default-constructed avatars with no bodyparts; leaving them in
    // player_slots made any later test that iterates every slot
    // (weather.cpp handle_weather_effects -> wet_character) read a bodyless
    // avatar and log an error for every weather tick.
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

TEST_CASE( "mp_activate_player_zero_is_the_identity_swap", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    const catacurses::window stdscr_before = catacurses::stdscr;
    const catacurses::window terrain_before = g->w_terrain;
    const int termx_before = TERMX;
    const int termy_before = TERMY;

    mp_activate_player( 0 );

    CHECK( catacurses::stdscr == stdscr_before );
    CHECK( g->w_terrain == terrain_before );
    CHECK( TERMX == termx_before );
    CHECK( TERMY == termy_before );
    CHECK( &get_avatar() == &g->player_at( 0 ) );
}

TEST_CASE( "contexts_track_the_slot_count", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    REQUIRE( g->contexts.size() == 1 );

    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();
    CHECK( g->contexts.size() == 2 );
    // A fresh context inherits the terminal size of context 0 until its client
    // reports its own.
    CHECK( g->contexts[1].viewport.term_cols == g->contexts[0].viewport.term_cols );
    CHECK( g->contexts[1].viewport.term_rows == g->contexts[0].viewport.term_rows );

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    CHECK( g->contexts.size() == 1 );
}

TEST_CASE( "activating_a_player_selects_that_players_context", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();

    mp_activate_player( 0 );
    CHECK( mp_active_context_ptr() == &g->contexts[0] );
    CHECK( ui_manager::get_active_stack() == &g->contexts[0].ui_stack );

    mp_activate_player( 1 );
    CHECK( mp_active_context_ptr() == &g->contexts[1] );
    CHECK( ui_manager::get_active_stack() == &g->contexts[1].ui_stack );

    // View state is per-context, not shared: writing through one player's
    // context must not be visible through the other's.
    mp_active_context().driving_view_offset = point_rel_ms( 7, 9 );
    mp_activate_player( 0 );
    CHECK( mp_active_context().driving_view_offset != point_rel_ms( 7, 9 ) );

    mp_activate_player( 0 );
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

TEST_CASE( "the_world_phase_has_no_active_player", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    g->add_player_slot().set_body();
    mp_sync_context_count();
    mp_activate_player( 0 );

    mp_activate_world();

    // With more than one player the world stack is a real object of its own:
    // UI raised during the world phase must land somewhere rather than
    // dereference null, and not in either player's stack.
    CHECK( ui_manager::get_active_stack() != nullptr );
    CHECK( ui_manager::get_active_stack() != &g->contexts[0].ui_stack );
    CHECK( ui_manager::get_active_stack() != &g->contexts[1].ui_stack );

    mp_activate_player( 0 );
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

// Single-player runs the world's half of the turn through the same
// mp_activate_world() as a server session. NPCs finishing an activity there
// call g->wait_popup_reset(), which drops the one player's progress popup
// ("Wait till you wake up..."); that popup registered in the player's ui stack,
// and ~ui_adaptor asserts it dies in the stack it registered in.
TEST_CASE( "a_single_player_popup_survives_a_reset_from_the_world_phase", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    mp_activate_player( 0 );
    g->contexts[0].wait_popup = std::make_unique<static_popup>();

    mp_activate_world();
    g->wait_popup_reset();

    CHECK( g->contexts[0].wait_popup == nullptr );
    mp_activate_player( 0 );
}

TEST_CASE( "the_world_phase_brings_slot_zero_back_into_u", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();

    g->player_at( 0 ).name = "SlotZero";
    b.name = "SlotOne";

    // Leave slot 1 active, the way do_turn()'s player loop does right before
    // its second mp_activate_world() call at N > 1 (the last iteration
    // activates the last slot).
    mp_activate_player( 1 );
    REQUIRE( get_avatar().name == "SlotOne" );

    mp_activate_world();

    // The world half must see one consistent player: g->u and
    // mp_active_context() both name slot 0, not two different players. Before
    // this was fixed, mp_activate_world() forced mp_active_context() to slot 0
    // but never touched g->u, so a caller like mon_info_update() that reads
    // get_avatar() would see the last-active player instead.
    CHECK( get_avatar().name == "SlotZero" );
    CHECK( &mp_active_context() == &g->contexts[0] );

    mp_activate_player( 0 );
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

TEST_CASE( "for_each_context_visits_every_player_and_restores", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    avatar &b = g->add_player_slot();
    b.set_body();
    mp_sync_context_count();
    mp_activate_player( 1 );

    std::vector<player_context *> seen;
    mp_for_each_context( [&seen]( player_context & ctx ) {
        seen.push_back( &ctx );
    } );

    CHECK( seen.size() == 2 );
    CHECK( seen[0] == &g->contexts[0] );
    CHECK( seen[1] == &g->contexts[1] );
    // The caller's context is restored: broadcast drawing must not leave the
    // baton holder pointing at somebody else's windows.
    CHECK( mp_active_context_ptr() == &g->contexts[1] );

    // Called from the world phase -- the broadcast paths' actual caller --
    // mp_for_each_context() must restore the world phase, not silently leave
    // a player active. The active slot alone cannot distinguish the two: the
    // world phase also rests on slot 0 (mp_activate_world()), same as a
    // caller that had activated player 0 directly.
    mp_activate_world();
    seen.clear();
    mp_for_each_context( [&seen]( player_context & ctx ) {
        seen.push_back( &ctx );
    } );
    CHECK( seen.size() == 2 );
    CHECK( ui_manager::get_active_stack() != &g->contexts[0].ui_stack );
    CHECK( ui_manager::get_active_stack() != &g->contexts[1].ui_stack );

    mp_activate_player( 0 );
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

TEST_CASE( "each_players_mouse_view_lives_in_that_players_ui_stack", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    g->add_player_slot().set_body();
    mp_sync_context_count();

    mp_activate_player( 0 );
    mp_active_live_view().show( tripoint::zero );
    REQUIRE( mp_active_live_view().is_enabled() );

    // The hint is per player: showing slot 0's must not enable slot 1's.
    mp_activate_player( 1 );
    CHECK_FALSE( mp_active_live_view().is_enabled() );
    mp_active_live_view().show( tripoint::zero );
    REQUIRE( mp_active_live_view().is_enabled() );

    // Hiding slot 1's view must not touch slot 0's.
    mp_active_live_view().hide();
    mp_activate_player( 0 );
    CHECK( mp_active_live_view().is_enabled() );
    mp_active_live_view().hide();

    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

TEST_CASE( "a_destroyed_context_drops_its_mouse_view_where_it_registered", "[mp]" )
{
    restore_active_state restore;
    ui_stack_t other;
    ui_manager::set_active_stack( &other );
    {
        player_context ctx;
        ui_manager::set_active_stack( &ctx.ui_stack );
        ctx.liveview = std::make_unique<live_view>();
        ctx.liveview->show( tripoint::zero );
        REQUIRE( ctx.ui_stack.size() == 1 );
        // Destroy the context under somebody else's stack, the way g.reset()
        // destroys contexts during shutdown. ~ui_adaptor asserts its owner is
        // the active stack, so the context destructor has to point the stack
        // at itself for the drop. Reaching the end of this block is the
        // assertion: a context whose destructor did not do that aborts.
        ui_manager::set_active_stack( &other );
    }
    CHECK( other.empty() );
}

TEST_CASE( "a_context_destroyed_under_its_own_stack_restores_the_world_stack", "[mp]" )
{
    restore_active_state restore;
    ui_stack_t *dead = nullptr;
    {
        player_context ctx;
        dead = &ctx.ui_stack;
        ui_manager::set_active_stack( &ctx.ui_stack );
        ctx.liveview = std::make_unique<live_view>();
        ctx.liveview->show( tripoint::zero );
        REQUIRE( ctx.ui_stack.size() == 1 );
        // Destroy the context with its own stack active -- single player
        // shutting down with the hint up, or mp_sync_context_count() shrinking
        // the contexts. The previous stack cannot be restored: it is about to
        // die with the context. The destructor has to fall back to the world
        // stack; otherwise the active stack dangles and the next ui_adaptor
        // asserts against a destroyed owner.
    }
    CHECK( ui_manager::get_active_stack() != nullptr );
    CHECK( ui_manager::get_active_stack() != dead );
}

TEST_CASE( "shrinking_contexts_drops_each_contexts_overmap_adaptor", "[mp]" )
{
    restore_active_state restore;
    mp_ensure_player_context();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
    g->add_player_slot().set_body();
    mp_sync_context_count();
    REQUIRE( g->contexts.size() == 2 );

    mp_activate_player( 1 );
    // The retained overmap adaptor lives in the context it was created under,
    // and its ui_adaptor registers in that context's own stack.
    ui_stack_t *const dead = &g->contexts[1].ui_stack;
    mp_active_overmap_ui() = std::make_shared<ui_adaptor>();
    REQUIRE( dead->size() == 1 );

    // Shrink with slot 1's stack active and no force_quit(): the only thing
    // that can drop the adaptor is mp_sync_context_count()'s own reset. The
    // assertion in ~ui_adaptor, or the stack-empty check in the shrink loop,
    // aborts the run if that reset is missing.
    g->remove_extra_player_slots();
    REQUIRE( ui_manager::get_active_stack() == dead );
    mp_sync_context_count();

    CHECK( g->contexts.size() == 1 );
    CHECK( ui_manager::get_active_stack() != nullptr );
    CHECK( ui_manager::get_active_stack() != dead );
}
