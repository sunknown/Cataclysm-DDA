#pragma once
#ifndef CATA_SRC_MP_MP_PROTOCOL_H
#define CATA_SRC_MP_MP_PROTOCOL_H

#include <cstdint>
#include <string>
#include <vector>

#include "input_enums.h"

// Version of the wire protocol. Sent in the client's first message; a
// mismatch is refused with an explanation rather than tolerated, because the
// failure mode of a silent mismatch is a garbled screen.
constexpr int MP_PROTOCOL_VERSION = 3;

// Capability bits. A client asks for streams in its hello; the server answers
// with the set it can serve in the hello_ack, so a client never waits for a
// stream that will not come. Bits are independent.
constexpr uint32_t MP_CAP_SCENE = 1u;
constexpr uint32_t MP_CAP_LOG = 2u;
constexpr uint32_t MP_CAP_DESCRIBE = 4u;
// Everything this server can serve. Advertised in every hello_ack, accepted
// or refused, so a client never waits for a stream that will not come.
constexpr uint32_t MP_SERVER_CAPS = MP_CAP_SCENE | MP_CAP_LOG | MP_CAP_DESCRIBE;

// Every framed payload starts with one kind byte. Control traffic is JSON, so
// it can be read in a log by eye; screen traffic is the compact binary of
// mp_screen_codec, because it is the hot path.
enum class mp_msg_kind : uint8_t {
    control = 1,
    screen = 2,
    // A full 3D scene frame. Sent only to clients that negotiated `caps` bit0;
    // the server itself never receives one.
    scene = 3,
};

std::vector<uint8_t> mp_wrap( mp_msg_kind kind, const std::vector<uint8_t> &body );
bool mp_unwrap( const std::vector<uint8_t> &payload, mp_msg_kind &kind,
                std::vector<uint8_t> &body );

enum class mp_control_kind {
    hello,
    hello_ack,
    resize,
    input,
    cursor,
    bye,
    character_card,
    log,
    describe,
    plan,
    plan_cancel,
    plan_ready,
    plan_pass,
    party,
    phase,
};

struct mp_hello {
    int proto = 0;
    std::string name;
    // The save_id from the client's character card, empty when it has none.
    // The server looks the character up by it (Phase 3b), but only inside the
    // world the card was written in: see mp_card_save_id_in_world().
    std::string save_id;
    // The world that save_id belongs to, from the same card. A card is one per
    // server AND world, and the same address may serve another world whose
    // saves hold a character of the same name, so the save id alone is not
    // enough to say "this is you". Empty when the client has no card.
    std::string world;
    int width = 0;
    int height = 0;
    bool colors_256 = false;
    // Capability bits from MP_CAP_*; the client asks for the streams it can
    // draw. Absent means 0, so an old text client is served exactly as before.
    uint32_t caps = 0;
    // Requested encoded z-levels, 1..10. Absent means 10; the parser clamps out
    // of range requests rather than refusing the client.
    uint8_t z_levels = 10;
};

struct mp_hello_ack {
    bool accepted = false;
    std::string reason;
    // The capability bits the server will honour, from MP_SERVER_CAPS. Absent
    // from an older server means 0: the client must not expect any stream.
    uint32_t caps = 0;
};

struct mp_resize {
    int width = 0;
    int height = 0;
};

struct mp_cursor {
    int x = 0;
    int y = 0;
    bool visible = false;
};

// Server to client, sent the moment the character is established -- not at
// session end. The card does not change during play, and sending it early means
// it survives the server crashing.
struct mp_character_card_msg {
    std::string world_name;
    std::string save_id;
    std::string character_name;
};

// One styled run of a native log line. `color` is a palette index, the same
// byte the screen grid uses.
struct mp_log_run {
    std::string text;
    uint8_t color = 0;
};

// One visible sidebar log message, its runs in reading order.
struct mp_log_line {
    std::vector<mp_log_run> runs;
};

// The phase 4b-1 log snapshot. `ui` separates an in-game screen from join and
// character creation; the rect is a placement hint, zero when there is no
// source panel; `top` mirrors LOG_FLOW=new_top.
struct mp_log_msg {
    uint32_t seq = 0;
    bool ui = false;
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t w = 0;
    uint16_t h = 0;
    bool top = false;
    std::vector<mp_log_line> lines;
};

// Client to server: what is under a rectangle of the client's 3D screen. `w`
// bounds the answer's text width, so the server wraps exactly as the client
// will draw. This is a hover: the client sends it when the cursor rests.
struct mp_describe_request {
    uint32_t seq = 0;
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t w = 0;
};

// Server to client: the describe answer for request `seq`, rendered at `frame`
// (the scene frame the answer was computed from, so the client can discard an
// answer that outlived its world). Same run shape as the log, because both are
// drawn with the same palette and wrapping rules.
struct mp_describe_msg {
    uint32_t seq = 0;
    uint32_t frame = 0;
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t w = 0;
    std::vector<mp_log_line> lines;
};

// What a client intends to do this round. The plan is ephemeral: it lives in
// the round state on the server and is never saved. `cost` is the client's own
// estimate for its overlay only -- the server recomputes it and the client
// never decides what it can afford.
enum class mp_plan_kind : uint8_t {
    none,
    move,
    melee,
    ranged,
    use_item,
    interact,
    pass,
};

struct mp_plan_msg {
    // Client sequence number, echoed in the party snapshot so a client can tell
    // which of its plans the server accepted.
    uint32_t seq = 0;
    mp_plan_kind kind = mp_plan_kind::none;
    int x = 0;
    int y = 0;
    int z = 0;
    int cost = 0;
    // Display label: the creature or item the plan names, empty for plain
    // movement.
    std::string label;
};

// One seat's public round state in the party snapshot. `moves` is the seat's
// current AP, `pcost` the server's cost of its plan (0 when it has none).
struct mp_party_seat {
    uint8_t slot = 0;
    int moves = 0;
    int speed = 0;
    bool ready = false;
    bool passed = false;
    bool connected = false;
    // Set while the seat is in a menu, so the client can draw the menu label
    // and forward raw keys instead of mapping them to plans.
    bool away = false;
    mp_plan_kind plan = mp_plan_kind::none;
    int px = 0;
    int py = 0;
    int pcost = 0;
    // The menu label while the seat is in one, empty otherwise.
    std::string menu;
    // The seat's character name, for the party panel.
    std::string name;
    // The last rejection text for this seat, empty when none: the client shows
    // it instead of guessing why a plan was refused.
    std::string error;
};

// Server to client: the whole party's round state, sent whenever it changes.
//
// A scene cell exists only in its sender's rect -- each seat's frame is
// centred on that seat's own view and window -- so a partner's plan cannot be
// placed by cell index. The header therefore carries the receiver's own frame:
// `you` is its slot, and `vx`/`vy` are that seat's scene origin in bubble
// coordinates. Every seat's `px`/`py` are bubble tiles, and the client maps
// them through its own anchor. Each recipient gets its own body; the seats are
// shared.
struct mp_party_msg {
    uint8_t phase = 0;
    // The recipient's slot, or -1 when it has no context behind it.
    int8_t you = -1;
    // Whether `vx`/`vy` are this recipient's scene origin, or merely 0.
    bool view_known = false;
    int vx = 0;
    int vy = 0;
    std::vector<mp_party_seat> seats;
};

// One control message. Only the member matching `kind` is meaningful; this is
// a plain struct rather than a variant so that adding a message type later is
// a local edit.
struct mp_control {
    mp_control_kind kind = mp_control_kind::bye;
    mp_hello hello;
    mp_hello_ack hello_ack;
    mp_resize resize;
    mp_cursor cursor;
    input_event event;
    std::string bye_reason;
    mp_character_card_msg character_card;
    mp_log_msg log;
    mp_describe_msg describe;
    // plan and its three siblings (cancel/ready/pass) carry only the sequence.
    mp_plan_msg plan;
    mp_party_msg party;
    // The round phase, meaningful for the `phase` message.
    uint8_t phase = 0;
};

std::string mp_control_to_json( const mp_control &msg );
bool mp_control_from_json( const std::string &text, mp_control &out );

#endif // CATA_SRC_MP_MP_PROTOCOL_H
