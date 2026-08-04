#include "mp/mp_frame.h"

#include <algorithm>

#include "cata_assert.h"

static constexpr size_t header_bytes = 4;

// Decodes the four byte big-endian length header at the front of buffer.
// Caller must ensure buffer.size() >= header_bytes.
static uint32_t decode_header_len( const std::vector<uint8_t> &buffer )
{
    return ( static_cast<uint32_t>( buffer[0] ) << 24 ) |
           ( static_cast<uint32_t>( buffer[1] ) << 16 ) |
           ( static_cast<uint32_t>( buffer[2] ) << 8 ) |
           static_cast<uint32_t>( buffer[3] );
}

std::vector<uint8_t> mp_frame_encode( const std::vector<uint8_t> &payload )
{
    // Callers must never ask us to encode a payload the reader would refuse:
    // truncating the size into the 32 bit header would silently desynchronise
    // the stream. This is a programmer error, not a runtime condition.
    cata_assert( payload.size() <= MP_MAX_FRAME_BYTES );
    std::vector<uint8_t> out;
    out.reserve( header_bytes + payload.size() );
    const uint32_t len = static_cast<uint32_t>( payload.size() );
    out.push_back( static_cast<uint8_t>( ( len >> 24 ) & 0xFF ) );
    out.push_back( static_cast<uint8_t>( ( len >> 16 ) & 0xFF ) );
    out.push_back( static_cast<uint8_t>( ( len >> 8 ) & 0xFF ) );
    out.push_back( static_cast<uint8_t>( len & 0xFF ) );
    out.insert( out.end(), payload.begin(), payload.end() );
    return out;
}

void mp_frame_reader::feed( const uint8_t *const data, const size_t len )
{
    if( failed_ ) {
        return;
    }
    buffer_.insert( buffer_.end(), data, data + len );
    // Validate the header currently at the head of the buffer as soon as it
    // is available, rather than waiting for next() to be called, so a hostile
    // length at the very start of the stream shows up in failed() right away.
    // A malformed header buried behind an earlier valid frame is still only
    // caught once next() walks the stream far enough to reach it.
    if( buffer_.size() >= header_bytes && decode_header_len( buffer_ ) > MP_MAX_FRAME_BYTES ) {
        failed_ = true;
        buffer_.clear();
    }
}

std::optional<std::vector<uint8_t>> mp_frame_reader::next()
{
    if( failed_ || buffer_.size() < header_bytes ) {
        return std::nullopt;
    }
    const uint32_t len = decode_header_len( buffer_ );
    if( len > MP_MAX_FRAME_BYTES ) {
        failed_ = true;
        buffer_.clear();
        return std::nullopt;
    }
    if( buffer_.size() < header_bytes + len ) {
        return std::nullopt;
    }
    std::vector<uint8_t> payload( buffer_.begin() + header_bytes,
                                  buffer_.begin() + header_bytes + len );
    buffer_.erase( buffer_.begin(), buffer_.begin() + header_bytes + len );
    return payload;
}
