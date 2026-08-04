#include "mp/mp_protocol.h"

#include <sstream>

#include "json.h"
#include "json_loader.h"
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
        default:
            return false;
    }
    body.assign( payload.begin() + 1, payload.end() );
    return true;
}

namespace
{

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

void read_event( const JsonObject &obj, input_event &ev )
{
    ev.type = static_cast<input_event_t>( obj.get_int( "etype" ) );
    ev.sequence.clear();
    for( const int key : obj.get_int_array( "seq" ) ) {
        ev.sequence.push_back( key );
    }
    ev.modifiers.clear();
    for( const int mod : obj.get_int_array( "mods" ) ) {
        ev.modifiers.insert( static_cast<keymod_t>( mod ) );
    }
    ev.mouse_pos = point( obj.get_int( "mx" ), obj.get_int( "my" ) );
    ev.text = obj.get_string( "text" );
    ev.edit = obj.get_string( "edit" );
    ev.edit_refresh = obj.get_bool( "edit_refresh" );
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
            jo.member( "w", msg.hello.width );
            jo.member( "h", msg.hello.height );
            jo.member( "c256", msg.hello.colors_256 );
            break;
        case mp_control_kind::hello_ack:
            jo.member( "ok", msg.hello_ack.accepted );
            jo.member( "reason", msg.hello_ack.reason );
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
                msg.hello.width = obj.get_int( "w" );
                msg.hello.height = obj.get_int( "h" );
                msg.hello.colors_256 = obj.get_bool( "c256" );
                break;
            case mp_control_kind::hello_ack:
                msg.hello_ack.accepted = obj.get_bool( "ok" );
                msg.hello_ack.reason = obj.get_string( "reason" );
                break;
            case mp_control_kind::resize:
                msg.resize.width = obj.get_int( "w" );
                msg.resize.height = obj.get_int( "h" );
                break;
            case mp_control_kind::input:
                read_event( obj, msg.event );
                break;
            case mp_control_kind::cursor:
                msg.cursor.x = obj.get_int( "x" );
                msg.cursor.y = obj.get_int( "y" );
                msg.cursor.visible = obj.get_bool( "vis" );
                break;
            case mp_control_kind::bye:
                msg.bye_reason = obj.get_string( "reason" );
                break;
        }
        out = msg;
        return true;
    } catch( const JsonError & ) {
        return false;
    }
}
