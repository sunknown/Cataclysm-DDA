#if defined(MP_SERVER)

#include "cata_catch.h"

#include <string>

#include "cursesdef.h"
#include "mp/mp_cell.h"
#include "mp/mp_server_screen.h"
#include "output.h"
#include "point.h"

TEST_CASE( "drawing a curses window reaches the server grid", "[mp]" )
{
    mp_server_reset_for_test();
    mp_server_set_terminal_size( 40, 10 );

    catacurses::window win = catacurses::newwin( 3, 12, point( 2, 1 ) );
    catacurses::mvwprintw( win, point( 0, 0 ), "hello" );
    catacurses::wrefresh( win );

    CHECK( mp_server_grid().at( 2, 1 ).ch == "h" );
    CHECK( mp_server_grid().at( 6, 1 ).ch == "o" );
}

TEST_CASE( "a window redraw with no change dirties nothing", "[mp]" )
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

TEST_CASE( "terminal size comes from the grid", "[mp]" )
{
    mp_server_reset_for_test();
    mp_server_set_terminal_size( 96, 30 );

    CHECK( get_terminal_width() == 96 );
    CHECK( get_terminal_height() == 30 );
}

#endif // MP_SERVER
