#include "mp/mp_screen_codec.h"

#include <cstddef>
#include <string>

#include "mp/mp_cell.h"

namespace
{

void put_u16( std::vector<uint8_t> &out, const int value )
{
    out.push_back( static_cast<uint8_t>( ( value >> 8 ) & 0xFF ) );
    out.push_back( static_cast<uint8_t>( value & 0xFF ) );
}

class reader
{
    public:
        reader( const std::vector<uint8_t> &data ) : data_( data ) {}

        bool u8( uint8_t &out ) {
            if( pos_ >= data_.size() ) {
                return false;
            }
            out = data_[pos_++];
            return true;
        }
        bool u16( int &out ) {
            uint8_t hi = 0;
            uint8_t lo = 0;
            if( !u8( hi ) || !u8( lo ) ) {
                return false;
            }
            out = ( static_cast<int>( hi ) << 8 ) | static_cast<int>( lo );
            return true;
        }
        bool str( const size_t len, std::string &out ) {
            if( pos_ + len > data_.size() ) {
                return false;
            }
            out.assign( data_.begin() + pos_, data_.begin() + pos_ + len );
            pos_ += len;
            return true;
        }
        // Advances past `len` bytes without decoding them. Used to fast
        // forward a fresh reader past a header already parsed by another one.
        bool skip( const size_t len ) {
            if( pos_ + len > data_.size() ) {
                return false;
            }
            pos_ += len;
            return true;
        }

    private:
        const std::vector<uint8_t> &data_;
        size_t pos_ = 0;
};

// A glyph is either empty (a continuation cell) or 1-4 bytes forming exactly
// one well-formed UTF-8 code point. This is the trust boundary: the value
// ends up in a client's rendering path (utf8_width, mk_wcwidth, ...), so
// malformed input is rejected here rather than trusted downstream.
bool valid_glyph( const std::string &ch )
{
    if( ch.empty() ) {
        return true;
    }
    if( ch.size() > 4 ) {
        return false;
    }
    const uint8_t lead = static_cast<uint8_t>( ch[0] );
    size_t expected_len = 0;
    if( ( lead & 0x80 ) == 0x00 ) {
        expected_len = 1;
    } else if( ( lead & 0xE0 ) == 0xC0 ) {
        expected_len = 2;
    } else if( ( lead & 0xF0 ) == 0xE0 ) {
        expected_len = 3;
    } else if( ( lead & 0xF8 ) == 0xF0 ) {
        expected_len = 4;
    } else {
        // Not a valid UTF-8 lead byte -- either a lone continuation byte
        // (10xxxxxx) or one of the bytes UTF-8 never uses.
        return false;
    }
    if( expected_len != ch.size() ) {
        return false;
    }
    for( size_t i = 1; i < ch.size(); ++i ) {
        if( ( static_cast<uint8_t>( ch[i] ) & 0xC0 ) != 0x80 ) {
            return false;
        }
    }
    return true;
}

// Walks `row_count` rows out of `r`, one call per row read from the wire.
// When `target` is null this only validates the payload and records which
// row indices were seen (rejecting duplicates -- a legitimate encoder never
// repeats a row). When `target` is non-null it additionally writes each
// decoded cell straight into it.
//
// The same function backs both the validation pass and the apply pass in
// mp_screen_apply: calling it once with target == nullptr and then again,
// with a fresh reader at the same offset, with target == &grid guarantees
// the apply pass cannot fail, so a malformed payload never leaves the
// target half-updated -- without needing to buffer every decoded cell in a
// scratch vector first. A hostile row_count of up to 65535 rows, each
// claiming a single run spanning the whole row, would otherwise force a
// decoder that buffers writes to materialise width * row_count pending
// entries, which is a remote memory-exhaustion vector; this walk never
// allocates more than the row_count-length "seen" bitmap.
bool walk_rows( reader &r, const int row_count, const int width, const int height,
                 std::vector<bool> &seen_row, mp_screen_grid *target )
{
    for( int i = 0; i < row_count; ++i ) {
        int y = 0;
        int run_count = 0;
        if( !r.u16( y ) || !r.u16( run_count ) || y < 0 || y >= height ) {
            return false;
        }
        if( seen_row[y] ) {
            return false;
        }
        seen_row[y] = true;

        int x = 0;
        for( int j = 0; j < run_count; ++j ) {
            int repeat = 0;
            uint8_t fg = 0;
            uint8_t bg = 0;
            uint8_t glyph_len = 0;
            if( !r.u16( repeat ) || !r.u8( fg ) || !r.u8( bg ) || !r.u8( glyph_len ) ) {
                return false;
            }
            if( glyph_len > 4 ) {
                return false;
            }
            std::string ch;
            if( !r.str( glyph_len, ch ) ) {
                return false;
            }
            if( !valid_glyph( ch ) ) {
                return false;
            }
            if( repeat <= 0 || x + repeat > width ) {
                return false;
            }
            if( target != nullptr ) {
                mp_cell c;
                c.fg = fg;
                c.bg = bg;
                c.ch = ch;
                for( int k = 0; k < repeat; ++k ) {
                    target->set( x + k, y, c );
                }
            }
            x += repeat;
        }
        if( x != width ) {
            return false;
        }
    }
    return true;
}

} // namespace

std::vector<uint8_t> mp_screen_encode( const mp_screen_grid &grid )
{
    const std::vector<int> rows = grid.dirty_rows();

    std::vector<uint8_t> out;
    put_u16( out, grid.width() );
    put_u16( out, grid.height() );
    put_u16( out, static_cast<int>( rows.size() ) );

    for( const int y : rows ) {
        put_u16( out, y );

        // Runs are built into a scratch buffer first, because the run count
        // precedes them on the wire and is not known until the row is walked.
        std::vector<uint8_t> runs;
        int run_count = 0;
        int x = 0;
        while( x < grid.width() ) {
            const mp_cell &first = grid.at( x, y );
            int repeat = 1;
            while( x + repeat < grid.width() && grid.at( x + repeat, y ) == first ) {
                ++repeat;
            }
            put_u16( runs, repeat );
            runs.push_back( first.fg );
            runs.push_back( first.bg );
            runs.push_back( static_cast<uint8_t>( first.ch.size() ) );
            runs.insert( runs.end(), first.ch.begin(), first.ch.end() );
            ++run_count;
            x += repeat;
        }

        put_u16( out, run_count );
        out.insert( out.end(), runs.begin(), runs.end() );
    }

    return out;
}

bool mp_screen_apply( mp_screen_grid &target, const std::vector<uint8_t> &payload )
{
    reader header( payload );
    int width = 0;
    int height = 0;
    int row_count = 0;
    if( !header.u16( width ) || !header.u16( height ) || !header.u16( row_count ) ) {
        return false;
    }
    if( width != target.width() || height != target.height() ) {
        return false;
    }
    // dirty_rows() can never report more than `height` distinct rows, so a
    // legitimate encoder never sends more; a bigger count can only come from
    // a hostile or corrupt peer. Reject it before doing any row work.
    if( row_count > height ) {
        return false;
    }

    // Validation pass: walk the whole payload without touching `target`, so
    // a malformed or truncated payload never leaves it half-updated.
    std::vector<bool> seen_row( static_cast<size_t>( height ), false );
    if( !walk_rows( header, row_count, width, height, seen_row, nullptr ) ) {
        return false;
    }

    // Apply pass: a fresh reader skips past the header (already validated
    // above -- 3 u16 fields, 6 bytes) and this time writes straight into
    // `target`. Every row was already validated, so this walk cannot fail.
    reader body( payload );
    body.skip( 6 );
    std::vector<bool> seen_row_again( static_cast<size_t>( height ), false );
    walk_rows( body, row_count, width, height, seen_row_again, &target );
    return true;
}
