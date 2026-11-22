#include "log_snapshot.h"

#include <algorithm>
#include <utility>

#include "output.h"

bool Messages::log_exceeds_ttl( const int timestamp_in_user_actions, const int ttl,
                                const int user_actions )
{
    return ttl > 0 && timestamp_in_user_actions + ttl <= user_actions;
}

std::vector<Messages::log_message> log_snapshot_build(
    const std::vector<log_source_entry> &newest_first, const size_t max_messages,
    const bool from_top )
{
    std::vector<Messages::log_message> result;
    if( max_messages == 0 ) {
        return result;
    }
    result.reserve( std::min( max_messages, newest_first.size() ) );

    for( const log_source_entry &entry : newest_first ) {
        if( entry.expired ) {
            // Older entries are older still: the scan stops, it does not skip.
            break;
        }
        if( !from_top && entry.hidden ) {
            continue;
        }

        Messages::log_message line;
        if( !entry.recent ) {
            // Old messages never show inline colour, exactly like the glyph log.
            line.runs.push_back( { remove_color_tags( entry.text ), entry.color } );
        } else {
            for( const colored_text_run &run : split_colored_text( entry.text, entry.color,
                     entry.color, report_color_error::no ) ) {
                // A trailing tag yields an empty run that only carries the
                // colour; the wire forbids empty runs.
                if( !run.text.empty() ) {
                    line.runs.push_back( { run.text, run.color } );
                }
            }
        }

        // A message made only of tags has nothing to draw.
        if( !line.runs.empty() ) {
            result.push_back( std::move( line ) );
        }
        if( result.size() >= max_messages ) {
            break;
        }
    }

    return result;
}
