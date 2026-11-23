#pragma once
#ifndef CATA_SRC_MESSAGES_H
#define CATA_SRC_MESSAGES_H

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "calendar.h"
#include "color.h"
#include "coords_fwd.h"
#include "debug.h"
#include "enums.h"
#include "string_formatter.h"
#include "translation.h"

class Creature;
class JsonObject;
class JsonOut;

namespace catacurses
{
class window;
} // namespace catacurses

namespace Messages
{

std::vector<std::pair<std::string, std::string>> recent_messages( size_t count );
bool has_debug_filter( debugmode::debug_filter type );
void add_msg( std::string msg );
void add_msg( const game_message_params &params, std::string msg );
// One player action with both the acting player's and another seat's text.
// `npc_msg` keeps the <npcname> placeholder; `actor_id` is the stable
// character name and `actor_display` is the display name frozen at add time.
void add_msg_player_or_npc( const game_message_params &params, std::string player_msg,
                            std::string npc_msg, const std::string &actor_id,
                            const std::string &actor_display );
void clear_messages();
void deactivate();
// Re-enables message logging without discarding the history deactivate() left
// in place. A session that keeps playing after a character's death needs this:
// game::is_game_over() deactivates the log for the single-player death screen,
// and everybody still at a terminal has to keep seeing messages.
void activate();
size_t size();
bool has_undisplayed_messages();
void display_messages();
void display_messages( const catacurses::window &ipk_target, int left, int top, int right,
                       int bottom );

// One styled segment of a sidebar log line. `color` is the colour in force for
// the segment: inline <color_*> tags are already resolved, and an old message's
// segment carries the message's old colour.
struct log_run {
    std::string text;
    nc_color color;
};

// One visible sidebar log message, its runs in reading order.
struct log_message {
    std::vector<log_run> runs;
};

// The sidebar log, newest first, filtered exactly like display_messages: a
// message past the TTL stops the scan, and a cooldown-hidden message is skipped
// only when the log flows downwards. Reads `curmes`; never advances it.
std::vector<log_message> sidebar_snapshot( size_t max_messages );

// The same snapshot resolved for one viewer: the actor's own messages keep
// the second-person text, another seat reads the third-person form with the
// actor's display name. An empty viewer id is the no-actor path.
std::vector<log_message> sidebar_snapshot_for( const std::string &viewer_id,
        size_t max_messages );

// The same snapshot with an explicit clock and user-action counter. A test seam:
// game::user_action_counter is private, and TTL and recency need it controlled.
std::vector<log_message> sidebar_snapshot_at( size_t max_messages, time_point curmes,
        int user_actions, bool from_top );

// True when a message inserted at `timestamp_in_user_actions` is past the TTL
// at `user_actions`. Shared by message_exceeds_ttl and the snapshot.
bool log_exceeds_ttl( int timestamp_in_user_actions, int ttl, int user_actions );

void serialize( JsonOut &json );
void deserialize( const JsonObject &json );

} // namespace Messages

void add_msg( std::string msg );
template<typename ...Args>
inline void add_msg( const std::string &msg, Args &&... args )
{
    return add_msg( string_format( msg, std::forward<Args>( args )... ) );
}
template<typename ...Args>
inline void add_msg( const char *const msg, Args &&... args )
{
    return add_msg( string_format( msg, std::forward<Args>( args )... ) );
}
template<typename ...Args>
inline void add_msg( const translation &msg, Args &&... args )
{
    return add_msg( string_format( msg.translated(), std::forward<Args>( args )... ) );
}

void add_msg( const game_message_params &params, std::string msg );
template<typename ...Args>
inline void add_msg( const game_message_params &params, const std::string &msg, Args &&... args )
{
    if( params.type == m_debug && !debug_mode ) {
        return;
    }
    return add_msg( params, string_format( msg, std::forward<Args>( args )... ) );
}
template<typename ...Args>
inline void add_msg( const game_message_params &params, const char *const msg, Args &&... args )
{
    if( params.type == m_debug && !debug_mode ) {
        return;
    }
    return add_msg( params, string_format( msg, std::forward<Args>( args )... ) );
}

void add_msg_if_player_sees( const tripoint_bub_ms &target, std::string msg );
void add_msg_if_player_sees( const Creature &target, std::string msg );
template<typename ...Args>
inline void add_msg_if_player_sees( const tripoint_bub_ms &target, const std::string &msg,
                                    Args &&... args )
{
    return add_msg_if_player_sees( target, string_format( msg, std::forward<Args>( args )... ) );
}
template<typename ...Args>
inline void add_msg_if_player_sees( const Creature &target, const std::string &msg,
                                    Args &&... args )
{
    return add_msg_if_player_sees( target, string_format( msg, std::forward<Args>( args )... ) );
}
template<typename ...Args>
inline void add_msg_if_player_sees( const tripoint_bub_ms &target, const char *const msg,
                                    Args &&... args )
{
    return add_msg_if_player_sees( target, string_format( msg, std::forward<Args>( args )... ) );
}
template<typename ...Args>
inline void add_msg_if_player_sees( const Creature &target, const char *const msg, Args &&... args )
{
    return add_msg_if_player_sees( target, string_format( msg, std::forward<Args>( args )... ) );
}
template<typename ...Args>
inline void add_msg_if_player_sees( const tripoint_bub_ms &target, const translation &msg,
                                    Args &&... args )
{
    return add_msg_if_player_sees( target, string_format( msg, std::forward<Args>( args )... ) );
}
template<typename ...Args>
inline void add_msg_if_player_sees( const Creature &target, const translation &msg,
                                    Args &&... args )
{
    return add_msg_if_player_sees( target, string_format( msg, std::forward<Args>( args )... ) );
}

void add_msg_if_player_sees( const tripoint_bub_ms &target, const game_message_params &params,
                             std::string msg );
void add_msg_if_player_sees( const Creature &target, const game_message_params &params,
                             std::string msg );
template<typename ...Args>
inline void add_msg_if_player_sees( const tripoint_bub_ms &target,
                                    const game_message_params &params,
                                    const std::string &msg, Args &&... args )
{
    if( params.type == m_debug && !debug_mode ) {
        return;
    }
    return add_msg_if_player_sees( target, params, string_format( msg,
                                   std::forward<Args>( args )... ) );
}
template<typename ...Args>
inline void add_msg_if_player_sees( const Creature &target, const game_message_params &params,
                                    const std::string &msg, Args &&... args )
{
    if( params.type == m_debug && !debug_mode ) {
        return;
    }
    return add_msg_if_player_sees( target, params, string_format( msg,
                                   std::forward<Args>( args )... ) );
}
template<typename ...Args>
inline void add_msg_if_player_sees( const tripoint_bub_ms &target,
                                    const game_message_params &params,
                                    const char *const msg, Args &&... args )
{
    if( params.type == m_debug && !debug_mode ) {
        return;
    }
    return add_msg_if_player_sees( target, params, string_format( msg,
                                   std::forward<Args>( args )... ) );
}
template<typename ...Args>
inline void add_msg_if_player_sees( const Creature &target, const game_message_params &params,
                                    const char *const msg, Args &&... args )
{
    if( params.type == m_debug && !debug_mode ) {
        return;
    }
    return add_msg_if_player_sees( target, params, string_format( msg,
                                   std::forward<Args>( args )... ) );
}

// This function acts as a marker to clang-tidy to enforce no translations
// in developer facing debug messages.
template <typename T>
inline T &&clang_tidy_no_translations( T &&t )
{
    return std::forward<T>( t );
}

#define add_msg_debug_if(condition, type, ...)                                                          \
    do {                                                                                                \
        if( debug_mode && Messages::has_debug_filter( type ) && ( condition ) ) {                       \
            std::string cata_dbg_msg = clang_tidy_no_translations( string_format( __VA_ARGS__ ) );      \
            Messages::add_msg( m_debug, cata_dbg_msg );                                                 \
            debug_menu::log_debug_msg( type, cata_dbg_msg );                                            \
        }                                                                                               \
    } while( false )

#define add_msg_debug(type, ...) \
    add_msg_debug_if( true, type, __VA_ARGS__ )

namespace debug_menu
{
void log_debug_msg( debugmode::debug_filter type, const std::string &msg );
} // namespace debug_menu

void modify_msg_with_exclamations( std::string &msg, game_message_type type );

#endif // CATA_SRC_MESSAGES_H
