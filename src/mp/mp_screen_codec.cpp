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

// Largest number of bytes a single glyph may occupy on the wire. This must
// stay comfortably above what cata_cursesport::fill() (src/cursesport.cpp)
// can legitimately produce -- a base character plus a handful of zero-width
// combining marks -- while staying far below 255, since glyph_len is a
// single byte on the wire and mp_screen_encode must never let a clamped
// glyph's length wrap around that byte.
constexpr size_t glyph_max_bytes = 32;

// Returns the byte length of the well-formed UTF-8 code point starting at
// `ch[pos]`, or 0 if the bytes there do not form one. This is more than a
// lead-byte-length check: it validates every continuation byte against the
// exact range the Unicode standard allows for that lead byte, which is what
// rules out overlong encodings (e.g. C0 80), UTF-16 surrogates smuggled in
// as a 3-byte sequence (ED A0..BF ..), and 4-byte sequences past U+10FFFF.
size_t utf8_code_point_len( const std::string &ch, const size_t pos )
{
    const size_t n = ch.size();
    const uint8_t b0 = static_cast<uint8_t>( ch[pos] );

    if( ( b0 & 0x80 ) == 0x00 ) {
        return 1;
    }

    // True when byte `pos + i` exists and falls within [lo, hi].
    const auto in_range = [&ch, n, pos]( const size_t i, const uint8_t lo, const uint8_t hi ) {
        if( pos + i >= n ) {
            return false;
        }
        const uint8_t b = static_cast<uint8_t>( ch[pos + i] );
        return b >= lo && b <= hi;
    };

    if( b0 >= 0xC2 && b0 <= 0xDF ) {
        return in_range( 1, 0x80, 0xBF ) ? 2 : 0;
    }
    if( b0 == 0xE0 ) {
        return in_range( 1, 0xA0, 0xBF ) && in_range( 2, 0x80, 0xBF ) ? 3 : 0;
    }
    if( b0 >= 0xE1 && b0 <= 0xEC ) {
        return in_range( 1, 0x80, 0xBF ) && in_range( 2, 0x80, 0xBF ) ? 3 : 0;
    }
    if( b0 == 0xED ) {
        // Second byte capped at 0x9F, not 0xBF: 0xA0-0xBF here would encode
        // U+D800-U+DFFF, the UTF-16 surrogate range, which is not a valid
        // Unicode scalar value on its own.
        return in_range( 1, 0x80, 0x9F ) && in_range( 2, 0x80, 0xBF ) ? 3 : 0;
    }
    if( b0 >= 0xEE && b0 <= 0xEF ) {
        return in_range( 1, 0x80, 0xBF ) && in_range( 2, 0x80, 0xBF ) ? 3 : 0;
    }
    if( b0 == 0xF0 ) {
        // Second byte starts at 0x90, not 0x80: 0x80-0x8F here would be an
        // overlong encoding of a code point already reachable with a 3-byte
        // sequence.
        return in_range( 1, 0x90, 0xBF ) && in_range( 2, 0x80, 0xBF ) &&
               in_range( 3, 0x80, 0xBF ) ? 4 : 0;
    }
    if( b0 >= 0xF1 && b0 <= 0xF3 ) {
        return in_range( 1, 0x80, 0xBF ) && in_range( 2, 0x80, 0xBF ) &&
               in_range( 3, 0x80, 0xBF ) ? 4 : 0;
    }
    if( b0 == 0xF4 ) {
        // Second byte capped at 0x8F: above that the code point would
        // exceed U+10FFFF, the highest valid Unicode scalar value.
        return in_range( 1, 0x80, 0x8F ) && in_range( 2, 0x80, 0xBF ) &&
               in_range( 3, 0x80, 0xBF ) ? 4 : 0;
    }
    // 0x80-0xC1 (bare continuation bytes, plus the two lead bytes that can
    // only start an overlong 2-byte sequence) and 0xF5-0xFF (past the
    // highest valid 4-byte lead byte) are never a valid lead byte.
    return 0;
}

// A glyph is either empty (a continuation cell) or a sequence of one or more
// well-formed UTF-8 code points, up to glyph_max_bytes total. More than one
// code point is not a wire-format curiosity to tolerate: cata_cursesport::fill()
// deliberately packs a base character together with any zero-width combining
// marks that follow it into a single cursecell::ch (its own comment calls
// this out; the loop there breaks only at the *second* non-zero-width
// character), so a legitimate encoder can and does send e.g. "e" followed by
// a combining acute accent as one glyph. This is the trust boundary: the
// value ends up in a client's rendering path (utf8_width, mk_wcwidth, ...),
// so malformed input is rejected here rather than trusted downstream.
bool valid_glyph( const std::string &ch )
{
    if( ch.empty() ) {
        return true;
    }
    if( ch.size() > glyph_max_bytes ) {
        return false;
    }
    size_t pos = 0;
    while( pos < ch.size() ) {
        const size_t len = utf8_code_point_len( ch, pos );
        if( len == 0 ) {
            return false;
        }
        pos += len;
    }
    return true;
}

// Truncates `ch` to at most glyph_max_bytes bytes without splitting a code
// point, so mp_screen_encode can never emit a glyph_len whose true length
// wrapped around after a static_cast<uint8_t>. Nothing upstream bounds how
// many zero-width combining marks cata_cursesport::fill() can accumulate
// into one cell, so this is a defensive clamp against pathological input,
// not something ordinary text (which stays far under the cap) ever reaches.
std::string clamp_glyph( const std::string &ch, const size_t max_bytes )
{
    if( ch.size() <= max_bytes ) {
        return ch;
    }
    size_t len = max_bytes;
    // Back off while `len` lands inside a multi-byte sequence (a
    // continuation byte, 10xxxxxx), so the cut always falls on a code point
    // boundary.
    while( len > 0 && ( static_cast<uint8_t>( ch[len] ) & 0xC0 ) == 0x80 ) {
        --len;
    }
    return ch.substr( 0, len );
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
            if( static_cast<size_t>( glyph_len ) > glyph_max_bytes ) {
                return false;
            }
            std::string ch;
            if( !r.str( glyph_len, ch ) ) {
                return false;
            }
            if( !valid_glyph( ch ) ) {
                return false;
            }
            // The x + repeat > width half of this check is not
            // independently observable through the return value alone: any
            // payload that overshoots also leaves x > width at the end of
            // the row, which the x != width check below rejects on its own.
            // It still runs first, as a fail-fast guard against reading
            // repeat cells that would land past the row.
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
            const std::string glyph = clamp_glyph( first.ch, glyph_max_bytes );
            put_u16( runs, repeat );
            runs.push_back( first.fg );
            runs.push_back( first.bg );
            runs.push_back( static_cast<uint8_t>( glyph.size() ) );
            runs.insert( runs.end(), glyph.begin(), glyph.end() );
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
    //
    // This bound is an early exit, not an independent security guard: by
    // pigeonhole, any row_count greater than height already forces at least
    // one row_index to repeat or fall outside [0, height) among the rows
    // that follow, and walk_rows() rejects both of those on its own. Without
    // this check the payload would still be rejected -- just after parsing
    // further into it first.
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
    // `target`. Every row was already validated, so this walk should not be
    // able to fail; its result is still propagated rather than assumed, so a
    // future edit that ever made the two passes diverge would surface as a
    // returned `false` instead of silently leaving `target` half-updated.
    reader body( payload );
    body.skip( 6 );
    std::vector<bool> seen_row_again( static_cast<size_t>( height ), false );
    return walk_rows( body, row_count, width, height, seen_row_again, &target );
}
