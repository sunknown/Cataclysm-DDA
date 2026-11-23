#include "mp/mp_protocol.h"

#include <algorithm>
#include <sstream>

#include "json.h"
#include "json_loader.h"
#include "mp/mp_describe.h"
#include "point.h"

std::vector<uint8_t> mp_wrap( const mp_msg_kind kind, const std::vector<uint8_t> &body )
{
    std::vector<uint8_t> out;
    out.reserve( body.size() + 1 );
    out.push_back( static_cast<uint8_t>( kind ) );
    out.insert( out.end(), body.begin(), body.end() );
    return out;
}

bool mp_unwrap( const std::vector<uint8_t> &payload, mp_msg_kind &kind,
                std::vector<uint8_t> &body )
{
    if( payload.empty() ) {
        return false;
    }
    switch( payload[0] ) {
        case static_cast<uint8_t>( mp_msg_kind::control ):
            kind = mp_msg_kind::control;
            break;
        case static_cast<uint8_t>( mp_msg_kind::screen ):
            kind = mp_msg_kind::screen;
            break;
        case static_cast<uint8_t>( mp_msg_kind::scene ):
            kind = mp_msg_kind::scene;
            break;
        default:
            return false;
    }
    body.assign( payload.begin() + 1, payload.end() );
    return true;
}

namespace
{

// A describe answer is one hover tooltip, so its caps are deliberately tighter
// than the log's whole-sidebar 200 lines / 64 KiB: remote input must not make
// the client allocate a screenful of text per request. The limits themselves
// live in mp_describe.h so the builder and this parser cannot drift apart.

const char *kind_to_string( const mp_control_kind kind )
{
    switch( kind ) {
        case mp_control_kind::hello:
            return "hello";
        case mp_control_kind::hello_ack:
            return "hello_ack";
        case mp_control_kind::resize:
            return "resize";
        case mp_control_kind::input:
            return "input";
        case mp_control_kind::cursor:
            return "cursor";
        case mp_control_kind::bye:
            return "bye";
        case mp_control_kind::character_card:
            return "character_card";
        case mp_control_kind::log:
            return "log";
        case mp_control_kind::describe:
            return "describe";
    }
    return "bye";
}

bool string_to_kind( const std::string &text, mp_control_kind &out )
{
    if( text == "hello" ) {
        out = mp_control_kind::hello;
    } else if( text == "hello_ack" ) {
        out = mp_control_kind::hello_ack;
    } else if( text == "resize" ) {
        out = mp_control_kind::resize;
    } else if( text == "input" ) {
        out = mp_control_kind::input;
    } else if( text == "cursor" ) {
        out = mp_control_kind::cursor;
    } else if( text == "bye" ) {
        out = mp_control_kind::bye;
    } else if( text == "character_card" ) {
        out = mp_control_kind::character_card;
    } else if( text == "log" ) {
        out = mp_control_kind::log;
    } else if( text == "describe" ) {
        out = mp_control_kind::describe;
    } else {
        return false;
    }
    return true;
}

void write_event( JsonOut &jo, const input_event &ev )
{
    jo.member( "etype", static_cast<int>( ev.type ) );
    jo.member( "seq" );
    jo.write( ev.sequence );
    jo.member( "mods" );
    jo.start_array();
    for( const keymod_t m : ev.modifiers ) {
        jo.write( static_cast<int>( m ) );
    }
    jo.end_array();
    jo.member( "mx", ev.mouse_pos.x );
    jo.member( "my", ev.mouse_pos.y );
    jo.member( "text", ev.text );
    jo.member( "edit", ev.edit );
    jo.member( "edit_refresh", ev.edit_refresh );
}

bool read_event( const JsonObject &obj, input_event &ev )
{
    // etype and mods are cast straight into scoped enums. Casting an
    // out-of-range int into an enum is undefined behaviour, and the values come
    // from the peer, so refuse instead of casting. The bounds mirror the
    // enumerators of input_event_t (error..mouse) and keymod_t (ctrl..shift) in
    // input_enums.h; the switch in input_event::long_description() would also
    // hit its default branch for a value that slips past.
    const int etype = obj.get_int( "etype" );
    if( etype < static_cast<int>( input_event_t::error ) ||
        etype > static_cast<int>( input_event_t::mouse ) ) {
        return false;
    }
    ev.type = static_cast<input_event_t>( etype );
    ev.sequence.clear();
    for( const int key : obj.get_int_array( "seq" ) ) {
        ev.sequence.push_back( key );
    }
    ev.modifiers.clear();
    for( const int mod : obj.get_int_array( "mods" ) ) {
        if( mod < static_cast<int>( keymod_t::ctrl ) ||
            mod > static_cast<int>( keymod_t::shift ) ) {
            return false;
        }
        ev.modifiers.insert( static_cast<keymod_t>( mod ) );
    }
    ev.mouse_pos = point( obj.get_int( "mx" ), obj.get_int( "my" ) );
    ev.text = obj.get_string( "text" );
    ev.edit = obj.get_string( "edit" );
    ev.edit_refresh = obj.get_bool( "edit_refresh" );
    return true;
}

// Writes a `lines` member: a nested array of {t,c} runs. The log snapshot and
// the describe answer share the run shape because the client draws both with
// the same palette and wrapping rules.
void write_lines( JsonOut &jo, const char *const member,
                      const std::vector<mp_log_line> &lines )
{
    jo.member( member );
    jo.start_array();
    for( const mp_log_line &line : lines ) {
        jo.start_array();
        for( const mp_log_run &run : line.runs ) {
            jo.start_object();
            jo.member( "t", run.text );
            jo.member( "c", static_cast<int>( run.color ) );
            jo.end_object();
        }
        jo.end_array();
    }
    jo.end_array();
}

// Reads a `lines` member, which must be present. The caller supplies the caps:
// the log snapshot is a whole sidebar, the describe answer one tooltip. A
// malformed shape, an out-of-palette colour or an exceeded cap all fail the
// whole message -- a partly parsed answer could draw a lie.
bool read_lines( const JsonObject &obj, const size_t max_lines,
                     const size_t max_runs, const size_t max_text,
                     std::vector<mp_log_line> &out )
{
    JsonArray lines = obj.get_array( "lines" );
    if( lines.size() > max_lines ) {
        return false;
    }
    size_t text_bytes = 0;
    out.reserve( lines.size() );
    for( size_t i = 0; i < lines.size(); i++ ) {
        JsonArray runs = lines.get_array( i );
        if( runs.size() > max_runs ) {
            return false;
        }
        mp_log_line line;
        line.runs.reserve( runs.size() );
        for( size_t j = 0; j < runs.size(); j++ ) {
            JsonObject run = runs.get_object( j );
            const std::string text = run.get_string( "t" );
            const int color = run.get_int( "c" );
            if( color < 0 || color > 255 ) {
                return false;
            }
            text_bytes += text.size();
            if( text_bytes > max_text ) {
                return false;
            }
            line.runs.push_back( { text, static_cast<uint8_t>( color ) } );
        }
        out.push_back( std::move( line ) );
    }
    return true;
}

} // namespace

std::string mp_control_to_json( const mp_control &msg )
{
    std::ostringstream stream;
    JsonOut jo( stream );
    jo.start_object();
    jo.member( "t", std::string( kind_to_string( msg.kind ) ) );
    switch( msg.kind ) {
        case mp_control_kind::hello:
            jo.member( "proto", msg.hello.proto );
            jo.member( "name", msg.hello.name );
            jo.member( "save_id", msg.hello.save_id );
            jo.member( "world", msg.hello.world );
            jo.member( "w", msg.hello.width );
            jo.member( "h", msg.hello.height );
            jo.member( "c256", msg.hello.colors_256 );
            jo.member( "caps", static_cast<int>( msg.hello.caps ) );
            jo.member( "zl", static_cast<int>( msg.hello.z_levels ) );
            break;
        case mp_control_kind::hello_ack:
            jo.member( "ok", msg.hello_ack.accepted );
            jo.member( "reason", msg.hello_ack.reason );
            jo.member( "caps", static_cast<int>( msg.hello_ack.caps ) );
            break;
        case mp_control_kind::resize:
            jo.member( "w", msg.resize.width );
            jo.member( "h", msg.resize.height );
            break;
        case mp_control_kind::input:
            write_event( jo, msg.event );
            break;
        case mp_control_kind::cursor:
            jo.member( "x", msg.cursor.x );
            jo.member( "y", msg.cursor.y );
            jo.member( "vis", msg.cursor.visible );
            break;
        case mp_control_kind::bye:
            jo.member( "reason", msg.bye_reason );
            break;
        case mp_control_kind::character_card:
            jo.member( "world", msg.character_card.world_name );
            jo.member( "save_id", msg.character_card.save_id );
            jo.member( "name", msg.character_card.character_name );
            break;
        case mp_control_kind::log:
            jo.member( "seq", static_cast<int>( msg.log.seq ) );
            jo.member( "ui", msg.log.ui );
            jo.member( "x", static_cast<int>( msg.log.x ) );
            jo.member( "y", static_cast<int>( msg.log.y ) );
            jo.member( "w", static_cast<int>( msg.log.w ) );
            jo.member( "h", static_cast<int>( msg.log.h ) );
            jo.member( "top", msg.log.top );
            write_lines( jo, "lines", msg.log.lines );
            break;
        case mp_control_kind::describe:
            jo.member( "seq", static_cast<int>( msg.describe.seq ) );
            jo.member( "frame", static_cast<int>( msg.describe.frame ) );
            jo.member( "x", static_cast<int>( msg.describe.x ) );
            jo.member( "y", static_cast<int>( msg.describe.y ) );
            jo.member( "w", static_cast<int>( msg.describe.w ) );
            write_lines( jo, "lines", msg.describe.lines );
            break;
    }
    jo.end_object();
    return stream.str();
}

bool mp_control_from_json( const std::string &text, mp_control &out )
{
    try {
        JsonValue value = json_loader::from_string( text );
        JsonObject obj = value.get_object();
        // Unknown fields are ignored rather than reported. Without this, the
        // JsonObject destructor calls report_unvisited() and fires a debugmsg
        // for every member this function did not read (src/json.cpp), which a
        // peer could trigger at will by adding one field -- on the server that
        // is a debug error prompt raised by remote input, which the input
        // module's contract (mp_server_input.cpp) forbids. It would also
        // freeze the wire format: `proto` exists so a later version can add
        // fields, and an older peer must skip what it does not recognise
        // instead of complaining about it.
        obj.allow_omitted_members();
        if( !obj.has_string( "t" ) ) {
            return false;
        }
        mp_control msg;
        if( !string_to_kind( obj.get_string( "t" ), msg.kind ) ) {
            return false;
        }
        switch( msg.kind ) {
            case mp_control_kind::hello:
                msg.hello.proto = obj.get_int( "proto" );
                msg.hello.name = obj.get_string( "name" );
                // Optional, so an older client's hello still parses and gets the
                // protocol-version refusal instead of being dropped unread.
                msg.hello.save_id = obj.get_string( "save_id", "" );
                msg.hello.world = obj.get_string( "world", "" );
                msg.hello.width = obj.get_int( "w" );
                msg.hello.height = obj.get_int( "h" );
                msg.hello.colors_256 = obj.get_bool( "c256" );
                // Both are optional so an old client still parses. A negative
                // capability mask must not turn into a full set of bits, and an
                // out-of-range level request clamps to the range the scene
                // builder can honour instead of refusing the connection.
                msg.hello.caps = static_cast<uint32_t>(
                                     std::max( 0, obj.get_int( "caps", 0 ) ) );
                msg.hello.z_levels = static_cast<uint8_t>(
                                         std::clamp( obj.get_int( "zl", 10 ), 1, 10 ) );
                break;
            case mp_control_kind::hello_ack:
                msg.hello_ack.accepted = obj.get_bool( "ok" );
                msg.hello_ack.reason = obj.get_string( "reason" );
                // Optional so an older server's ack still parses; it just
                // advertises no streams. A negative mask must not become a
                // full set of bits.
                msg.hello_ack.caps = static_cast<uint32_t>(
                                         std::max( 0, obj.get_int( "caps", 0 ) ) );
                break;
            case mp_control_kind::resize:
                msg.resize.width = obj.get_int( "w" );
                msg.resize.height = obj.get_int( "h" );
                break;
            case mp_control_kind::input:
                if( !read_event( obj, msg.event ) ) {
                    return false;
                }
                break;
            case mp_control_kind::cursor:
                msg.cursor.x = obj.get_int( "x" );
                msg.cursor.y = obj.get_int( "y" );
                msg.cursor.visible = obj.get_bool( "vis" );
                break;
            case mp_control_kind::bye:
                msg.bye_reason = obj.get_string( "reason" );
                break;
            case mp_control_kind::character_card:
                msg.character_card.world_name = obj.get_string( "world" );
                msg.character_card.save_id = obj.get_string( "save_id" );
                msg.character_card.character_name = obj.get_string( "name" );
                break;
            case mp_control_kind::log: {
                const auto dim = []( const int value ) {
                    return static_cast<uint16_t>( std::clamp( value, 0, 65535 ) );
                };
                msg.log.seq = static_cast<uint32_t>( std::max( 0, obj.get_int( "seq" ) ) );
                msg.log.ui = obj.get_bool( "ui" );
                msg.log.x = dim( obj.get_int( "x" ) );
                msg.log.y = dim( obj.get_int( "y" ) );
                msg.log.w = dim( obj.get_int( "w" ) );
                msg.log.h = dim( obj.get_int( "h" ) );
                msg.log.top = obj.get_bool( "top" );

                if( !read_lines( obj, 200, 256, 64u * 1024u, msg.log.lines ) ) {
                    return false;
                }
                break;
            }
            case mp_control_kind::describe: {
                // seq, x, y and w are required: a request without a rectangle
                // names nothing, and an answer without seq cannot be matched to
                // its request. `frame` and `lines` are the answer's, so both are
                // optional -- a request carries neither.
                if( !obj.has_int( "seq" ) || !obj.has_int( "x" ) || !obj.has_int( "y" ) ||
                    !obj.has_int( "w" ) ) {
                    return false;
                }
                const auto dim = []( const int value ) {
                    return static_cast<uint16_t>( std::clamp( value, 0, 65535 ) );
                };
                msg.describe.seq = static_cast<uint32_t>( std::max( 0, obj.get_int( "seq" ) ) );
                msg.describe.frame =
                    static_cast<uint32_t>( std::max( 0, obj.get_int( "frame", 0 ) ) );
                msg.describe.x = dim( obj.get_int( "x" ) );
                msg.describe.y = dim( obj.get_int( "y" ) );
                msg.describe.w = dim( obj.get_int( "w" ) );
                // An answer is all-or-nothing: a partly parsed tooltip could
                // draw a lie, so a bad line fails the whole message.
                if( obj.has_array( "lines" ) &&
                    !read_lines( obj, MP_DESCRIBE_MAX_LINES, MP_DESCRIBE_MAX_RUNS,
                                     MP_DESCRIBE_MAX_TEXT_BYTES, msg.describe.lines ) ) {
                    return false;
                }
                break;
            }
        }
        out = msg;
        return true;
    } catch( const JsonError & ) {
        return false;
    }
}
