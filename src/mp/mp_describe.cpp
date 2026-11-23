#include "mp/mp_describe.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cursesdef.h"
#if defined( MP_SERVER )
#include "cursesport.h"
#include "mp/mp_context.h"
#include "mp/mp_scene.h"
#include "mp/mp_server_mask.h"
#endif
#include "game.h"
#include "map.h"
#include "point.h"

namespace
{

// Cuts to at most `limit` bytes without leaving a partial UTF-8 sequence: the
// first excluded byte is advanced past every continuation byte.
std::string cut_utf8( const std::string &text, const size_t limit )
{
    if( text.size() <= limit ) {
        return text;
    }
    size_t cut = limit;
    while( cut > 0 && ( static_cast<unsigned char>( text[cut] ) & 0xC0 ) == 0x80 ) {
        --cut;
    }
    return text.substr( 0, cut );
}

bool is_blank( const mp_cell &cell )
{
    return cell.ch == " ";
}

} // namespace

std::vector<mp_log_line> mp_describe_extract( const std::vector<mp_cell> &cells,
        const int width, const int height )
{
    std::vector<mp_log_line> lines;
    if( width <= 0 || height <= 0 ) {
        return lines;
    }

    // Only whole rows are read; a short grid is a caller bug, not a reason to
    // invent cells.
    const size_t row_width = static_cast<size_t>( width );
    const size_t available_rows = cells.size() / row_width;
    const size_t capped_rows = std::min( static_cast<size_t>( height ), available_rows );
    const int rows = static_cast<int>( capped_rows );
    if( rows <= 0 ) {
        return lines;
    }

    // Where each row's text starts and ends, trailing blanks already removed.
    // Leading blanks are kept for now so the narrowest margin can be found:
    // blank rows do not vote, or an indented block would lose its alignment.
    struct row_span {
        int begin = 0;
        int end = 0;
    };
    std::vector<row_span> spans( static_cast<size_t>( rows ) );
    int indent = width;
    for( int y = 0; y < rows; ++y ) {
        const size_t base = static_cast<size_t>( y ) * row_width;
        int begin = 0;
        while( begin < width && is_blank( cells[base + begin] ) ) {
            ++begin;
        }
        int end = width;
        while( end > begin && is_blank( cells[base + end - 1] ) ) {
            --end;
        }
        spans[y] = { begin, end };
        if( begin < end ) {
            indent = std::min( indent, begin );
        }
    }
    if( indent >= width ) {
        // Nothing but blanks.
        return lines;
    }

    // Leading and trailing blank rows are margins, an inner one is a gap the
    // client draws as an empty line.
    int first = 0;
    while( first < rows && spans[first].begin >= spans[first].end ) {
        ++first;
    }
    int last = rows - 1;
    while( last > first && spans[last].begin >= spans[last].end ) {
        --last;
    }

    size_t text_bytes = 0;
    for( int y = first; y <= last; ++y ) {
        if( lines.size() >= static_cast<size_t>( MP_DESCRIBE_MAX_LINES ) ) {
            break;
        }
        if( text_bytes >= MP_DESCRIBE_MAX_TEXT_BYTES ) {
            // The answer is at the byte cap; a later blank row would only be
            // trailing padding.
            break;
        }

        const row_span &span = spans[y];
        if( span.begin >= span.end ) {
            lines.push_back( mp_log_line() );
            continue;
        }

        mp_log_line line;
        bool full = false;
        const size_t base = static_cast<size_t>( y ) * row_width;
        for( int x = indent; x < span.end; ++x ) {
            const mp_cell &cell = cells[base + x];
            if( cell.ch.empty() ) {
                // The second column of a wide glyph adds no character, so the
                // rest of the row shifts left by one. The limit is documented,
                // not papered over with an invented space.
                continue;
            }
            if( text_bytes >= MP_DESCRIBE_MAX_TEXT_BYTES ) {
                full = true;
                break;
            }
            const size_t remaining = MP_DESCRIBE_MAX_TEXT_BYTES - text_bytes;
            const std::string text = cut_utf8( cell.ch, remaining );
            if( text.empty() ) {
                // Not even this glyph fits; the answer ends here.
                full = true;
                break;
            }
            if( !line.runs.empty() && line.runs.back().color == cell.fg ) {
                // The run cap bounds run count, not characters, so a merged
                // run keeps the line's text.
                line.runs.back().text += text;
            } else if( line.runs.size() < static_cast<size_t>( MP_DESCRIBE_MAX_RUNS ) ) {
                line.runs.push_back( { text, cell.fg } );
            } else {
                // The line is already at the run cap; the rest of it is cut.
                break;
            }
            text_bytes += text.size();
            if( text.size() < cell.ch.size() ) {
                full = true;
                break;
            }
        }

        if( !line.runs.empty() ) {
            lines.push_back( std::move( line ) );
        }
        if( full ) {
            break;
        }
    }

    return lines;
}

#if defined( MP_SERVER )
std::vector<mp_log_line> mp_describe_lines( const tripoint_bub_ms &tile, const int width )
{
    std::vector<mp_log_line> lines;
    if( width <= 0 ) {
        return lines;
    }

    // The scratch window is never refreshed or blitted: it exists only to let
    // the game's own tile-info renderer produce text the feed can read back.
    catacurses::window win = catacurses::newwin( MP_DESCRIBE_MAX_LINES, width, point::zero );
    if( !win ) {
        return lines;
    }
    werase( win );
    int line = 0;
    g->pre_print_all_tile_info( tile, win, line, MP_DESCRIBE_MAX_LINES - 1,
                                get_map().get_visibility_variables_cache() );

    std::vector<mp_cell> cells;
    cells.reserve( static_cast<size_t>( width ) * MP_DESCRIBE_MAX_LINES );
    for( int y = 0; y < MP_DESCRIBE_MAX_LINES; ++y ) {
        for( int x = 0; x < width; ++x ) {
            catacurses::window_cell cell;
            if( !catacurses::get_cell( win, point( x, y ), cell ) ) {
                // A window that lost a cell is a curses failure; keep the row
                // aligned with a blank rather than shift the text.
                cells.push_back( mp_cell() );
                continue;
            }
            mp_cell out;
            out.ch = std::move( cell.ch );
            out.fg = cell.fg;
            out.bg = cell.bg;
            cells.push_back( std::move( out ) );
        }
    }

    return mp_describe_extract( cells, width, MP_DESCRIBE_MAX_LINES );
}

mp_describe_msg mp_describe_reply( const uint32_t seq, const uint32_t frame,
                                   const int cell_x, const int cell_y, const int width )
{
    mp_describe_msg msg;
    msg.seq = seq;
    msg.frame = frame;
    msg.x = static_cast<uint16_t>( cell_x );
    msg.y = static_cast<uint16_t>( cell_y );
    // The effective width reaches `w` before any check, so a failed answer
    // still reports the size the client would have drawn. The renderer below
    // gets the same value rather than the raw request.
    const int effective_width = std::clamp( width, MP_DESCRIBE_MIN_WIDTH,
                              MP_DESCRIBE_MAX_WIDTH );
    msg.w = static_cast<uint16_t>( effective_width );

    if( g == nullptr ) {
        return msg;
    }
    const player_context *const ctx = mp_active_context_ptr();
    if( ctx == nullptr ) {
        return msg;
    }
    // The live map window, never a cached rect, decides which cells exist. A
    // resize between the request and this call leaves the answer empty rather
    // than describing a cell the client cannot point at.
    mp_scene_rect rect;
    if( !mp_scene_mask_map_rect( rect ) || rect.w == 0 || rect.h == 0 ) {
        return msg;
    }
    const std::optional<tripoint_bub_ms> tile =
        mp_scene_tile_at( rect, ctx->ter_view_p, cell_x, cell_y );
    if( !tile ) {
        return msg;
    }
    if( !get_map().inbounds( *tile ) ) {
        return msg;
    }
    map &here = get_map();
    // A hover can be answered before the active seat's own draw has rebuilt the
    // shared visibility state. Activating a seat and the world half of a turn
    // only mark it dirty; until it is rebuilt, apparent_light_at() still reads
    // the previous activation's fog and vision threshold, and a tile this
    // player can see comes back as "Unseen.". The dirty flag bounds the
    // rebuild to once per activation, not once per hover.
    if( here.get_visibility_variables_cache().visibility_cache_dirty ) {
        here.build_map_cache( ctx->ter_view_p.z(), true );
        here.update_visibility_cache( ctx->ter_view_p.z() );
    }
    msg.lines = mp_describe_lines( *tile, effective_width );
    return msg;
}
#endif // MP_SERVER
