#include "mp/mp_protocol.h"

#include <algorithm>
#include <sstream>

#include "json.h"
#include "json_loader.h"
#include "mp/mp_describe.h"
#include "mp/mp_turn.h"
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
        case mp_control_kind::plan:
            return "plan";
        case mp_control_kind::plan_cancel:
            return "plan_cancel";
        case mp_control_kind::plan_ready:
            return "plan_ready";
        case mp_control_kind::plan_pass:
            return "plan_pass";
        case mp_control_kind::party:
            return "party";
        case mp_control_kind::phase:
            return "phase";
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
    } else if( text == "plan" ) {
        out = mp_control_kind::plan;
    } else if( text == "plan_cancel" ) {
        out = mp_control_kind::plan_cancel;
    } else if( text == "plan_ready" ) {
        out = mp_control_kind::plan_ready;
    } else if( text == "plan_pass" ) {
        out = mp_control_kind::plan_pass;
    } else if( text == "party" ) {
        out = mp_control_kind::party;
    } else if( text == "phase" ) {
        out = mp_control_kind::phase;
    } else {
        return false;
    }
    return true;
}

// The plans are their own enum but share the wire with the control kinds: a
// lowercase name per enumerator, refused when unknown rather than defaulted,
// because defaulting to none would silently drop a peer's intent.
const char *plan_kind_to_string( const mp_plan_kind kind )
{
    switch( kind ) {
        case mp_plan_kind::none:
            return "none";
        case mp_plan_kind::move:
            return "move";
        case mp_plan_kind::melee:
            return "melee";
        case mp_plan_kind::ranged:
            return "ranged";
        case mp_plan_kind::use_item:
            return "use_item";
        case mp_plan_kind::interact:
            return "interact";
        case mp_plan_kind::pass:
            return "pass";
    }
    return "none";
}

bool string_to_plan_kind( const std::string &text, mp_plan_kind &out )
{
    if( text == "none" ) {
        out = mp_plan_kind::none;
    } else if( text == "move" ) {
        out = mp_plan_kind::move;
    } else if( text == "melee" ) {
        out = mp_plan_kind::melee;
    } else if( text == "ranged" ) {
        out = mp_plan_kind::ranged;
    } else if( text == "use_item" ) {
        out = mp_plan_kind::use_item;
    } else if( text == "interact" ) {
        out = mp_plan_kind::interact;
    } else if( text == "pass" ) {
        out = mp_plan_kind::pass;
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
        case mp_control_kind::plan:
            jo.member( "seq", static_cast<int>( msg.plan.seq ) );
            jo.member( "kind", std::string( plan_kind_to_string( msg.plan.kind ) ) );
            jo.member( "x", msg.plan.x );
            jo.member( "y", msg.plan.y );
            jo.member( "z", msg.plan.z );
            jo.member( "cost", msg.plan.cost );
            jo.member( "label", msg.plan.label );
            break;
        case mp_control_kind::plan_cancel:
        case mp_control_kind::plan_ready:
        case mp_control_kind::plan_pass:
            jo.member( "seq", static_cast<int>( msg.plan.seq ) );
            break;
        case mp_control_kind::party:
            jo.member( "phase", static_cast<int>( msg.party.phase ) );
            jo.member( "you", static_cast<int>( msg.party.you ) );
            jo.member( "anchor_known", msg.party.anchor_known );
            jo.member( "ax", msg.party.ax );
            jo.member( "ay", msg.party.ay );
            jo.member( "seats" );
            jo.start_array();
            for( const mp_party_seat &seat : msg.party.seats ) {
                jo.start_object();
                jo.member( "slot", static_cast<int>( seat.slot ) );
                jo.member( "moves", seat.moves );
                jo.member( "speed", seat.speed );
                jo.member( "ready", seat.ready );
                jo.member( "passed", seat.passed );
                jo.member( "connected", seat.connected );
                jo.member( "away", seat.away );
                jo.member( "plan", std::string( plan_kind_to_string( seat.plan ) ) );
                jo.member( "px", seat.px );
                jo.member( "py", seat.py );
                jo.member( "pcost", seat.pcost );
                jo.member( "menu", seat.menu );
                jo.member( "name", seat.name );
                jo.member( "error", seat.error );
                jo.member( "availability", seat.availability );
                jo.end_object();
            }
            jo.end_array();
            break;
        case mp_control_kind::phase:
            jo.member( "phase", static_cast<int>( msg.phase ) );
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
            case mp_control_kind::plan:
                if( !obj.has_string( "kind" ) ) {
                    return false;
                }
                if( !string_to_plan_kind( obj.get_string( "kind" ), msg.plan.kind ) ) {
                    // An intent this build does not understand must not be
                    // guessed at: defaulting to none would drop a real plan.
                    return false;
                }
                msg.plan.seq = static_cast<uint32_t>( std::max( 0, obj.get_int( "seq" ) ) );
                msg.plan.x = obj.get_int( "x" );
                msg.plan.y = obj.get_int( "y" );
                msg.plan.z = obj.get_int( "z" );
                msg.plan.cost = obj.get_int( "cost" );
                msg.plan.label = obj.get_string( "label" );
                break;
            case mp_control_kind::plan_cancel:
            case mp_control_kind::plan_ready:
            case mp_control_kind::plan_pass:
                msg.plan.seq = static_cast<uint32_t>( std::max( 0, obj.get_int( "seq" ) ) );
                break;
            case mp_control_kind::party: {
                const auto byte = []( const int value ) {
                    return static_cast<uint8_t>( std::clamp( value, 0, 255 ) );
                };
                msg.party.phase = byte( obj.get_int( "phase" ) );
                // Absent in a body from before the header existed: the default
                // -1/false/0 tells the client it has no anchor and must not
                // place plans. The removed v4 names are unknown members here
                // and are ignored, never read as an anchor.
                msg.party.you = static_cast<int8_t>(
                                    std::clamp( obj.get_int( "you", -1 ), -1, 127 ) );
                msg.party.anchor_known = obj.get_bool( "anchor_known", false );
                msg.party.ax = obj.get_int( "ax", 0 );
                msg.party.ay = obj.get_int( "ay", 0 );
                JsonArray seats = obj.get_array( "seats" );
                msg.party.seats.reserve( seats.size() );
                for( size_t i = 0; i < seats.size(); i++ ) {
                    JsonObject seat = seats.get_object( i );
                    // Same reasoning as the outer object: an early refusal
                    // below must not leave members unread, which would raise a
                    // debugmsg triggered by remote input.
                    seat.allow_omitted_members();
                    mp_party_seat out_seat;
                    out_seat.slot = byte( seat.get_int( "slot" ) );
                    out_seat.moves = seat.get_int( "moves" );
                    out_seat.speed = seat.get_int( "speed" );
                    out_seat.ready = seat.get_bool( "ready" );
                    out_seat.passed = seat.get_bool( "passed" );
                    out_seat.connected = seat.get_bool( "connected" );
                    out_seat.away = seat.get_bool( "away" );
                    if( !seat.has_string( "plan" ) ||
                        !string_to_plan_kind( seat.get_string( "plan" ), out_seat.plan ) ) {
                        return false;
                    }
                    out_seat.px = seat.get_int( "px" );
                    out_seat.py = seat.get_int( "py" );
                    out_seat.pcost = seat.get_int( "pcost" );
                    out_seat.menu = seat.get_string( "menu" );
                    out_seat.name = seat.get_string( "name" );
                    out_seat.error = seat.get_string( "error" );
                    // Mandatory and validated: a missing or unknown state is a
                    // body this client cannot draw, not a default to guess at.
                    mp_action_availability availability =
                        mp_action_availability::unavailable;
                    if( !seat.has_string( "availability" ) ||
                        !mp_action_availability_from_string(
                            seat.get_string( "availability" ), availability ) ) {
                        return false;
                    }
                    out_seat.availability = mp_action_availability_name( availability );
                    msg.party.seats.push_back( std::move( out_seat ) );
                }
                break;
            }
            case mp_control_kind::phase:
                msg.phase = static_cast<uint8_t>(
                                std::clamp( obj.get_int( "phase" ), 0, 255 ) );
                break;
        }
        out = msg;
        return true;
    } catch( const JsonError & ) {
        return false;
    }
}
