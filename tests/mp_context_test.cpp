#include "cata_catch.h"

#include <memory>

#include "cata_scope_helpers.h"
#include "game_test_helper.h"
#include "mp/mp_context.h"
#include "output.h"
#include "panels.h"
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

    mp_activate_player( 1 );
    CHECK( get_avatar().name == "SlotOne" );
    get_avatar().name = "SlotOneEdited";

    mp_activate_player( 2 );
    CHECK( get_avatar().name == "SlotTwo" );

    // Back to slot 1: its content is preserved (an unpaired swap would break
    // this).
    mp_activate_player( 1 );
    CHECK( get_avatar().name == "SlotOneEdited" );

    mp_activate_player( 0 );
    CHECK( get_avatar().name == "SlotZero" );
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
