#include "mp/mp_cell.h"

#include <algorithm>

mp_screen_grid::mp_screen_grid( const int width, const int height )
{
    resize( width, height );
}

void mp_screen_grid::resize( const int width, const int height )
{
    width_ = std::max( 0, width );
    height_ = std::max( 0, height );
    cells_.assign( static_cast<size_t>( width_ ) * height_, mp_cell() );
    dirty_.assign( height_, true );
}

const mp_cell &mp_screen_grid::at( const int x, const int y ) const
{
    static const mp_cell out_of_range;
    if( x < 0 || y < 0 || x >= width_ || y >= height_ ) {
        return out_of_range;
    }
    return cells_[static_cast<size_t>( y ) * width_ + x];
}

void mp_screen_grid::set( const int x, const int y, const mp_cell &c )
{
    if( x < 0 || y < 0 || x >= width_ || y >= height_ ) {
        return;
    }
    mp_cell &dest = cells_[static_cast<size_t>( y ) * width_ + x];
    if( dest == c ) {
        return;
    }
    dest = c;
    dirty_[y] = true;
}

void mp_screen_grid::mark_all_dirty()
{
    dirty_.assign( height_, true );
}

void mp_screen_grid::clear_dirty()
{
    dirty_.assign( height_, false );
}

bool mp_screen_grid::any_dirty() const
{
    return std::any_of( dirty_.begin(), dirty_.end(), []( const bool d ) {
        return d;
    } );
}

std::vector<int> mp_screen_grid::dirty_rows() const
{
    std::vector<int> rows;
    for( int y = 0; y < height_; ++y ) {
        if( dirty_[y] ) {
            rows.push_back( y );
        }
    }
    return rows;
}
