#include "cata_catch.h"

#if defined(MULTIPLAYER)

#include <string>

#include "mp/mp_server_config.h"

TEST_CASE( "server_config_reads_every_key", "[mp]" )
{
    mp_server_config cfg;
    std::string err;
    REQUIRE( mp_load_server_config( "tests/data/mp/server-config-ok.json", cfg, err ) );
    CHECK( cfg.port.value() == 4321 );
    CHECK( cfg.world.value() == "coop" );
    CHECK( cfg.players.value() == 2 );
    CHECK( cfg.hurry_ms.value() == 5000 );
    CHECK( cfg.hurry_combat_ms.value() == 2000 );
}

TEST_CASE( "server_config_refuses_unknown_keys_and_bad_json", "[mp]" )
{
    mp_server_config cfg;
    std::string err;
    CHECK_FALSE( mp_load_server_config( "tests/data/mp/server-config-unknown.json", cfg, err ) );
    CHECK_FALSE( err.empty() );
    CHECK_FALSE( mp_load_server_config( "tests/data/mp/server-config-bad.json", cfg, err ) );
}

TEST_CASE( "cli_overrides_the_config_file", "[mp]" )
{
    mp_server_config file;
    file.port = 4000;
    file.world = "file-world";
    file.players = 3;
    cli_server_overrides cli;
    cli.port = 5000;
    cli.world = "cli-world";
    int port = 0;
    std::string world;
    int players = 0;
    std::string err;
    REQUIRE( mp_apply_cli_server_config( file, cli, port, world, players, err ) );
    CHECK( port == 5000 );
    CHECK( world == "cli-world" );
    CHECK( players == 3 );
}

TEST_CASE( "server_config_refuses_missing_port_or_world", "[mp]" )
{
    mp_server_config file;
    cli_server_overrides cli;
    int port = 0;
    std::string world;
    int players = 1;
    std::string err;
    CHECK_FALSE( mp_apply_cli_server_config( file, cli, port, world, players, err ) );
    CHECK_FALSE( err.empty() );
}

#endif // MULTIPLAYER
