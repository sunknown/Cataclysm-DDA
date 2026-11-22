#include "cata_catch.h"

#if defined( MP_SERVER )

#include <cstdint>
#include <string>

#include "color.h"
#include "cursesdef.h"
#include "cursesport.h"
#include "output.h"
#include "point.h"

// The hover-describe feed and the screen blit both read window cells through
// catacurses::get_cell, so the accessor has to report exactly what the window
// holds: the raw glyph and the palette bytes, with out-of-range requests
// rejected instead of silently returning a stale cell.

TEST_CASE( "mp_window_get_cell", "[mp]" )
{
    // Two lines of five columns, matching the blit's own window geometry.
    catacurses::window win = catacurses::newwin( 2, 5, point::zero );
    REQUIRE( win );

    SECTION( "a printed glyph carries its colour bytes" ) {
        mvwprintz( win, point( 0, 0 ), c_red, "x" );
        catacurses::window_cell cell;
        REQUIRE( catacurses::get_cell( win, point( 0, 0 ), cell ) );
        CHECK( cell.ch == "x" );
        CHECK( cell.fg == nc_color_foreground_index( c_red ) );
        CHECK( cell.bg == static_cast<uint8_t>(
                   cata_cursesport::colorpairs[c_red.to_color_pair_index()].BG ) );
    }

    SECTION( "a non-default background is copied, not masked off" ) {
        mvwprintz( win, point( 1, 1 ), c_white_red, "y" );
        catacurses::window_cell cell;
        REQUIRE( catacurses::get_cell( win, point( 1, 1 ), cell ) );
        CHECK( cell.ch == "y" );
        CHECK( cell.fg == nc_color_foreground_index( c_white_red ) );
        CHECK( cell.bg == static_cast<uint8_t>(
                   cata_cursesport::colorpairs[c_white_red.to_color_pair_index()].BG ) );
        CHECK( cell.bg != 0 );
    }

    SECTION( "a wide glyph leaves the continuation cell empty" ) {
        mvwprintz( win, point( 0, 0 ), c_white, u8"\u65e5" );
        catacurses::window_cell first;
        catacurses::window_cell second;
        REQUIRE( catacurses::get_cell( win, point( 0, 0 ), first ) );
        REQUIRE( catacurses::get_cell( win, point( 1, 0 ), second ) );
        CHECK( first.ch == u8"\u65e5" );
        // The continuation column is reported as the window stores it; the
        // caller decides how to render it.
        CHECK( second.ch.empty() );
    }

    SECTION( "a point outside the window is rejected and leaves the cell untouched" ) {
        catacurses::window_cell cell;
        cell.ch = "keep";
        cell.fg = 7;
        cell.bg = 9;
        CHECK_FALSE( catacurses::get_cell( win, point( 5, 0 ), cell ) );
        CHECK_FALSE( catacurses::get_cell( win, point( -1, 0 ), cell ) );
        CHECK( cell.ch == "keep" );
        CHECK( cell.fg == 7 );
        CHECK( cell.bg == 9 );
    }

    SECTION( "a null window is rejected and leaves the cell untouched" ) {
        catacurses::window_cell cell;
        cell.ch = "keep";
        cell.fg = 7;
        cell.bg = 9;
        CHECK_FALSE( catacurses::get_cell( catacurses::window(), point( 0, 0 ), cell ) );
        CHECK( cell.ch == "keep" );
        CHECK( cell.fg == 7 );
        CHECK( cell.bg == 9 );
    }
}

#endif // MP_SERVER
