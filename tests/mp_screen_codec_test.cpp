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

// True when every cell of `g` still holds its default value -- used to check
// that a rejected mp_screen_apply() call left the target untouched.
static bool all_default( const mp_screen_grid &g )
{
    for( int y = 0; y < g.height(); ++y ) {
        for( int x = 0; x < g.width(); ++x ) {
            if( !( g.at( x, y ) == mp_cell() ) ) {
                return false;
            }
        }
    }
    return true;
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

TEST_CASE( "mp_screen codec pins the wire format exactly", "[mp]" )
{
    // Every other test round trips through both the encoder and the decoder
    // together, so a matched drift in both -- swapped fg/bg, run_count and
    // row_index swapped, a flipped byte order -- would pass unnoticed. This
    // nails the encoder's output to a byte-for-byte layout instead.
    //
    // 3x2 grid, row 1 dirty: (0,1)="A" fg1/bg2, (1,1)="A" fg1/bg2 (same run),
    // (2,1)="" fg3/bg4 (a second run, a continuation cell).
    mp_screen_grid grid( 3, 2 );
    grid.clear_dirty();
    grid.set( 0, 1, cell( "A", 1, 2 ) );
    grid.set( 1, 1, cell( "A", 1, 2 ) );
    grid.set( 2, 1, cell( "", 3, 4 ) );

    const std::vector<uint8_t> expected = {
        0x00, 0x03,                         // width = 3
        0x00, 0x02,                         // height = 2
        0x00, 0x01,                         // row_count = 1
        0x00, 0x01,                         // row_index = 1
        0x00, 0x02,                         // run_count = 2
        0x00, 0x02, 0x01, 0x02, 0x01, 0x41, // repeat=2 fg=1 bg=2 len=1 'A'
        0x00, 0x01, 0x03, 0x04, 0x00        // repeat=1 fg=3 bg=4 len=0
    };
    CHECK( mp_screen_encode( grid ) == expected );
}

TEST_CASE( "mp_screen_apply decodes a hand-built payload matching the pinned format", "[mp]" )
{
    // The mirror image of the previous test: a payload built by hand (not
    // via mp_screen_encode) must decode exactly as the format comment says.
    const std::vector<uint8_t> payload = {
        0x00, 0x03, 0x00, 0x02, 0x00, 0x01,
        0x00, 0x01, 0x00, 0x02,
        0x00, 0x02, 0x01, 0x02, 0x01, 0x41,
        0x00, 0x01, 0x03, 0x04, 0x00
    };

    mp_screen_grid target( 3, 2 );
    REQUIRE( mp_screen_apply( target, payload ) );
    CHECK( target.at( 0, 1 ) == cell( "A", 1, 2 ) );
    CHECK( target.at( 1, 1 ) == cell( "A", 1, 2 ) );
    CHECK( target.at( 2, 1 ) == cell( "", 3, 4 ) );
    // Row 0 was never mentioned by the payload and must stay untouched.
    CHECK( target.at( 0, 0 ) == mp_cell() );
    CHECK( target.at( 1, 0 ) == mp_cell() );
    CHECK( target.at( 2, 0 ) == mp_cell() );
}

TEST_CASE( "mp_screen codec round trips a double-width continuation cell", "[mp]" )
{
    // glyph_len == 0 (the empty glyph, marking a continuation cell) is the
    // one length the wire format calls out specially, but the general round
    // trip test above only ever uses "#" and "e"-with-accent. Cover it here.
    mp_screen_grid source( 4, 1 );
    source.set( 0, 0, cell( "A", 7, 0 ) );
    source.set( 1, 0, cell( "", 7, 0 ) );
    source.set( 2, 0, cell( "#", 3, 1 ) );
    source.set( 3, 0, cell( "", 3, 1 ) );

    mp_screen_grid target( 4, 1 );
    REQUIRE( mp_screen_apply( target, mp_screen_encode( source ) ) );
    for( int x = 0; x < source.width(); ++x ) {
        CHECK( target.at( x, 0 ) == source.at( x, 0 ) );
    }
}

TEST_CASE( "mp_screen_apply rejects a run with a non-positive repeat", "[mp]" )
{
    const std::vector<uint8_t> payload = {
        0x00, 0x03, 0x00, 0x02, 0x00, 0x01, // 3x2, one dirty row
        0x00, 0x00, 0x00, 0x01,             // row 0, one run
        0x00, 0x00, 0x07, 0x00, 0x01, 0x78  // repeat=0 fg=7 bg=0 len=1 'x'
    };
    mp_screen_grid target( 3, 2 );
    CHECK_FALSE( mp_screen_apply( target, payload ) );
    CHECK( all_default( target ) );
}

TEST_CASE( "mp_screen_apply rejects a run that overflows the row width", "[mp]" )
{
    const std::vector<uint8_t> payload = {
        0x00, 0x03, 0x00, 0x02, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x05, 0x07, 0x00, 0x01, 0x78  // repeat=5 on a width-3 row
    };
    mp_screen_grid target( 3, 2 );
    CHECK_FALSE( mp_screen_apply( target, payload ) );
    CHECK( all_default( target ) );
}

TEST_CASE( "mp_screen_apply rejects a row whose runs fall short of the row width", "[mp]" )
{
    const std::vector<uint8_t> payload = {
        0x00, 0x03, 0x00, 0x02, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x02, 0x07, 0x00, 0x01, 0x78  // repeat=2, leaves x=2 != width=3
    };
    mp_screen_grid target( 3, 2 );
    CHECK_FALSE( mp_screen_apply( target, payload ) );
    CHECK( all_default( target ) );
}

TEST_CASE( "mp_screen_apply rejects an out-of-range row index", "[mp]" )
{
    const std::vector<uint8_t> payload = {
        0x00, 0x03, 0x00, 0x02, 0x00, 0x01,
        0x00, 0x05, 0x00, 0x00  // row_index=5 on a height-2 grid
    };
    mp_screen_grid target( 3, 2 );
    CHECK_FALSE( mp_screen_apply( target, payload ) );
    CHECK( all_default( target ) );
}

TEST_CASE( "mp_screen_apply rejects a duplicate row index", "[mp]" )
{
    // A legitimate encoder never emits the same row twice; this is the
    // amplification vector the decoder must not tolerate (see the
    // row_count bound test below for the sibling case).
    const std::vector<uint8_t> payload = {
        0x00, 0x03, 0x00, 0x02, 0x00, 0x02, // 3x2, claims two dirty rows
        0x00, 0x00, 0x00, 0x01, 0x00, 0x03, 0x07, 0x00, 0x01, 0x78, // row 0
        0x00, 0x00, 0x00, 0x01, 0x00, 0x03, 0x07, 0x00, 0x01, 0x79  // row 0 again
    };
    mp_screen_grid target( 3, 2 );
    CHECK_FALSE( mp_screen_apply( target, payload ) );
    CHECK( all_default( target ) );
}

TEST_CASE( "mp_screen_apply rejects a row_count larger than the grid height", "[mp]" )
{
    // A legitimate encoder's row_count is bounded by dirty_rows(), which can
    // never exceed height. Without this check a decoder that buffers writes
    // can be walked into materialising width * row_count entries from a
    // payload of only a few hundred kilobytes -- hundreds of megabytes on a
    // wide terminal. This grid never even reaches the row data.
    const std::vector<uint8_t> payload = {
        0x00, 0x03, 0x00, 0x02, 0x00, 0x03  // row_count=3 on a height-2 grid
    };
    mp_screen_grid target( 3, 2 );
    CHECK_FALSE( mp_screen_apply( target, payload ) );
    CHECK( all_default( target ) );
}

TEST_CASE( "mp_screen_apply rejects a glyph_len that overruns the payload", "[mp]" )
{
    const std::vector<uint8_t> payload = {
        0x00, 0x03, 0x00, 0x02, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x01, 0x07, 0x00, 0x04, 0x78 // glyph_len=4 but only 1 byte follows
    };
    mp_screen_grid target( 3, 2 );
    CHECK_FALSE( mp_screen_apply( target, payload ) );
    CHECK( all_default( target ) );
}

TEST_CASE( "mp_screen_apply rejects a glyph longer than 4 bytes", "[mp]" )
{
    // No single UTF-8 code point encodes to more than 4 bytes; a longer
    // glyph can only be a hostile or corrupt payload trying to smuggle
    // arbitrary bytes into a cell that a later client hands to its renderer.
    const std::vector<uint8_t> payload = {
        0x00, 0x03, 0x00, 0x02, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x01, 0x07, 0x00, 0x05, 0x41, 0x41, 0x41, 0x41, 0x41 // len=5
    };
    mp_screen_grid target( 3, 2 );
    CHECK_FALSE( mp_screen_apply( target, payload ) );
    CHECK( all_default( target ) );
}

TEST_CASE( "mp_screen_apply rejects a malformed UTF-8 lead byte", "[mp]" )
{
    // 0x80 is a bare continuation byte (10xxxxxx); it can never legally
    // start a UTF-8 sequence.
    const std::vector<uint8_t> payload = {
        0x00, 0x03, 0x00, 0x02, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x01, 0x07, 0x00, 0x01, 0x80 // len=1, byte=0x80
    };
    mp_screen_grid target( 3, 2 );
    CHECK_FALSE( mp_screen_apply( target, payload ) );
    CHECK( all_default( target ) );
}
#endif // MULTIPLAYER
