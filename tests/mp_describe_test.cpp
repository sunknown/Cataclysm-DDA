#include "cata_catch.h"

#if defined( MP_SERVER )

#include <cstdint>
#include <string>
#include <vector>

#include "avatar.h"
#include "catacharset.h"
#include "color.h"
#include "cursesdef.h"
#include "cursesport.h"
#include "game.h"
#include "game_test_helper.h"
#include "map.h"
#include "map_helpers.h"
#include "map_helpers_tests.h"
#include "mp/mp_cell.h"
#include "mp/mp_context.h"
#include "mp/mp_describe.h"
#include "mp/mp_protocol.h"
#include "output.h"
#include "player_helpers.h"
#include "point.h"
#include "type_id.h"

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

// Phase 4b-2: the server renders the game's own tile-info text into a scratch
// window and reads it back, so a describe answer is what the look-around panel
// would have drawn rather than a second description implementation.

namespace
{

// A cleared world with one avatar and no creatures, matching the co-op scene
// tests. The active context is restored when the test ends.
void stand_on_a_cleared_map_for_describe()
{
    mp_ensure_player_context();
    clear_avatar();
    build_test_map( ter_id( "t_pavement" ) );
    clear_creatures();
    game_test_helper::reset_player_slots();
    mp_sync_context_count();
}

} // namespace

TEST_CASE( "mp_describe_lines_renders_a_tile", "[mp]" )
{
    restore_active_state restore;
    stand_on_a_cleared_map_for_describe();
    map &here = get_map();
    avatar &you = get_avatar_legacy();
    set_time_to_day();
    here.invalidate_visibility_cache();
    here.update_visibility_cache( you.posz() );

    const tripoint_bub_ms tile = you.pos_bub();

    // A local context owns the map window and view centre; only the pointer is
    // installed, and restore_active_state puts the previous pointer back. The
    // scratch window therefore cannot leak into the shared context or later
    // cases.
    player_context ctx;
    ctx.slot_index = 0;
    mp_set_active_context( &ctx );
    ctx.w_terrain = catacurses::newwin( 10, 40, point( 10, 0 ) );
    ctx.ter_view_p = tile;

    for( const int width : { 20, 40, 80 } ) {
        CAPTURE( width );
        const std::vector<mp_log_line> lines = mp_describe_lines( tile, width );
        REQUIRE_FALSE( lines.empty() );

        std::string text;
        for( const mp_log_line &line : lines ) {
            const std::string line_text = run_text( line );
            text += line_text;
            // The window is the requested width, so no folded line can be
            // wider than it.
            CHECK( utf8_width( line_text ) <= width );
            for( const mp_log_run &run : line.runs ) {
                CHECK_FALSE( run.text.empty() );
            }
        }
        CHECK_FALSE( text.empty() );
        // The rendered tile is pinned: a function that described some other
        // tile would not produce this terrain name.
        CHECK( text.find( "Pavement" ) != std::string::npos );
    }
}

// Phase 4b-2: every hover request gets an answer, even a failed one. The
// envelope carries the request's identity and the effective width, and only
// the text differs between a described tile and a rejected cell.
TEST_CASE( "mp_describe_reply_fills_the_envelope", "[mp]" )
{
    restore_active_state restore;
    stand_on_a_cleared_map_for_describe();
    map &here = get_map();
    avatar &you = get_avatar_legacy();
    set_time_to_day();

    // The hovered cell maps to the view centre, so the avatar's own tile is
    // described and no sight test can turn the answer into an empty one.
    const tripoint_bub_ms view_centre( 20, 5, 0 );
    const tripoint_bub_ms previous_pos = you.pos_bub();
    you.setpos( here, view_centre, false );
    // The lightmap is only rebuilt when asked, so a bare move would leave the
    // new tile's light at its previous value and the tile would read as
    // unseen. build_test_map() deliberately skips the lightmap, so build it
    // here at the day time set above.
    here.invalidate_map_cache( 0 );
    here.build_map_cache( 0, false );
    here.invalidate_visibility_cache();
    here.update_visibility_cache( you.posz() );

    player_context ctx;
    ctx.slot_index = 0;
    mp_set_active_context( &ctx );
    ctx.w_terrain = catacurses::newwin( 10, 40, point( 10, 0 ) );
    ctx.ter_view_p = view_centre;

    SECTION( "a cell inside the map window describes its tile" ) {
        const mp_describe_msg msg = mp_describe_reply( 7, 42, 30, 5, 40 );
        CHECK( msg.seq == 7 );
        CHECK( msg.frame == 42 );
        CHECK( msg.x == 30 );
        CHECK( msg.y == 5 );
        CHECK( msg.w == 40 );
        REQUIRE_FALSE( msg.lines.empty() );

        std::string text;
        for( const mp_log_line &line : msg.lines ) {
            text += run_text( line );
        }
        CHECK( text.find( "Pavement" ) != std::string::npos );
    }

    SECTION( "a visible neighbouring tile is not Unseen" ) {
        // The hovered cell is three tiles east of the avatar, open pavement in
        // daylight. Looking around there shows its terrain, so a hover must
        // describe the same tile rather than fall back to "Unseen.".
        const mp_describe_msg msg = mp_describe_reply( 13, 48, 33, 5, 40 );
        REQUIRE_FALSE( msg.lines.empty() );

        std::string text;
        for( const mp_log_line &line : msg.lines ) {
            text += run_text( line );
        }
        CHECK( text.find( "Unseen" ) == std::string::npos );
        CHECK( text.find( "Pavement" ) != std::string::npos );
    }

    SECTION( "a cell outside the map window keeps the envelope" ) {
        const mp_describe_msg msg = mp_describe_reply( 8, 43, 9, 0, 40 );
        CHECK( msg.seq == 8 );
        CHECK( msg.frame == 43 );
        CHECK( msg.x == 9 );
        CHECK( msg.y == 0 );
        CHECK( msg.w == 40 );
        CHECK( msg.lines.empty() );
    }

    SECTION( "the width is clamped to the supported range" ) {
        const mp_describe_msg narrow = mp_describe_reply( 9, 44, 30, 5, 5 );
        CHECK( narrow.w == MP_DESCRIBE_MIN_WIDTH );
        CHECK_FALSE( narrow.lines.empty() );

        const mp_describe_msg wide = mp_describe_reply( 10, 45, 30, 5, 500 );
        CHECK( wide.w == MP_DESCRIBE_MAX_WIDTH );
        CHECK_FALSE( wide.lines.empty() );
    }

    SECTION( "no map window yields an empty answer" ) {
        ctx.w_terrain = catacurses::window();
        const mp_describe_msg msg = mp_describe_reply( 11, 46, 30, 5, 40 );
        CHECK( msg.seq == 11 );
        CHECK( msg.frame == 46 );
        CHECK( msg.x == 30 );
        CHECK( msg.y == 5 );
        CHECK( msg.w == 40 );
        CHECK( msg.lines.empty() );
    }

    // Other co-op tests share the world; the next case starts from the
    // position this one found rather than the one it needed to see a tile.
    you.setpos( here, previous_pos, false );
}

// Phase 4b-2: a hover can be answered before the active seat's own draw has
// rebuilt the shared visibility state. Activating a seat and the world half of
// a turn only mark it dirty; a stale cache from the previous activation made
// every tile but the avatar's own come back as "Unseen.". The answer must be
// built from the seat that asked.
TEST_CASE( "mp_describe_refreshes_stale_visibility", "[mp]" )
{
    restore_active_state restore;
    stand_on_a_cleared_map_for_describe();
    map &here = get_map();
    avatar &you = get_avatar_legacy();
    set_time_to_day();

    const tripoint_bub_ms previous_pos = you.pos_bub();
    const tripoint_bub_ms first( 20, 5, 0 );
    // Far enough that the previous seat's fog does not reach it: an open
    // pavement map lets a FOV see about 60 tiles.
    const tripoint_bub_ms seat( 100, 100, 0 );

    // Build the caches for one position, then take the second seat.
    you.setpos( here, first, false );
    here.invalidate_map_cache( 0 );
    here.build_map_cache( 0, false );
    here.invalidate_visibility_cache();
    here.update_visibility_cache( you.posz() );

    // Seat activation moves the avatar and marks the shared visibility state
    // dirty; no draw has run for the new position yet.
    you.setpos( here, seat, false );
    here.invalidate_visibility_cache();

    const visibility_variables &vc = here.get_visibility_variables_cache();
    CHECK( here.access_cache( 0 ).seen_cache[seat.x()][seat.y()] == 0.0f );
    CHECK( here.apparent_light_at( seat, vc ) == lit_level::BLANK );

    player_context ctx;
    ctx.slot_index = 0;
    mp_set_active_context( &ctx );
    ctx.w_terrain = catacurses::newwin( 10, 40, point( 10, 0 ) );
    ctx.ter_view_p = seat;

    // Cell (30, 5) maps to the view centre, i.e. the avatar's own tile at the
    // new seat. With the previous seat's fog it reads as unseen.
    const mp_describe_msg msg = mp_describe_reply( 14, 49, 30, 5, 40 );
    REQUIRE_FALSE( msg.lines.empty() );

    std::string text;
    for( const mp_log_line &line : msg.lines ) {
        text += run_text( line );
    }
    CHECK( text.find( "Unseen" ) == std::string::npos );
    CHECK( text.find( "Pavement" ) != std::string::npos );

    you.setpos( here, previous_pos, false );
}

#endif // MP_SERVER
