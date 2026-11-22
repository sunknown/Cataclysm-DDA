#include "mp/mp_describe.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace
{

// A line of more runs than this is truncated. The run cap is a per-line draw
// budget; the text cap is global, so one tall answer cannot crowd out the
// rest of the connection.
constexpr size_t describe_max_runs = 256;

// Cuts to at most `limit` bytes without leaving a partial UTF-8 sequence: the
// first excluded byte is advanced past every continuation byte.
std::string utf8_truncate( const std::string &text, const size_t limit )
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
            const std::string text = utf8_truncate( cell.ch, remaining );
            if( text.empty() ) {
                // Not even this glyph fits; the answer ends here.
                full = true;
                break;
            }
            if( !line.runs.empty() && line.runs.back().color == cell.fg ) {
                // The run cap bounds run count, not characters, so a merged
                // run keeps the line's text.
                line.runs.back().text += text;
            } else if( line.runs.size() < describe_max_runs ) {
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
