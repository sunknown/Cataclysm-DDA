#include "mp/mp_client_screen.h"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "cursesdef.h"
#include "debug.h"
#include "mp/mp_protocol.h"
#include "mp/mp_screen_codec.h"

// The pure-logic half of the thin client. See mp_client_screen.h for the split
// between this file and mp_client_main.cpp.

namespace
{

// cursesport's pair table has 100 entries (src/cursesport.h:85). Pair 0 is the
// curses default, pair 1 is reserved as the white-on-black fallback, and the
// cacheable slots run 2..97: 96 pairs, the cap the design mandates. The small
// gap between 97 and 99 is a deliberate safety margin.
constexpr int MP_PAIR_CACHE_MAX = 96;
constexpr int MP_PAIR_FIRST = 2;
constexpr int MP_PAIR_FALLBACK = 1;

// The pair cache proper plus a one-shot "already warned about exhaustion" flag,
// so a screen full of novel colour combinations logs a single line instead of
// one per cell. Process-wide for the same reason cursesport's pair table is: a
// render pass on a later message must find the pair an earlier pass registered.
struct pair_cache_state {
    std::map<std::pair<uint8_t, uint8_t>, int> pairs;
    bool exhaustion_logged = false;
};

pair_cache_state &pair_cache()
{
    static pair_cache_state cache;
    return cache;
}

} // namespace

bool mp_parse_host_port( const std::string &text, std::string &host, int &port )
{
    // The last colon separates the host from the port, so "host:port" works
    // even when the host itself is an IPv6 literal ("::1:4321" -> "::1" + 4321).
    const size_t colon = text.rfind( ':' );
    if( colon == std::string::npos || colon == 0 || colon + 1 >= text.size() ) {
        // A missing colon, an empty host, or an empty port are all unusable.
        return false;
    }
    const std::string host_part = text.substr( 0, colon );
    const std::string port_part = text.substr( colon + 1 );
    if( port_part.find_first_not_of( "0123456789" ) != std::string::npos ) {
        // A non-numeric port (e.g. "notaport") cannot be dialled.
        return false;
    }
    long parsed = 0;
    try {
        parsed = std::stol( port_part );
    } catch( const std::exception & ) {
        // Out of the range a long can hold; also not a diallable port.
        return false;
    }
    if( parsed < 1 || parsed > 65535 ) {
        return false;
    }
    host = host_part;
    port = static_cast<int>( parsed );
    return true;
}

mp_client_event mp_client_apply_message( const std::vector<uint8_t> &payload,
        mp_screen_grid &screen, mp_client_state &state )
{
    mp_msg_kind kind = mp_msg_kind::control;
    std::vector<uint8_t> body;
    if( !mp_unwrap( payload, kind, body ) ) {
        return mp_client_event::none;
    }

    if( kind == mp_msg_kind::screen ) {
        // A screen the client cannot reconcile with its grid (size mismatch,
        // malformed runs, hostile glyphs) is dropped whole; the caller keeps
        // whatever the grid held before.
        if( !mp_screen_apply( screen, body ) ) {
            return mp_client_event::none;
        }
        return mp_client_event::screen;
    }

    mp_control msg;
    if( !mp_control_from_json( std::string( body.begin(), body.end() ), msg ) ) {
        return mp_client_event::none;
    }
    switch( msg.kind ) {
        case mp_control_kind::cursor:
            state.cursor_x = msg.cursor.x;
            state.cursor_y = msg.cursor.y;
            state.cursor_visible = msg.cursor.visible;
            return mp_client_event::cursor;
        case mp_control_kind::hello_ack:
            state.accepted = msg.hello_ack.accepted;
            state.reject_reason = msg.hello_ack.reason;
            // An acceptance needs no further action from the caller; only a
            // refusal is an event the loop must react to.
            return state.accepted ? mp_client_event::none : mp_client_event::rejected;
        case mp_control_kind::bye:
            // The server is closing the session; the caller treats this as the
            // signal to stop the loop and exit cleanly.
            return mp_client_event::closed;
        case mp_control_kind::character_card:
            state.card = msg.character_card;
            return mp_client_event::character_card;
        default:
            // hello, resize and input travel from the client to the server;
            // receiving them back means nothing actionable.
            return mp_client_event::none;
    }
}

std::vector<uint8_t> mp_client_encode_input( const input_event &ev )
{
    mp_control msg;
    msg.kind = mp_control_kind::input;
    msg.event = ev;
    const std::string json = mp_control_to_json( msg );
    return mp_wrap( mp_msg_kind::control, std::vector<uint8_t>( json.begin(), json.end() ) );
}

int mp_client_pair_for( const uint8_t fg, const uint8_t bg )
{
    const std::pair<uint8_t, uint8_t> key( fg, bg );
    pair_cache_state &cache = pair_cache();
    const auto it = cache.pairs.find( key );
    if( it != cache.pairs.end() ) {
        return it->second;
    }
    if( cache.pairs.size() >= MP_PAIR_CACHE_MAX ) {
        // The pair table is finite; degrading to the readable default beats
        // corrupting it. In practice CDDA keeps well under forty live colour
        // combinations, so real play never reaches this branch.
        if( !cache.exhaustion_logged ) {
            cache.exhaustion_logged = true;
            DebugLog( D_WARNING, D_MAIN ) << "mp client: colour pair cache exhausted; "
                                          "falling back to pair 1 (white on black)";
        }
        return MP_PAIR_FALLBACK;
    }
    const int pair = MP_PAIR_FIRST + static_cast<int>( cache.pairs.size() );
    // The fg/bg values are indices into the 256-colour space; base_color is
    // a plain integer enum, so a static_cast is the whole conversion.
    catacurses::init_pair( static_cast<short>( pair ),
                           static_cast<catacurses::base_color>( fg ),
                           static_cast<catacurses::base_color>( bg ) );
    cache.pairs.emplace( key, pair );
    return pair;
}

void mp_client_reset_pairs_for_test()
{
    pair_cache() = pair_cache_state();
}
