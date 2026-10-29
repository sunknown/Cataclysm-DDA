#pragma once
#ifndef CATA_SRC_MP_MP_CHARACTER_CARD_H
#define CATA_SRC_MP_MP_CHARACTER_CARD_H

#include <string>

#include "cata_path.h"

// What the client remembers about a character it plays on some server: enough to
// say "I am this survivor" on the next connect, and nothing more.
//
// Deliberately not the character's save. A save without its world is unplayable
// -- position, missions, factions and map memory all reference that world -- so
// keeping one would be a backup that cannot restore anything, plus a question
// about which copy wins. The server is authoritative; this is a token.
struct mp_character_card {
    std::string server;          // host:port, as typed into --connect
    std::string world_name;
    std::string save_id;         // matches save_t::decoded_name() on the server
    std::string character_name;  // for the menu line and the log

    bool empty() const {
        return save_id.empty();
    }
};

// Where a card for `server` lives in the client's config dir. One file per
// server, so "your characters, per server" is a directory listing.
cata_path mp_card_path( const std::string &server );

// Reads the card for `server`. An absent file reads as an empty card, never as
// an error: the worst case is being offered character creation. A corrupt one
// is reported by the file reader once and then reads as empty too.
mp_character_card mp_load_character_card( const std::string &server );

// Writes `card`. Returns false when the write failed, which the caller logs --
// a lost card costs the player one menu choice next time, nothing more.
bool mp_save_character_card( const mp_character_card &card );

#endif // CATA_SRC_MP_MP_CHARACTER_CARD_H
