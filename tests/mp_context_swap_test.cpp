#if defined(MP_SERVER)

#include "cata_catch.h"

#include "cached_options.h"      // test_mode
#include "cata_imgui.h"          // cataimgui::client
#include "cata_scope_helpers.h"  // restore_on_out_of_scope
#include "cursesport.h"          // cata_cursesport::WINDOW
#include "game_test_helper.h"    // restore_active_state
#include "mp/mp_context.h"
#include "ui_manager.h"

namespace {

// Returns the window content as a line-by-line sequence of glyphs.
std::string dump_cells( const catacurses::window &win )
{
    cata_cursesport::WINDOW *w = win.get<cata_cursesport::WINDOW>();
    REQUIRE( w != nullptr );
    std::string out;
    for( const cata_cursesport::curseline &line : w->line ) {
        for( const cata_cursesport::cursecell &cell : line.chars ) {
            out += cell.ch;
        }
        out += '\n';
    }
    return out;
}

} // namespace

TEST_CASE( "context_swap_restores_each_screens_cells", "[mp]" )
{
    // The test points the active context/stack at the stack locals a/b; RAII
    // restores the previous state, otherwise the next test writes into freed
    // memory.
    restore_active_state restore;

    // The redraw path (ui_manager::redraw_invalidated) drives an ImGui frame
    // through the global imclient, which the server's catacurses::init_interface()
    // creates at startup. The test harness never runs that init, so create the
    // client for the duration of this test and restore the previous (null) one
    // on scope exit.
    restore_on_out_of_scope restore_imclient( std::move( imclient ) );
    imclient = std::make_unique<cataimgui::client>();

    // Two contexts with their own root windows.
    player_context a;
    player_context b;
    const int cols = 30;
    const int rows = 10;
    a.stdscr = catacurses::newwin( rows, cols, point::zero );
    b.stdscr = catacurses::newwin( rows, cols, point::zero );

    // Scenarios: scripted redraw callbacks paint into the active stdscr.
    const std::string scenario_a = "SCENARIO_A";
    const std::string scenario_b = "SCENARIO_B";
    const auto paint = []( const std::string &text ) {
        werase( catacurses::stdscr );
        mvwprintw( catacurses::stdscr, point( 1, 2 ), text );
        wnoutrefresh( catacurses::stdscr );
    };

    // Activate A, paint A, capture the content.
    mp_set_active_context( &a );
    ui_manager::set_active_stack( &a.ui_stack );
    catacurses::stdscr = a.stdscr;
    {
        ui_adaptor ui;
        ui.on_redraw( [&]( ui_adaptor & ) { paint( scenario_a ); } );
        ui.position( point::zero, point( cols, rows ) );
        ui.invalidate_ui();
        {
            restore_on_out_of_scope restore_test_mode( test_mode );
            test_mode = false;
            ui_manager::redraw_invalidated();
        }
    }
    const std::string cells_a = dump_cells( a.stdscr );

    // Activate B, paint B, capture.
    mp_set_active_context( &b );
    ui_manager::set_active_stack( &b.ui_stack );
    catacurses::stdscr = b.stdscr;
    {
        ui_adaptor ui;
        ui.on_redraw( [&]( ui_adaptor & ) { paint( scenario_b ); } );
        ui.position( point::zero, point( cols, rows ) );
        ui.invalidate_ui();
        {
            restore_on_out_of_scope restore_test_mode( test_mode );
            test_mode = false;
            ui_manager::redraw_invalidated();
        }
    }
    const std::string cells_b = dump_cells( b.stdscr );
    // Scenario B actually landed in window B.
    CHECK( cells_b.find( scenario_b ) != std::string::npos );
    // Window A is not polluted by scenario B.
    CHECK( cells_a.find( scenario_b ) == std::string::npos );

    // Back to A, repeat scenario A, capture A'.
    mp_set_active_context( &a );
    ui_manager::set_active_stack( &a.ui_stack );
    catacurses::stdscr = a.stdscr;
    {
        ui_adaptor ui;
        ui.on_redraw( [&]( ui_adaptor & ) { paint( scenario_a ); } );
        ui.position( point::zero, point( cols, rows ) );
        ui.invalidate_ui();
        {
            restore_on_out_of_scope restore_test_mode( test_mode );
            test_mode = false;
            ui_manager::redraw_invalidated();
        }
    }
    const std::string cells_a2 = dump_cells( a.stdscr );

    // Key assertion: A == A' by cell content.
    CHECK( cells_a == cells_a2 );
}
#endif // defined(MP_SERVER)
