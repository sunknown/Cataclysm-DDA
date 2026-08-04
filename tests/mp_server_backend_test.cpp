#if defined(MP_SERVER)

#include "cata_catch.h"

#include <string>

#include "cursesdef.h"
#include "mp/mp_cell.h"
#include "mp/mp_imgui_server.h"
#include "mp/mp_server_screen.h"
#include "output.h"
#include "point.h"

TEST_CASE( "drawing_a_curses_window_reaches_the_server_grid", "[mp]" )
{
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
    mp_server_reset_for_test();
    mp_server_set_terminal_size( 96, 30 );

    CHECK( get_terminal_width() == 96 );
    CHECK( get_terminal_height() == 30 );
}

TEST_CASE( "imgui_frames_reach_the_server_screen_grid", "[mp]" )
{
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
    mp_server_reset_for_test();
    mp_server_set_terminal_size( 20, 5 );

    mp_test_fill_imtui_screen( 40, 10, 'Q', 7, 0 );
    mp_imgui_draw_screen();

    CHECK( mp_server_grid().at( 19, 4 ).ch == "Q" );
    CHECK( mp_server_grid().width() == 20 );
    CHECK( mp_server_grid().height() == 5 );
}

#endif // MP_SERVER
