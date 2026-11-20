#include <filesystem>
#include <fstream>
#include <string>

#include "cata_catch.h"
#include "cata_path.h"
#include "debug.h"

namespace
{

// Removes the scratch tree even when a REQUIRE aborts the test.
class scratch_dir_guard
{
    public:
        explicit scratch_dir_guard( std::filesystem::path dir ) : dir_( std::move( dir ) ) {
            std::filesystem::remove_all( dir_ );
        }
        ~scratch_dir_guard() {
            std::error_code ec;
            std::filesystem::remove_all( dir_, ec );
        }
        scratch_dir_guard( const scratch_dir_guard & ) = delete;
        scratch_dir_guard &operator=( const scratch_dir_guard & ) = delete;
    private:
        std::filesystem::path dir_;
};

} // namespace

TEST_CASE( "debug_log_parent_is_created", "[debug]" )
{
    // The repository root is the test working directory, so this is a scratch
    // tree that never collides with the test's own --user-dir.
    const std::filesystem::path scratch =
        std::filesystem::path( ".data" ) / "debug-log-parent-test";
    const scratch_dir_guard guard( scratch );

    const std::filesystem::path config_dir = scratch / "config";
    const cata_path log_file( cata_path::root_path::unknown, config_dir / "debug.log" );
    REQUIRE_FALSE( std::filesystem::exists( config_dir ) );

    CHECK( ensure_debug_log_parent( log_file ) );
    CHECK( std::filesystem::is_directory( config_dir ) );

    // Creating the directory is only useful if it makes the log openable: this
    // is the exact open DebugFile::init performs on a fresh --userdir.
    std::ofstream log( log_file.generic_u8string(), std::ios::out | std::ios::app );
    REQUIRE( log.good() );
    log << "test\n";
    log.close();
    CHECK( std::filesystem::exists( config_dir / "debug.log" ) );
}
