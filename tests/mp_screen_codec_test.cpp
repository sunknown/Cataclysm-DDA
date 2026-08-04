#include "cata_catch.h"

#if defined(MULTIPLAYER)
#include <cstdint>
#include <string>
#include <vector>

#include "mp/mp_cell.h"
#include "mp/mp_screen_codec.h"

static mp_cell cell( const std::string &ch, const uint8_t fg = 7, const uint8_t bg = 0 )
{
    mp_cell c;
    c.ch = ch;
    c.fg = fg;
    c.bg = bg;
    return c;
}

TEST_CASE( "mp_screen_grid starts fully dirty", "[mp]" )
{
    // A freshly created grid has never been sent, so every row must go out.
    mp_screen_grid grid( 4, 3 );
    CHECK( grid.dirty_rows() == std::vector<int> { 0, 1, 2 } );
}

TEST_CASE( "mp_screen_grid marks only changed rows dirty", "[mp]" )
{
    mp_screen_grid grid( 4, 3 );
    grid.clear_dirty();
    REQUIRE_FALSE( grid.any_dirty() );

    grid.set( 2, 1, cell( "x" ) );
    CHECK( grid.dirty_rows() == std::vector<int> { 1 } );
}

TEST_CASE( "mp_screen_grid ignores a write that changes nothing", "[mp]" )
{
    // Redrawing the same content is extremely common in this game; if it
    // dirtied rows we would resend the whole screen on every keystroke.
    mp_screen_grid grid( 4, 3 );
    grid.set( 2, 1, cell( "x" ) );
    grid.clear_dirty();

    grid.set( 2, 1, cell( "x" ) );
    CHECK_FALSE( grid.any_dirty() );
}

TEST_CASE( "mp_screen codec transfers dirty rows only", "[mp]" )
{
    mp_screen_grid source( 8, 4 );
    mp_screen_grid target( 8, 4 );

    // Full initial sync.
    const std::vector<uint8_t> first = mp_screen_encode( source );
    REQUIRE( mp_screen_apply( target, first ) );
    source.clear_dirty();

    source.set( 3, 2, cell( "@", 3, 1 ) );
    const std::vector<uint8_t> delta = mp_screen_encode( source );
    source.clear_dirty();

    REQUIRE( mp_screen_apply( target, delta ) );
    CHECK( target.at( 3, 2 ) == cell( "@", 3, 1 ) );
    CHECK( target.at( 0, 0 ) == mp_cell() );
}

TEST_CASE( "mp_screen codec round trips a full screen of varied content", "[mp]" )
{
    mp_screen_grid source( 20, 6 );
    for( int y = 0; y < source.height(); ++y ) {
        for( int x = 0; x < source.width(); ++x ) {
            const uint8_t fg = static_cast<uint8_t>( ( x * 7 + y ) % 256 );
            const uint8_t bg = static_cast<uint8_t>( ( x + y * 3 ) % 16 );
            source.set( x, y, cell( x % 5 == 0 ? "é" : "#", fg, bg ) );
        }
    }

    mp_screen_grid target( 20, 6 );
    REQUIRE( mp_screen_apply( target, mp_screen_encode( source ) ) );

    for( int y = 0; y < source.height(); ++y ) {
        for( int x = 0; x < source.width(); ++x ) {
            REQUIRE( target.at( x, y ) == source.at( x, y ) );
        }
    }
}

TEST_CASE( "mp_screen codec compresses runs of identical cells", "[mp]" )
{
    // A blank 80x24 screen is 1920 identical cells; run-length encoding must
    // turn that into a handful of bytes, not two kilobytes.
    mp_screen_grid blank( 80, 24 );
    const std::vector<uint8_t> payload = mp_screen_encode( blank );
    CHECK( payload.size() < 400 );
}

TEST_CASE( "mp_screen_apply rejects a payload for a different screen size", "[mp]" )
{
    mp_screen_grid source( 8, 4 );
    mp_screen_grid target( 10, 4 );
    CHECK_FALSE( mp_screen_apply( target, mp_screen_encode( source ) ) );
}

TEST_CASE( "mp_screen_apply rejects a truncated payload", "[mp]" )
{
    mp_screen_grid source( 8, 4 );
    std::vector<uint8_t> payload = mp_screen_encode( source );
    payload.resize( payload.size() / 2 );

    mp_screen_grid target( 8, 4 );
    CHECK_FALSE( mp_screen_apply( target, payload ) );
}
#endif // MULTIPLAYER
