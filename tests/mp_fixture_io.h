#pragma once
#ifndef CATA_TESTS_MP_FIXTURE_IO_H
#define CATA_TESTS_MP_FIXTURE_IO_H

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Opt-in wire-fixture export for the 3D client's tests. MPSERVER_FIXTURE_DIR
// names the directory the bodies are written to; when it is unset the export
// test returns immediately and never touches the filesystem.
inline std::optional<std::filesystem::path> mp_fixture_dir()
{
    const char *dir = std::getenv( "MPSERVER_FIXTURE_DIR" );
    if( dir == nullptr || *dir == '\0' ) {
        return std::nullopt;
    }
    return std::filesystem::path( dir );
}

// Writes one fixture body verbatim. The file holds the wire bytes only, with
// no 4-byte frame prefix. Throws when the file cannot be written: an export
// that silently did nothing would let the client's expectations drift.
inline void mp_fixture_write( const std::filesystem::path &dir,
                              const std::string &name,
                              const std::vector<uint8_t> &bytes )
{
    const std::filesystem::path path = dir / name;
    std::ofstream out( path, std::ios::binary | std::ios::trunc );
    if( !out ) {
        throw std::runtime_error( "cannot open fixture file " + path.string() );
    }
    out.write( reinterpret_cast<const char *>( bytes.data() ),
               static_cast<std::streamsize>( bytes.size() ) );
    out.close();
    if( !out ) {
        throw std::runtime_error( "cannot write fixture file " + path.string() );
    }
}

#endif // CATA_TESTS_MP_FIXTURE_IO_H
