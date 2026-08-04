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

    private:
        const std::vector<uint8_t> &data_;
        size_t pos_ = 0;
};

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
    reader r( payload );
    int width = 0;
    int height = 0;
    int row_count = 0;
    if( !r.u16( width ) || !r.u16( height ) || !r.u16( row_count ) ) {
        return false;
    }
    if( width != target.width() || height != target.height() ) {
        return false;
    }

    // Decode into a scratch list first so a truncated payload cannot leave the
    // target half-updated.
    struct pending {
        int x;
        int y;
        mp_cell cell;
    };
    std::vector<pending> writes;

    for( int i = 0; i < row_count; ++i ) {
        int y = 0;
        int run_count = 0;
        if( !r.u16( y ) || !r.u16( run_count ) || y < 0 || y >= height ) {
            return false;
        }
        int x = 0;
        for( int j = 0; j < run_count; ++j ) {
            int repeat = 0;
            uint8_t fg = 0;
            uint8_t bg = 0;
            uint8_t glyph_len = 0;
            if( !r.u16( repeat ) || !r.u8( fg ) || !r.u8( bg ) || !r.u8( glyph_len ) ) {
                return false;
            }
            mp_cell c;
            c.fg = fg;
            c.bg = bg;
            if( !r.str( glyph_len, c.ch ) ) {
                return false;
            }
            if( repeat <= 0 || x + repeat > width ) {
                return false;
            }
            for( int k = 0; k < repeat; ++k ) {
                writes.push_back( pending{ x + k, y, c } );
            }
            x += repeat;
        }
        if( x != width ) {
            return false;
        }
    }

    for( const pending &w : writes ) {
        target.set( w.x, w.y, w.cell );
    }
    return true;
}
