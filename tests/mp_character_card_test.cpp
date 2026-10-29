#include "cata_catch.h"

#include <string>

#include "mp/mp_character_card.h"

// The card is the client's whole memory of a character on a server. It is read
// by the client, which is an ordinary build, so these run in every build.

TEST_CASE( "a_card_survives_a_round_trip", "[mp]" )
{
    mp_character_card card;
    card.server = "10.0.0.4:4321";
    card.world_name = "Falconer";
    card.save_id = "Ada Lovelace";
    card.character_name = "Ada Lovelace";

    REQUIRE( mp_save_character_card( card ) );
    const mp_character_card back = mp_load_character_card( card.server );

    CHECK( back.server == card.server );
    CHECK( back.world_name == card.world_name );
    CHECK( back.save_id == card.save_id );
    CHECK( back.character_name == card.character_name );
    CHECK_FALSE( back.empty() );
}

TEST_CASE( "a_missing_card_reads_as_empty_not_as_an_error", "[mp]" )
{
    // Being offered character creation is the correct outcome of having no
    // card. An error here would turn "first time on this server" into a
    // failure to connect.
    const mp_character_card none = mp_load_character_card( "no.such.host:1" );
    CHECK( none.empty() );
}

#if defined(MULTIPLAYER)
#include "mp/mp_protocol.h"

TEST_CASE( "the_hello_carries_the_save_id", "[mp]" )
{
    mp_control hello;
    hello.kind = mp_control_kind::hello;
    hello.hello.proto = MP_PROTOCOL_VERSION;
    hello.hello.name = "Ada";
    hello.hello.save_id = "Ada Lovelace";
    hello.hello.width = 80;
    hello.hello.height = 24;

    mp_control back;
    REQUIRE( mp_control_from_json( mp_control_to_json( hello ), back ) );
    CHECK( back.hello.save_id == "Ada Lovelace" );
}

TEST_CASE( "the_character_card_message_round_trips", "[mp]" )
{
    mp_control msg;
    msg.kind = mp_control_kind::character_card;
    msg.character_card.world_name = "Falconer";
    msg.character_card.save_id = "Ada Lovelace";
    msg.character_card.character_name = "Ada Lovelace";

    mp_control back;
    REQUIRE( mp_control_from_json( mp_control_to_json( msg ), back ) );
    CHECK( back.kind == mp_control_kind::character_card );
    CHECK( back.character_card.world_name == "Falconer" );
    CHECK( back.character_card.save_id == "Ada Lovelace" );
    CHECK( back.character_card.character_name == "Ada Lovelace" );
}
#endif // MULTIPLAYER
