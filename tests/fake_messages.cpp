#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "calendar.h"
#include "fake_messages.h"
#include "coords_fwd.h"
#include "messages.h"

class Creature;
class JsonObject;
class JsonOut;
namespace debugmode
{
enum debug_filter : int;
}  // namespace debugmode
struct game_message_params;

namespace catacurses
{
class window;
}  // namespace catacurses

/**
 * For unit testing we just store all messages so they can be dumped in the
 * event of a test failure.
 */

static std::vector<std::pair<std::string, std::string>> messages;

std::vector<std::pair<std::string, std::string>> Messages::recent_messages( size_t )
{
    return messages;
}
bool Messages::has_debug_filter( debugmode::debug_filter )
{
    return true;
}
void Messages::add_msg( std::string m )
{
    if( !m.empty() ) {
        messages.emplace_back( to_string_time_of_day( calendar::turn ), std::move( m ) );
    }
}
void Messages::add_msg( const game_message_params &, std::string m )
{
    add_msg( std::move( m ) );
}
void Messages::clear_messages()
{
    messages.clear();
}
void Messages::deactivate() {}
void Messages::activate() {}
size_t Messages::size()
{
    return messages.size();
}
bool Messages::has_undisplayed_messages()
{
    return false;
}
void Messages::display_messages() {}
void Messages::display_messages( const catacurses::window &, int, int, int, int ) {}
void Messages::serialize( JsonOut & ) {}
void Messages::deserialize( const JsonObject & ) {}
void add_msg( std::string m )
{
    Messages::add_msg( std::move( m ) );
}
void add_msg( const game_message_params &, std::string m )
{
    Messages::add_msg( std::move( m ) );
}
void add_msg_if_player_sees( const tripoint_bub_ms &, std::string m )
{
    Messages::add_msg( std::move( m ) );
}
void add_msg_if_player_sees( const Creature &, std::string m )
{
    Messages::add_msg( std::move( m ) );
}
void add_msg_if_player_sees( const tripoint_bub_ms &, const game_message_params &, std::string m )
{
    Messages::add_msg( std::move( m ) );
}
void add_msg_if_player_sees( const Creature &, const game_message_params &, std::string m )
{
    Messages::add_msg( std::move( m ) );
}

// Phase 4b-1: the snapshot the server's body builder reads. The real
// implementation lives in src/messages.cpp, which this binary does not link.
static std::vector<Messages::log_message> sidebar_snapshot_stub;

void set_fake_sidebar_snapshot( std::vector<Messages::log_message> lines )
{
    sidebar_snapshot_stub = std::move( lines );
}

std::vector<Messages::log_message> Messages::sidebar_snapshot( size_t )
{
    return sidebar_snapshot_stub;
}

std::vector<Messages::log_message> Messages::sidebar_snapshot_at( size_t, time_point, int, bool )
{
    return sidebar_snapshot_stub;
}
