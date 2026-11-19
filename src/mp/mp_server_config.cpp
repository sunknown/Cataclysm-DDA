#include "mp/mp_server_config.h"

#include <algorithm>
#include <array>
#include <sstream>

#include "filesystem.h"
#include "json.h"
#include "json_error.h"

namespace
{

constexpr std::array<const char *, 5> known_keys = { {
        "port", "world", "players", "hurry-ms", "hurry-combat-ms"
    }
};

bool is_known_key( const std::string &name )
{
    for( const char *key : known_keys ) {
        if( name == key ) {
            return true;
        }
    }
    return false;
}

std::optional<int> read_int( const TextJsonObject &jo, const std::string &name )
{
    if( !jo.has_member( name ) ) {
        return std::nullopt;
    }
    if( !jo.has_int( name ) ) {
        jo.throw_error_at( name, "expected an integer" );
    }
    return jo.get_int( name );
}

std::optional<std::string> read_string( const TextJsonObject &jo, const std::string &name )
{
    if( !jo.has_member( name ) ) {
        return std::nullopt;
    }
    if( !jo.has_string( name ) ) {
        jo.throw_error_at( name, "expected a string" );
    }
    return jo.get_string( name );
}

} // namespace

bool mp_load_server_config( const std::string &path, mp_server_config &out, std::string &err )
{
    out = mp_server_config();
    err.clear();
    if( !file_exist( path ) ) {
        return true;
    }
    try {
        std::istringstream stream( read_entire_file( path ) );
        TextJsonIn jsin( stream, path );
        TextJsonObject jo = jsin.get_object();
        // Every member has to be one the server understands: a typo in an
        // option name must be an error, not a silently ignored setting.
        for( const TextJsonMember &member : jo ) {
            if( !is_known_key( member.name() ) ) {
                jo.throw_error_at( member.name(), "unknown server option" );
            }
        }
        mp_server_config result;
        result.port = read_int( jo, "port" );
        result.world = read_string( jo, "world" );
        result.players = read_int( jo, "players" );
        result.hurry_ms = read_int( jo, "hurry-ms" );
        result.hurry_combat_ms = read_int( jo, "hurry-combat-ms" );
        out = result;
        return true;
    } catch( const JsonError &e ) {
        err = "cannot read server config \"" + path + "\": " + e.what();
        out = mp_server_config();
        return false;
    }
}

bool mp_apply_cli_server_config( const mp_server_config &file,
                                 const cli_server_overrides &cli, int &port,
                                 std::string &world, int &players, std::string &err )
{
    err.clear();
    const std::optional<int> merged_port = cli.port ? cli.port : file.port;
    const std::optional<std::string> merged_world = cli.world ? cli.world : file.world;
    const std::optional<int> merged_players = cli.players ? cli.players : file.players;
    if( !merged_port || *merged_port <= 0 ) {
        err = "no server port: give --server <port> or set \"port\" in the server config";
        return false;
    }
    if( !merged_world || merged_world->empty() ) {
        err = "no world: give --world <name> or set \"world\" in the server config";
        return false;
    }
    port = *merged_port;
    world = *merged_world;
    players = std::max( 1, merged_players.value_or( 1 ) );
    return true;
}
