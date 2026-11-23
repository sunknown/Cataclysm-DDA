#include "mp/mp_log.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

#include "panels.h"

#if defined(MP_SERVER)
#include "cursesport.h"
#include "messages.h"
#include "mp/mp_protocol.h"
#endif

namespace
{

mp_scene_rect source_rect;
bool source_rect_valid = false;

} // namespace

void mp_native_log_note_rect( const int x, const int y, const int w, const int h )
{
    if( w <= 0 || h <= 0 ) {
        // A panel with no area is no panel. Keep the origin out of the wire.
        source_rect = {};
        source_rect_valid = false;
        return;
    }
    source_rect.x = static_cast<uint16_t>( std::max( 0, x ) );
    source_rect.y = static_cast<uint16_t>( std::max( 0, y ) );
    source_rect.w = static_cast<uint16_t>( w );
    source_rect.h = static_cast<uint16_t>( h );
    source_rect_valid = true;
}

bool mp_native_log_source_rect( mp_scene_rect &out )
{
    if( !source_rect_valid ) {
        return false;
    }
    out = source_rect;
    return true;
}

void mp_native_log_clear_rect()
{
    source_rect = {};
    source_rect_valid = false;
}

bool mp_native_log_panel_enabled()
{
    for( const window_panel &panel :
         panel_manager::get_manager().get_current_layout().panels() ) {
        if( panel.get_id() == "Log" ) {
            return panel.toggle && panel.render();
        }
    }
    return false;
}

#if defined(MP_SERVER)

namespace
{

constexpr size_t log_max_lines = 200;
constexpr size_t log_max_runs = 256;
constexpr size_t log_max_text_bytes = 64u * 1024u;

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

} // namespace

std::string mp_log_body( const bool from_top, const bool ui, const mp_scene_rect &rect,
                         const uint32_t seq, const std::string &viewer_id )
{
    mp_control message;
    message.kind = mp_control_kind::log;
    message.log.seq = seq;
    message.log.ui = ui;
    message.log.x = rect.x;
    message.log.y = rect.y;
    message.log.w = rect.w;
    message.log.h = rect.h;
    message.log.top = from_top;

    size_t text_bytes = 0;
    for( const Messages::log_message &line :
         Messages::sidebar_snapshot_for( viewer_id, log_max_lines ) ) {
        if( message.log.lines.size() >= log_max_lines ) {
            break;
        }

        mp_log_line out;
        bool out_of_room = false;
        for( const Messages::log_run &run : line.runs ) {
            if( run.text.empty() ) {
                continue;
            }
            if( text_bytes >= log_max_text_bytes ) {
                out_of_room = true;
                break;
            }

            const size_t remaining = log_max_text_bytes - text_bytes;
            const bool truncated = run.text.size() > remaining;
            const std::string text = truncated ? utf8_truncate( run.text, remaining ) : run.text;

            if( !text.empty() ) {
                if( out.runs.size() < log_max_runs ) {
                    out.runs.push_back( { text, nc_color_foreground_index( run.color ) } );
                } else {
                    // No characters are lost to the run cap.
                    out.runs.back().text += text;
                }
                text_bytes += text.size();
            }

            if( truncated ) {
                out_of_room = true;
                break;
            }
        }

        if( !out.runs.empty() ) {
            message.log.lines.push_back( std::move( out ) );
        }
        if( out_of_room ) {
            break;
        }
    }

    return mp_control_to_json( message );
}

uint64_t mp_log_hash( const std::string &body )
{
    // The sequence is the only field that moves without the log changing. Skip
    // the digits after "seq": and hash everything else.
    size_t skip_begin = std::string::npos;
    size_t skip_end = std::string::npos;
    const size_t seq_pos = body.find( "\"seq\":" );
    if( seq_pos != std::string::npos ) {
        size_t i = seq_pos + 6;
        while( i < body.size() && std::isspace( static_cast<unsigned char>( body[i] ) ) ) {
            ++i;
        }
        skip_begin = i;
        while( i < body.size() && std::isdigit( static_cast<unsigned char>( body[i] ) ) ) {
            ++i;
        }
        skip_end = i;
    }

    uint64_t hash = 1469598103934665603ull;
    for( size_t i = 0; i < body.size(); i++ ) {
        if( i >= skip_begin && i < skip_end ) {
            continue;
        }
        hash ^= static_cast<unsigned char>( body[i] );
        hash *= 1099511628211ull;
    }
    return hash;
}

#endif // MP_SERVER
