#include "cata_catch.h"

#include <cstdint>
#include <vector>

#include "mp/mp_frame.h"

static std::vector<uint8_t> bytes( const std::string &s )
{
    return std::vector<uint8_t>( s.begin(), s.end() );
}

TEST_CASE( "mp_frame round trips a single payload", "[mp]" )
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

TEST_CASE( "mp_frame reassembles a payload split across feeds", "[mp]" )
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

TEST_CASE( "mp_frame yields several payloads from one feed", "[mp]" )
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

TEST_CASE( "mp_frame rejects an oversized length header", "[mp]" )
{
    // A hostile or corrupt peer must not be able to make us allocate freely.
    std::vector<uint8_t> frame = { 0xFF, 0xFF, 0xFF, 0xFF };

    mp_frame_reader reader;
    reader.feed( frame.data(), frame.size() );

    CHECK( reader.failed() );
    CHECK_FALSE( reader.next().has_value() );
}

TEST_CASE( "mp_frame accepts an empty payload", "[mp]" )
{
    const std::vector<uint8_t> frame = mp_frame_encode( {} );
    mp_frame_reader reader;
    reader.feed( frame.data(), frame.size() );

    const std::optional<std::vector<uint8_t>> got = reader.next();
    REQUIRE( got.has_value() );
    CHECK( got->empty() );
}
