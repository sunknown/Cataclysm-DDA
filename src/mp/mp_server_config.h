#pragma once
#ifndef CATA_SRC_MP_MP_SERVER_CONFIG_H
#define CATA_SRC_MP_MP_SERVER_CONFIG_H

#include <optional>
#include <string>

// The server's options as they may appear in <config>/mp/server.json. Every
// field is optional: an absent one falls through to the command line, then to
// the shipped default.
struct mp_server_config {
    std::optional<int> port;
    std::optional<std::string> world;
    std::optional<int> players;
    std::optional<int> hurry_ms;
    std::optional<int> hurry_combat_ms;
};

// The same five options as given on the command line. An empty field means
// the operator did not name it, which is what lets a bare --server ask for a
// server and leave the port to the file.
struct cli_server_overrides {
    std::optional<int> port;
    std::optional<std::string> world;
    std::optional<int> players;
    std::optional<int> hurry_ms;
    std::optional<int> hurry_combat_ms;
};

// Reads `path`. A missing file is not a mistake: `out` is left empty and true
// is returned. Unknown keys, malformed JSON and wrong value types are refused
// with `err` set and false -- the server never starts on a half-understood
// file.
bool mp_load_server_config( const std::string &path, mp_server_config &out, std::string &err );

// Merges the command line over the file and leaves the result in port, world
// and players. `cli` wins wherever it names a field. A port or world missing
// from both is refused with `err` set and false.
bool mp_apply_cli_server_config( const mp_server_config &file,
                                 const cli_server_overrides &cli, int &port,
                                 std::string &world, int &players, std::string &err );

#endif // CATA_SRC_MP_MP_SERVER_CONFIG_H
