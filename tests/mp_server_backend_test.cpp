#if defined(MP_SERVER)

#include "cata_catch.h"

#include <string>

#include "cursesdef.h"
#include "mp/mp_cell.h"
#include "mp/mp_context.h"
#include "mp/mp_imgui_server.h"
#include "mp/mp_server_main.h"
#include "mp/mp_server_screen.h"
#include "output.h"
#include "point.h"

// mp_server_reset_for_test() resets the screen module, which also resets the
// input module, and that now clears the active player context's input_queue.
// Every test must set one before the reset. Static, not a stack local, so the
// active pointer does not dangle between tests.
static player_context test_input_ctx;

// The real mp_server_request_exit() is defined in src/main.cpp, which is not
// part of the test binary (Makefile filters main.o out of the archive). The mp
// curses backend calls it when the only peer disconnects so the game stops
// spinning in its input loops; the tests never reach that input path, so a
// no-op here satisfies the archive's reference without terminating the test
// process.
void mp_server_request_exit()
{
}

TEST_CASE( "drawing_a_curses_window_reaches_the_server_grid", "[mp]" )
{
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_server_set_terminal_size( 40, 10 );

    catacurses::window win = catacurses::newwin( 3, 12, point( 2, 1 ) );
    catacurses::mvwprintw( win, point( 0, 0 ), "hello" );
    catacurses::wrefresh( win );

    CHECK( mp_server_grid().at( 2, 1 ).ch == "h" );
    CHECK( mp_server_grid().at( 6, 1 ).ch == "o" );
}

TEST_CASE( "a_window_redraw_with_no_change_dirties_nothing", "[mp]" )
{
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_server_set_terminal_size( 40, 10 );

    catacurses::window win = catacurses::newwin( 1, 12, point( 0, 0 ) );
    catacurses::mvwprintw( win, point( 0, 0 ), "steady" );
    catacurses::wrefresh( win );
    mp_server_grid().clear_dirty();

    catacurses::mvwprintw( win, point( 0, 0 ), "steady" );
    catacurses::wrefresh( win );

    CHECK_FALSE( mp_server_grid().any_dirty() );
}

TEST_CASE( "terminal_size_comes_from_the_grid", "[mp]" )
{
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_server_set_terminal_size( 96, 30 );

    CHECK( get_terminal_width() == 96 );
    CHECK( get_terminal_height() == 30 );
}

TEST_CASE( "imgui_frames_reach_the_server_screen_grid", "[mp]" )
{
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_server_set_terminal_size( 40, 10 );

    mp_test_fill_imtui_screen( 40, 10, 'Z', 3, 1 );
    mp_imgui_draw_screen();

    CHECK( mp_server_grid().at( 0, 0 ).ch == "Z" );
    CHECK( mp_server_grid().at( 39, 9 ).ch == "Z" );
    CHECK( mp_server_grid().at( 5, 5 ).fg == 3 );
    CHECK( mp_server_grid().at( 5, 5 ).bg == 1 );
}

TEST_CASE( "an_imgui_frame_larger_than_the_terminal_is_clipped_not_overrun", "[mp]" )
{
    mp_set_active_context( &test_input_ctx );
    test_input_ctx.input_queue.clear();
    mp_server_reset_for_test();
    mp_server_set_terminal_size( 20, 5 );

    mp_test_fill_imtui_screen( 40, 10, 'Q', 7, 0 );
    mp_imgui_draw_screen();

    CHECK( mp_server_grid().at( 19, 4 ).ch == "Q" );
    CHECK( mp_server_grid().width() == 20 );
    CHECK( mp_server_grid().height() == 5 );
}

#endif // MP_SERVER
