#include "mp/mp_character_card.h"

#include <ostream>

#include "cata_utility.h"
#include "catacharset.h"
#include "filesystem.h"
#include "flexbuffer_json.h"
#include "json.h"
#include "path_info.h"

static cata_path card_dir()
{
    return PATH_INFO::config_dir_path() / "mp";
}

cata_path mp_card_path( const std::string &server )
{
    // The address is user text ("10.0.0.4:4321"); encoded, it is a file name.
    return card_dir() / ( base64_encode( server ) + ".mpcard.json" );
}

mp_character_card mp_load_character_card( const std::string &server )
{
    mp_character_card card;
    const bool read = read_from_file_optional_json( mp_card_path( server ),
    [&card]( const JsonValue & jv ) {
        const JsonObject jo = jv.get_object();
        jo.allow_omitted_members();
        card.server = jo.get_string( "server", "" );
        card.world_name = jo.get_string( "world", "" );
        card.save_id = jo.get_string( "save_id", "" );
        card.character_name = jo.get_string( "name", "" );
    } );
    if( !read ) {
        return mp_character_card();
    }
    return card;
}

bool mp_save_character_card( const mp_character_card &card )
{
    if( !assure_dir_exist( card_dir() ) ) {
        return false;
    }
    return write_to_file( mp_card_path( card.server ), [&card]( std::ostream & fout ) {
        JsonOut jo( fout, /*pretty_print=*/true );
        jo.start_object();
        jo.member( "server", card.server );
        jo.member( "world", card.world_name );
        jo.member( "save_id", card.save_id );
        jo.member( "name", card.character_name );
        jo.end_object();
    }, "multiplayer character card" );
}
