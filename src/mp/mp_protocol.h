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
constexpr int MP_PROTOCOL_VERSION = 2;

// Every framed payload starts with one kind byte. Control traffic is JSON, so
// it can be read in a log by eye; screen traffic is the compact binary of
// mp_screen_codec, because it is the hot path.
enum class mp_msg_kind : uint8_t {
    control = 1,
    screen = 2,
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
};

struct mp_hello_ack {
    bool accepted = false;
    std::string reason;
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
};

std::string mp_control_to_json( const mp_control &msg );
bool mp_control_from_json( const std::string &text, mp_control &out );

#endif // CATA_SRC_MP_MP_PROTOCOL_H
