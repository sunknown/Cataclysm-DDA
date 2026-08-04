#include "cata_catch.h"

#if defined(MULTIPLAYER)
#include <cstdint>
#include <vector>

#include "mp/mp_frame.h"

static std::vector<uint8_t> bytes( const std::string &s )
{
    return std::vector<uint8_t>( s.begin(), s.end() );
}

// Builds a raw four byte big-endian length header, with no body attached.
// Used to probe mp_frame_reader's handling of a header in isolation, without
// going through mp_frame_encode (which is itself under test elsewhere).
static std::vector<uint8_t> header_for( uint32_t len )
{
    return {
        static_cast<uint8_t>( ( len >> 24 ) & 0xFF ),
        static_cast<uint8_t>( ( len >> 16 ) & 0xFF ),
        static_cast<uint8_t>( ( len >> 8 ) & 0xFF ),
        static_cast<uint8_t>( len & 0xFF )
    };
}

TEST_CASE( "mp_frame_round_trips_a_single_payload", "[mp]" )
{
    const std::vector<uint8_t> payload = bytes( "hello" );
    const std::vector<uint8_t> frame = mp_frame_encode( payload );

    mp_frame_reader reader;
    reader.feed( frame.data(), frame.size() );

    const std::optional<std::vector<uint8_t>> got = reader.next();
    REQUIRE( got.has_value() );
    CHECK( *got == payload );
    CHECK_FALSE( reader.next().has_value() );
}

TEST_CASE( "mp_frame_reassembles_a_payload_split_across_feeds", "[mp]" )
{
    // A byte-at-a-time feed is the worst case a socket can hand us: the reader
    // must yield nothing until the very last byte of the frame arrives.
    const std::vector<uint8_t> payload = bytes( "a longer payload than four bytes" );
    const std::vector<uint8_t> frame = mp_frame_encode( payload );

    mp_frame_reader reader;
    for( size_t i = 0; i + 1 < frame.size(); ++i ) {
        reader.feed( &frame[i], 1 );
        REQUIRE_FALSE( reader.next().has_value() );
    }
    reader.feed( &frame[frame.size() - 1], 1 );

    const std::optional<std::vector<uint8_t>> got = reader.next();
    REQUIRE( got.has_value() );
    CHECK( *got == payload );
}

TEST_CASE( "mp_frame_yields_several_payloads_from_one_feed", "[mp]" )
{
    std::vector<uint8_t> stream = mp_frame_encode( bytes( "one" ) );
    const std::vector<uint8_t> second = mp_frame_encode( bytes( "two" ) );
    stream.insert( stream.end(), second.begin(), second.end() );

    mp_frame_reader reader;
    reader.feed( stream.data(), stream.size() );

    REQUIRE( reader.next() == bytes( "one" ) );
    REQUIRE( reader.next() == bytes( "two" ) );
    CHECK_FALSE( reader.next().has_value() );
}

TEST_CASE( "mp_frame_rejects_an_oversized_length_header", "[mp]" )
{
    // A hostile or corrupt peer must not be able to make us allocate freely.
    std::vector<uint8_t> frame = { 0xFF, 0xFF, 0xFF, 0xFF };

    mp_frame_reader reader;
    reader.feed( frame.data(), frame.size() );

    CHECK( reader.failed() );
    CHECK_FALSE( reader.next().has_value() );
}

TEST_CASE( "mp_frame_accepts_an_empty_payload", "[mp]" )
{
    const std::vector<uint8_t> frame = mp_frame_encode( {} );
    mp_frame_reader reader;
    reader.feed( frame.data(), frame.size() );

    const std::optional<std::vector<uint8_t>> got = reader.next();
    REQUIRE( got.has_value() );
    CHECK( got->empty() );
}

TEST_CASE( "mp_frame_uses_a_big-endian_length_header", "[mp]" )
{
    // Byte order is a contract with whatever is on the other end of the
    // socket; round-tripping through our own encoder and decoder together
    // would not catch a swapped-endianness regression.
    const std::vector<uint8_t> frame = mp_frame_encode( bytes( "hello" ) );
    REQUIRE( frame.size() == 4 + 5 );
    CHECK( frame[0] == 0x00 );
    CHECK( frame[1] == 0x00 );
    CHECK( frame[2] == 0x00 );
    CHECK( frame[3] == 0x05 );

    // A reader fed a hand-built big-endian frame (no mp_frame_encode
    // involved) must decode it the same way.
    std::vector<uint8_t> hand_built = { 0x00, 0x00, 0x00, 0x03, 'f', 'o', 'o' };
    mp_frame_reader reader;
    reader.feed( hand_built.data(), hand_built.size() );
    CHECK( reader.next() == bytes( "foo" ) );
}

TEST_CASE( "mp_frame_accepts_a_length_header_exactly_at_the_limit", "[mp]" )
{
    // The body is deliberately not supplied: this only probes the header
    // check, which must accept MP_MAX_FRAME_BYTES itself (a strict ">" bound,
    // not ">=").
    const std::vector<uint8_t> frame = header_for( static_cast<uint32_t>( MP_MAX_FRAME_BYTES ) );

    mp_frame_reader reader;
    reader.feed( frame.data(), frame.size() );

    CHECK_FALSE( reader.failed() );
    CHECK_FALSE( reader.next().has_value() );
}

TEST_CASE( "mp_frame_rejects_a_length_header_one_past_the_limit", "[mp]" )
{
    const std::vector<uint8_t> frame = header_for( static_cast<uint32_t>( MP_MAX_FRAME_BYTES ) + 1 );

    mp_frame_reader reader;
    reader.feed( frame.data(), frame.size() );

    CHECK( reader.failed() );
    CHECK_FALSE( reader.next().has_value() );
}

TEST_CASE( "mp_frame_catches_a_bad_header_once_next___reaches_it", "[mp]" )
{
    // feed()'s eager check only looks at the header currently at the head of
    // the buffer. A malformed header arriving behind an earlier valid frame
    // is not detected until next() consumes its way to it.
    std::vector<uint8_t> stream = mp_frame_encode( bytes( "one" ) );
    const std::vector<uint8_t> bad_header = { 0xFF, 0xFF, 0xFF, 0xFF };
    stream.insert( stream.end(), bad_header.begin(), bad_header.end() );

    mp_frame_reader reader;
    reader.feed( stream.data(), stream.size() );
    CHECK_FALSE( reader.failed() );

    REQUIRE( reader.next() == bytes( "one" ) );
    CHECK_FALSE( reader.next().has_value() );
    CHECK( reader.failed() );
}
#endif // MULTIPLAYER
