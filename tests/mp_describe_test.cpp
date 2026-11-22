#include "cata_catch.h"

#if defined( MP_SERVER )

#include <cstdint>
#include <string>
#include <vector>

#include "color.h"
#include "cursesdef.h"
#include "cursesport.h"
#include "mp/mp_cell.h"
#include "mp/mp_describe.h"
#include "mp/mp_protocol.h"
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

// Phase 4b-2: the pure text rules of the hover-describe feed. A rendered
// tile-info grid becomes the lines the client draws, so these tests pin the
// trimming, the run split, and every cap without a map or a window.

namespace
{

// Builds a blank grid to write glyphs into; the cells start as spaces in the
// default foreground, the state a freshly cleared window row has.
std::vector<mp_cell> blank_grid( const int width, const int height )
{
    return std::vector<mp_cell>( static_cast<size_t>( width ) *
                                 static_cast<size_t>( height ) );
}

mp_cell glyph( const std::string &ch, const uint8_t fg = 7, const uint8_t bg = 0 )
{
    mp_cell cell;
    cell.ch = ch;
    cell.fg = fg;
    cell.bg = bg;
    return cell;
}

// Writes an ASCII line from column 0; the rest of the row stays blank.
void put_line( std::vector<mp_cell> &cells, const int width, const int y,
               const std::string &text, const uint8_t fg = 7 )
{
    for( size_t x = 0; x < text.size(); ++x ) {
        const size_t at = static_cast<size_t>( y ) * static_cast<size_t>( width ) + x;
        cells[at] = glyph( std::string( 1, text[x] ), fg );
    }
}

std::string run_text( const mp_log_line &line )
{
    std::string text;
    for( const mp_log_run &run : line.runs ) {
        text += run.text;
    }
    return text;
}

} // namespace

TEST_CASE( "mp_describe_extract_cleans_cells", "[mp]" )
{
    SECTION( "trailing blanks go and the shared indent is dropped" ) {
        std::vector<mp_cell> cells = blank_grid( 8, 2 );
        put_line( cells, 8, 0, " a b" );
        put_line( cells, 8, 1, "  b" );

        const std::vector<mp_log_line> lines = mp_describe_extract( cells, 8, 2 );
        REQUIRE( lines.size() == 2 );
        REQUIRE( lines[0].runs.size() == 1 );
        CHECK( lines[0].runs[0].text == "a b" );
        CHECK( lines[0].runs[0].color == 7 );
        REQUIRE( lines[1].runs.size() == 1 );
        // The extra space of the nested indent is text, not layout.
        CHECK( lines[1].runs[0].text == " b" );
    }

    SECTION( "edge blank rows drop and an inner one stays as an empty line" ) {
        std::vector<mp_cell> cells = blank_grid( 4, 5 );
        put_line( cells, 4, 1, "x" );
        put_line( cells, 4, 3, "y" );

        const std::vector<mp_log_line> lines = mp_describe_extract( cells, 4, 5 );
        REQUIRE( lines.size() == 3 );
        CHECK( run_text( lines[0] ) == "x" );
        CHECK( lines[1].runs.empty() );
        CHECK( run_text( lines[2] ) == "y" );
    }

    SECTION( "a run ends only where the foreground changes" ) {
        std::vector<mp_cell> cells = blank_grid( 4, 1 );
        cells[0] = glyph( "a", 7, 0 );
        cells[1] = glyph( "b", 7, 5 );
        cells[2] = glyph( "c", 2, 0 );
        cells[3] = glyph( "d", 2, 9 );

        const std::vector<mp_log_line> lines = mp_describe_extract( cells, 4, 1 );
        REQUIRE( lines.size() == 1 );
        // A background change alone never splits a run.
        REQUIRE( lines[0].runs.size() == 2 );
        CHECK( lines[0].runs[0].text == "ab" );
        CHECK( lines[0].runs[0].color == 7 );
        CHECK( lines[0].runs[1].text == "cd" );
        CHECK( lines[0].runs[1].color == 2 );
    }

    SECTION( "30 input rows stop at 24 lines" ) {
        std::vector<mp_cell> cells = blank_grid( 1, 30 );
        for( int y = 0; y < 30; ++y ) {
            put_line( cells, 1, y, "x" );
        }

        const std::vector<mp_log_line> lines = mp_describe_extract( cells, 1, 30 );
        REQUIRE( lines.size() == 24 );
        CHECK( run_text( lines.back() ) == "x" );
    }

    SECTION( "a 300-run line stops at 256 runs" ) {
        const int width = 300;
        std::vector<mp_cell> cells = blank_grid( width, 1 );
        for( int x = 0; x < width; ++x ) {
            cells[x] = glyph( "x", static_cast<uint8_t>( x % 2 == 0 ? 1 : 2 ) );
        }

        const std::vector<mp_log_line> lines = mp_describe_extract( cells, width, 1 );
        REQUIRE( lines.size() == 1 );
        REQUIRE( lines[0].runs.size() == 256 );
        CHECK( lines[0].runs.front().text == "x" );
        CHECK( lines[0].runs.front().color == 1 );
        CHECK( lines[0].runs.back().text == "x" );
        CHECK( lines[0].runs.back().color == 2 );
    }

    SECTION( "16 KiB + 1 byte of text is cut at a UTF-8 boundary" ) {
        const std::vector<mp_cell> ascii(
            MP_DESCRIBE_MAX_TEXT_BYTES + 1, glyph( "a" ) );
        const std::vector<mp_log_line> ascii_lines = mp_describe_extract(
                    ascii, static_cast<int>( ascii.size() ), 1 );
        REQUIRE( ascii_lines.size() == 1 );
        REQUIRE( ascii_lines[0].runs.size() == 1 );
        CHECK( ascii_lines[0].runs[0].text.size() == MP_DESCRIBE_MAX_TEXT_BYTES );

        // 8191 two-byte glyphs plus one three-byte glyph are 16 KiB + 1 byte.
        // The room left is two bytes, so the last glyph goes whole: the answer
        // never carries a partial sequence.
        std::vector<mp_cell> cells = blank_grid( 8192, 1 );
        for( int x = 0; x < 8191; ++x ) {
            cells[x] = glyph( u8"\u00e9" );
        }
        cells[8191] = glyph( u8"\u65e5" );

        const std::vector<mp_log_line> lines = mp_describe_extract( cells, 8192, 1 );
        REQUIRE( lines.size() == 1 );
        REQUIRE( lines[0].runs.size() == 1 );
        CHECK( lines[0].runs[0].text.size() == 2 * 8191 );
        CHECK( lines[0].runs[0].text.size() <= MP_DESCRIBE_MAX_TEXT_BYTES );
        CHECK( lines[0].runs[0].text.substr( 2 * 8191 - 2 ) == u8"\u00e9" );
    }

    SECTION( "a wide glyph leaves no character for its continuation cell" ) {
        std::vector<mp_cell> cells = blank_grid( 4, 1 );
        cells[0] = glyph( u8"\u65e5" );
        cells[1].ch.clear();
        cells[2] = glyph( "x" );

        const std::vector<mp_log_line> lines = mp_describe_extract( cells, 4, 1 );
        REQUIRE( lines.size() == 1 );
        REQUIRE( lines[0].runs.size() == 1 );
        // The grid is shifted one column for the rest of the row; the known
        // limit is not papered over with an invented space.
        CHECK( lines[0].runs[0].text == u8"\u65e5x" );
        CHECK( lines[0].runs[0].text.size() == 4 );
    }

    SECTION( "a grid with no text yields no lines" ) {
        CHECK( mp_describe_extract( {}, 5, 5 ).empty() );
        CHECK( mp_describe_extract( blank_grid( 5, 3 ), 5, 3 ).empty() );
    }
}

#endif // MP_SERVER
