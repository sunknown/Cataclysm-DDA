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

// Builds a payload wrapping one glyph as the sole cell of a 1x1 grid, for
// testing decoder rejection of a specific glyph byte sequence in isolation:
// on a 1x1 grid a single repeat=1 run always totals the row width, so
// nothing about the row shape can mask whatever the glyph check itself
// decides.
static std::vector<uint8_t> single_glyph_payload( const std::vector<uint8_t> &glyph )
{
    std::vector<uint8_t> payload = {
        0x00, 0x01, 0x00, 0x01, 0x00, 0x01, // 1x1 grid, one dirty row
        0x00, 0x00, 0x00, 0x01,             // row 0, one run
        0x00, 0x01, 0x07, 0x00,             // repeat=1 fg=7 bg=0
        static_cast<uint8_t>( glyph.size() )
    };
    payload.insert( payload.end(), glyph.begin(), glyph.end() );
    return payload;
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
    // Two runs, repeat=0 then repeat=3, so the row still totals width=3 if
    // the repeat<=0 check were the only thing standing in the way: a single
    // repeat=0 run would leave x=0 != width and get caught by the row-width
    // check regardless, which would not prove this check does anything.
    // With both runs present, deleting the repeat<=0 check lets repeat=0
    // contribute nothing and repeat=3 complete the row on its own, so only
    // the repeat<=0 check itself stands between this payload and acceptance.
    const std::vector<uint8_t> payload = {
        0x00, 0x03, 0x00, 0x02, 0x00, 0x01, // 3x2, one dirty row
        0x00, 0x00, 0x00, 0x02,             // row 0, two runs
        0x00, 0x00, 0x07, 0x00, 0x01, 0x78, // repeat=0 fg=7 bg=0 len=1 'x'
        0x00, 0x03, 0x07, 0x00, 0x01, 0x79  // repeat=3 fg=7 bg=0 len=1 'y'
    };
    mp_screen_grid target( 3, 2 );
    CHECK_FALSE( mp_screen_apply( target, payload ) );
    CHECK( all_default( target ) );
}

// The `x + repeat > width` half of the repeat/width check
// (mp_screen_codec.cpp) has no test of its own: any payload that overshoots
// the row also leaves `x != width` at the end of it, so the row-width check
// below rejects it regardless of whether the overshoot check itself ran.
// Deleting the overshoot check does not change this test's outcome, and
// that is by design -- see the comment beside the check in the source.
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
    // Row 5 gets a complete body (run_count=1, repeat=3 filling the width-3
    // row) rather than an empty one: with an empty body, a payload with the
    // y >= height check deleted would still fail on x != width (0 != 3), so
    // that would not prove this check does anything. With a complete body,
    // deleting y >= height indexes seen_row[5] on a height-2 (size 2)
    // std::vector<bool> -- undefined behaviour, not just a wrong answer --
    // so this check is load-bearing for memory safety, not merely
    // correctness, and was previously unpinned.
    const std::vector<uint8_t> payload = {
        0x00, 0x03, 0x00, 0x02, 0x00, 0x01,
        0x00, 0x05, 0x00, 0x01,             // row_index=5 on a height-2 grid
        0x00, 0x03, 0x07, 0x00, 0x01, 0x78  // repeat=3 fg=7 bg=0 len=1 'x'
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

// row_count > height (mp_screen_apply) has no test discriminating it from
// the checks below it. By pigeonhole, a row_count greater than height
// forces at least one row_index to repeat or land outside [0, height) among
// the rows that follow -- both already rejected by walk_rows() -- so this
// check is an early exit, not an independent guard. Deleting it does not
// change this test's outcome, and that is by design -- see the comment
// beside the check in the source.
TEST_CASE( "mp_screen_apply rejects a row_count larger than the grid height", "[mp]" )
{
    const std::vector<uint8_t> payload = {
        0x00, 0x03, 0x00, 0x02, 0x00, 0x03  // row_count=3 on a height-2 grid
    };
    mp_screen_grid target( 3, 2 );
    CHECK_FALSE( mp_screen_apply( target, payload ) );
    CHECK( all_default( target ) );
}

TEST_CASE( "mp_screen_apply rejects a glyph_len that overruns the payload", "[mp]" )
{
    // On a 1x1 grid, repeat=1 always totals the row width -- see
    // single_glyph_payload()'s comment -- so what rejects this payload is
    // solely the mismatch between the declared glyph_len (4) and the single
    // byte actually supplied, not any row-shape check.
    std::vector<uint8_t> payload = {
        0x00, 0x01, 0x00, 0x01, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x01,
        0x00, 0x01, 0x07, 0x00, 0x04, 0x78 // glyph_len=4 but only 1 byte follows
    };
    mp_screen_grid target( 1, 1 );
    CHECK_FALSE( mp_screen_apply( target, payload ) );
    CHECK( all_default( target ) );
}

TEST_CASE( "mp_screen_apply rejects a glyph longer than the byte cap", "[mp]" )
{
    // glyph_max_bytes (mp_screen_codec.cpp) is 32; 33 well-formed ASCII
    // bytes are supplied in full, so what rejects this is solely the length
    // cap -- not a truncated read, which is covered separately above.
    std::vector<uint8_t> glyph( 33, 0x41 ); // 33 'A' bytes -- one over the cap
    mp_screen_grid target( 1, 1 );
    CHECK_FALSE( mp_screen_apply( target, single_glyph_payload( glyph ) ) );
    CHECK( all_default( target ) );
}

TEST_CASE( "mp_screen_apply rejects a malformed UTF-8 lead byte", "[mp]" )
{
    // 0x80 is a bare continuation byte (10xxxxxx); it can never legally
    // start a UTF-8 sequence.
    const std::vector<uint8_t> glyph = { 0x80 };
    mp_screen_grid target( 1, 1 );
    CHECK_FALSE( mp_screen_apply( target, single_glyph_payload( glyph ) ) );
    CHECK( all_default( target ) );
}

TEST_CASE( "mp_screen_apply rejects UTF-8 that a lead-byte-length check alone would miss", "[mp]" )
{
    // A check that only matches the leading 1-bits of the first byte (e.g.
    // "starts with 1110" means "3 bytes follow") accepts far more than
    // valid UTF-8: it misses overlong encodings, UTF-16 surrogates smuggled
    // in as a 3-byte sequence, and 4-byte sequences past U+10FFFF. Each
    // glyph below has a lead byte and byte count that look plausible but a
    // continuation byte outside the range the Unicode standard actually
    // allows for that lead byte.
    const std::vector<std::vector<uint8_t>> invalid_glyphs = {
        { 0xC0, 0x80 },             // overlong 2-byte encoding of U+0000
        { 0xC1, 0xBF },             // overlong 2-byte encoding
        { 0xE0, 0x80, 0x80 },       // overlong 3-byte encoding
        { 0xED, 0xA0, 0x80 },       // UTF-16 surrogate U+D800
        { 0xF4, 0x90, 0x80, 0x80 }, // beyond U+10FFFF
        { 0xF5, 0x80, 0x80, 0x80 }  // lead byte past the valid 4-byte range
    };
    for( const std::vector<uint8_t> &glyph : invalid_glyphs ) {
        mp_screen_grid target( 1, 1 );
        CHECK_FALSE( mp_screen_apply( target, single_glyph_payload( glyph ) ) );
        CHECK( all_default( target ) );
    }
}

TEST_CASE( "mp_screen codec round trips a base character with a combining mark", "[mp]" )
{
    // cata_cursesport::fill() (src/cursesport.cpp) deliberately packs
    // zero-width combining marks into the same cursecell::ch as their base
    // character, so a legitimate cell can hold more than one code point.
    // "e" (1 byte) plus U+0301 COMBINING ACUTE ACCENT (0xCC 0x81, 2 bytes)
    // is the simplest real example of that -- a base letter plus a
    // *separate* floating accent, unlike the single precomposed code point
    // "e"-with-acute used elsewhere in this file.
    mp_screen_grid source( 2, 1 );
    source.set( 0, 0, cell( "e\xCC\x81", 7, 0 ) );
    source.set( 1, 0, cell( "#", 3, 1 ) );

    mp_screen_grid target( 2, 1 );
    REQUIRE( mp_screen_apply( target, mp_screen_encode( source ) ) );
    CHECK( target.at( 0, 0 ) == source.at( 0, 0 ) );
    CHECK( target.at( 1, 0 ) == source.at( 1, 0 ) );
}
#endif // MULTIPLAYER
