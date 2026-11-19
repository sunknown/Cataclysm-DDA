/* Main Loop for cataclysm
 * Linux only I guess
 * But maybe not
 * Who knows
 */

// KG: Yes, the above is inaccurate now. It's also a poem, it stays.

// IWYU pragma: no_include <sys/signal.h>
#include <algorithm>
#include <array>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#if defined(_WIN32)
#include "cata_allocator.h"
#include "platform_win.h"
#else
#include <csignal>
#endif

#include <flatbuffers/util.h>

#include "cached_options.h"
#include "cata_allocator.h"
#include "cata_path.h"
#include "color.h"
#include "compatibility.h"
#include "crash.h"
#include "cursesdef.h"
#include "debug.h"
#include "do_turn.h"
#include "event.h"
#include "event_bus.h"
#include "filesystem.h"
#include "game.h"
#include "mp/mp_context.h"
#if defined(MULTIPLAYER)
#include "mp/mp_client_main.h"
#endif
#if defined(MP_SERVER)
#include "avatar.h"
#include "mp/mp_server_config.h"
#include "mp/mp_server_log.h"
#include "mp/mp_server_main.h"
#include "mp/mp_turn.h"
#include "worldfactory.h"
#endif
#include "game_constants.h"
#include "game_ui.h"
#include "get_version.h"
#include "help.h"
#include "input.h"
#include "main_menu.h"
#include "mapsharing.h"
#include "memory_fast.h"
#include "options.h"
#include "ordered_static_globals.h"
#include "output.h"
#include "path_info.h"
#include "rng.h"
#include "system_locale.h"
#include "translations.h"
#include "type_id.h"
#include "ui_manager.h"
#include "cata_imgui.h"
#if defined(MACOSX) || defined(__CYGWIN__)
#   include <unistd.h> // getpid()
#endif

#if defined(EMSCRIPTEN)
#include <emscripten.h>
#endif

#if defined(PREFIX)
#   undef PREFIX
#   include "prefix.h"
#endif

class ui_adaptor;

#if defined(TILES) || defined(SDL_SOUND)
#   include "sdl_version_wrappers.h"
#endif

#if defined(TILES)
#   include "sdltiles.h"
#endif

#if defined(__ANDROID__)
#include <android/log.h>
#include <unistd.h>
#include "sdl_wrappers.h" // for GetAndroidExternalStoragePath(), SDL_main

// Taken from: https://codelab.wordpress.com/2014/11/03/how-to-use-standard-output-streams-for-logging-in-android-apps/
// Force Android standard output to adb logcat output

static int pfd[2];
static pthread_t thr;
static const char *tag = "cdda";

static void *thread_func( void * )
{
    ssize_t rdsz;
    char buf[128];
    for( ;; ) {
        if( ( ( rdsz = read( pfd[0], buf, sizeof buf - 1 ) ) > 0 ) ) {
            if( buf[rdsz - 1] == '\n' ) {
                --rdsz;
            }
            buf[rdsz] = 0;  /* add null-terminator */
            __android_log_write( ANDROID_LOG_DEBUG, tag, buf );
        }
    }
    return 0;
}

int start_logger( const char *app_name )
{
    tag = app_name;

    /* make stdout line-buffered and stderr unbuffered */
    setvbuf( stdout, 0, _IOLBF, 0 );
    setvbuf( stderr, 0, _IONBF, 0 );

    /* create the pipe and redirect stdout and stderr */
    pipe( pfd );
    dup2( pfd[1], 1 );
    dup2( pfd[1], 2 );

    /* spawn the logging thread */
    if( pthread_create( &thr, 0, thread_func, 0 ) == -1 ) {
        return -1;
    }
    pthread_detach( thr );
    return 0;
}

#endif //__ANDROID__

// The game's clean shutdown, installed as the SIGINT handler below and called
// directly at the end of run_game. Declared here so the mp server's exit hook
// (defined after the anonymous namespace) can use it too.
void exit_handler( int s );

namespace
{

#if defined(_WIN32) and defined(TILES)
// Used only if AttachConsole() works
FILE *CONOUT;
#endif
struct arg_handler {
    //! Handler function to be invoked when this argument is encountered. The handler will be
    //! called with the number of parameters after the flag was encountered, along with the array
    //! of following parameters. It must return an integer indicating how many parameters were
    //! consumed by the call or -1 to indicate that a required argument was missing.
    using handler_method = std::function<int ( int, const char ** )>;

    std::string_view flag;  //!< The commandline parameter to handle (e.g., "--seed").
    std::string_view param_documentation;  //!< Human readable description of this arguments parameter.
    std::string_view documentation;  //!< Human readable documentation for this argument.
    std::string_view help_group; //!< Section of the help message in which to include this argument.
    int num_args; //!< How many further arguments are expected for this parameter (usually 0 or 1).
    handler_method handler;  //!< The callback to be invoked when this argument is encountered.
};

template<typename FirstPassArgs, typename SecondPassArgs>
void printHelpMessage( const FirstPassArgs &first_pass_arguments,
                       const SecondPassArgs &second_pass_arguments )
{
    // Group all arguments by help_group.
    std::multimap<std::string, const arg_handler *> help_map;
    for( const arg_handler &handler : first_pass_arguments ) {
        help_map.emplace( handler.help_group, &handler );
    }
    for( const arg_handler &handler : second_pass_arguments ) {
        help_map.emplace( handler.help_group, &handler );
    }

    std::cout << "Command line parameters:\n";
    std::string current_help_group;
    for( std::pair<const std::string, const arg_handler *> &help_entry : help_map ) {
        if( help_entry.first != current_help_group ) {
            current_help_group = help_entry.first;
            std::cout << "\n" << current_help_group << "\n";
        }

        const arg_handler *handler = help_entry.second;
        std::cout << handler->flag << " " << handler->param_documentation;
        if( !handler->documentation.empty() ) {
            std::cout << "\n    " << handler->documentation << "\n";
        }
    }
    std::cout << std::endl;
}

/**
 * Displays current application version and compile options values
 */
void printVersionMessage()
{
#if defined(TILES)
    const bool hasTiles = true;
#else
    const bool hasTiles = false;
#endif

#if defined(SDL_SOUND)
    const bool hasSound = true;
#else
    const bool hasSound = false;
#endif

    printf( "Cataclysm Dark Days Ahead: %s\n\n"
            "%ctiles, %csound\n\n"
            "data dir: %s\nuser dir: %s\n",
            getVersionString(),
            hasTiles ? '+' : '-',
            hasSound ? '+' : '-',
            PATH_INFO::datadir().c_str(),
            PATH_INFO::user_dir().c_str() );
}

void process_args( const char **argv, int argc, const std::vector<arg_handler> &arg_handlers )
{
    while( argc ) {
        bool arg_handled = false;
        for( const arg_handler &handler : arg_handlers ) {
            if( handler.flag == argv[0] ) {
                argc--;
                argv++;
                if( argc < handler.num_args ) {
                    std::cout << "Missing expected argument to command line parameter " << handler.flag << std::endl;
                    std::exit( 1 );
                }
                int args_consumed = handler.handler( argc, argv );
                if( args_consumed < 0 ) {
                    printf( "Failed parsing parameter '%s'\n", *( argv - 1 ) );
                    std::exit( 1 );
                }
                argc -= args_consumed;
                argv += args_consumed;
                arg_handled = true;
                break;
            }
        }
        // Skip other options.
        if( !arg_handled ) {
            --argc;
            ++argv;
        }
    }
}

struct cli_opts {
    int seed = time( nullptr );
    bool verifyexit = false;
    bool noverify = false;
    bool check_mods = false;
    std::vector<std::string> opts;
    // Empty means the option was not named. --world stays optional so the
    // single-player default (open the menu) is still an empty world, while the
    // server can take the name from its config file.
    std::optional<std::string> world;
    bool disable_ascii_art = false;
    // The flag alone asks for a server and leaves the port to the config file;
    // an engaged port is the one the operator named.
    bool server_requested = false;
    std::optional<int> server_port;
    std::optional<int> players;
    std::string connect_to; /** host:port of a multiplayer server to join */
    // Countdown lengths for this server session, in milliseconds. Empty means
    // "leave the shipped default in force".
    std::optional<int> hurry_ms;
    std::optional<int> hurry_combat_ms;
};

cli_opts parse_commandline( int argc, const char **argv )
{
    cli_opts result;

    constexpr std::string_view section_default;
    constexpr std::string_view section_map_sharing = "Map sharing";
    constexpr std::string_view section_user_directory = "User directories";
    constexpr std::string_view section_accessibility = "Accessibility";
    const std::vector<arg_handler> first_pass_arguments = {{
            {
                "--seed", "<string of letters and or numbers>",
                "Sets the random number generator's seed value",
                section_default,
                1,
                [&result]( int, const char **params ) -> int {
                    const unsigned char *hash_input = reinterpret_cast<const unsigned char *>( params[0] );
                    result.seed = djb2_hash( hash_input );
                    return 1;
                }
            },
            {
                "--jsonverify", {},
                "Checks the CDDA json files and exits",
                section_default,
                0,
                [&result]( int, const char ** ) -> int {
                    result.verifyexit = true;
                    return 0;
                }
            },
            {
                "--check-mods", "[mod…]",
                "Checks the json files belonging to given CDDA mod and exits",
                section_default,
                1,
                [&result]( int n, const char **params ) -> int {
                    result.check_mods = true;
                    test_mode = true;
                    for( int i = 0; i < n; ++i )
                    {
                        result.opts.emplace_back( params[ i ] );
                    }
                    return 0;
                }
            },
            {
                "--noverify", {},
                "Skips JSON verification",
                section_default,
                0,
                [&result]( int, const char ** ) -> int {
                    result.noverify = true;
                    return 0;
                }
            },
            {
                "--world", "<name>",
                "Load world",
                section_default,
                0,
                [&result]( int argc, const char **params ) -> int {
                    // The name may be left out so the multiplayer server can
                    // take it from its config file; a token that begins with
                    // '-' is the next option, not a world name.
                    if( argc >= 1 && params[0][0] != '-' ) {
                        result.world = params[0];
                        return 1;
                    }
                    return 0;
                }
            },
            {
                "--basepath", "<path>",
                "Base path for all game data subdirectories",
                section_default,
                1,
                []( int, const char **params )
                {
                    PATH_INFO::init_base_path( params[0] );
                    PATH_INFO::set_standard_filenames();
                    return 1;
                }
            },
            {
                "--shared", {},
                "Activates the map-sharing mode",
                section_map_sharing,
                0,
                []( int, const char ** ) -> int {
                    MAP_SHARING::setSharing( true );
                    MAP_SHARING::setCompetitive( true );
                    MAP_SHARING::setWorldmenu( false );
                    return 0;
                }
            },
            {
                "--username", "<name>",
                "Instructs map-sharing code to use this name for you. In multiplayer it is the "
                "label your client is announced by; it is not your character's name.",
                section_map_sharing,
                1,
                []( int, const char **params ) -> int {
                    MAP_SHARING::setUsername( params[0] );
                    return 1;
                }
            },
            {
                "--addadmin", "<username>",
                "Instructs map-sharing code to use this name for your character and give you "
                "access to the cheat functions.",
                section_map_sharing,
                1,
                []( int, const char **params ) -> int {
                    MAP_SHARING::addAdmin( params[0] );
                    return 1;
                }
            },
            {
                "--adddebugger", "<username>",
                "Informs map-sharing code that you're running inside a debugger",
                section_map_sharing,
                1,
                []( int, const char **params ) -> int {
                    MAP_SHARING::addDebugger( params[0] );
                    return 1;
                }
            },
            {
                "--competitive", {},
                "Instructs map-sharing code to disable access to the in-game cheat functions",
                section_map_sharing,
                0,
                []( int, const char ** ) -> int {
                    MAP_SHARING::setCompetitive( true );
                    return 0;
                }
            },
            {
                "--userdir", "<path>",
                // NOLINTNEXTLINE(cata-text-style): the dot is not a period
                "Base path for user-overrides to files from the ./data directory and named below",
                section_user_directory,
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::init_user_dir( params[0] );
                    PATH_INFO::set_standard_filenames();
                    return 1;
                }
            },
            {
                "--disable-ascii-art", {},
                "Disable aesthetic ascii art in menus and descriptions.",
                section_accessibility,
                0,
                [&result]( int, const char ** ) -> int {
                    result.disable_ascii_art = true;
                    return 0;
                }
            },
            {
                "--server", "<port>",
                "Runs this instance as a multiplayer server that streams its screen to one client. "
                "The port may be omitted and taken from the server config file.",
                section_default,
                0,
                [&result]( int argc, const char **params ) -> int {
                    result.server_requested = true;
                    if( argc >= 1 && params[0][0] != '-' ) {
                        // A malformed number lands on zero, which the config
                        // merge refuses instead of hiding in a default.
                        result.server_port = std::atoi( params[0] );
                        return 1;
                    }
                    // Bare --server: the port comes from the config file.
                    return 0;
                }
            },
            {
                "--players", "<count>",
                "How many clients the multiplayer server waits for before starting. "
                "The number of seats is fixed: a player who leaves frees a seat for "
                "someone else to take, but no seat is added or removed.",
                section_default,
                0,
                [&result]( int argc, const char **params ) -> int {
                    // A malformed or absent count leaves the file (then the
                    // single-client default) in charge.
                    if( argc >= 1 && params[0][0] != '-' ) {
                        result.players = std::max( 1, std::atoi( params[0] ) );
                        return 1;
                    }
                    return 0;
                }
            },
            {
                "--connect", "<host:port>",
                "Connects to a multiplayer server and plays as its client.",
                section_default,
                1,
                [&result]( int, const char **params ) -> int {
                    result.connect_to = params[0];
                    return 1;
                }
            },
            {
                "--hurry-ms", "<milliseconds>",
                "How long, in milliseconds, the party waits for a player who is reading, "
                "1000-600000",
                section_default,
                1,
                [&result]( int, const char **params ) -> int {
                    // Malformed input lands on zero, which the range check
                    // refuses rather than hiding in a default.
                    result.hurry_ms = std::atoi( params[0] );
                    return 1;
                }
            },
            {
                "--hurry-combat-ms", "<milliseconds>",
                "Floor, in milliseconds, for a turn while a hostile can see a player and nobody "
                "is playing by hand (default 4000), 1000-600000",
                section_default,
                1,
                [&result]( int, const char **params ) -> int {
                    result.hurry_combat_ms = std::atoi( params[0] );
                    return 1;
                }
            }
        }
    };

    // The following arguments are dependent on one or more of the previous flags and are run
    // in a second pass.
    const std::vector<arg_handler> second_pass_arguments = {{
            {
                "--worldmenu", {},
                "Enables the world menu in the map-sharing code",
                section_map_sharing,
                0,
                []( int, const char ** ) -> int {
                    MAP_SHARING::setWorldmenu( true );
                    return true;
                }
            },
            {
                "--datadir", "<directory name>",
                "Sub directory from which game data is loaded",
                {},
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::set_datadir( params[0] );
                    return 1;
                }
            },
            {
                "--savedir", "<directory name>",
                "Subdirectory for game saves",
                section_user_directory,
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::set_savedir( params[0] );
                    return 1;
                }
            },
            {
                "--configdir", "<directory name>",
                "Subdirectory for game configuration",
                section_user_directory,
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::set_config_dir( params[0] );
                    return 1;
                }
            },
            {
                "--memorialdir", "<directory name>",
                "Subdirectory for memorials",
                section_user_directory,
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::set_memorialdir( params[0] );
                    return 1;
                }
            },
            {
                "--optionfile", "<filename>",
                "Name of the options file within the configdir",
                section_user_directory,
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::set_options( params[0] );
                    return 1;
                }
            },
            {
                "--keymapfile", "<filename>",
                "Name of the keymap file within the configdir",
                section_user_directory,
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::set_keymap( params[0] );
                    return 1;
                }
            },
            {
                "--autopickupfile", "<filename>",
                "Name of the autopickup options file within the configdir",
                {},
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::set_autopickup( params[0] );
                    return 1;
                }
            },
            {
                "--motdfile", "<filename>",
                "Name of the message of the day file within the motd directory",
                {},
                1,
                []( int, const char **params ) -> int {
                    PATH_INFO::set_motd( params[0] );
                    return 1;
                }
            },
        }
    };

    if( std::count( argv, argv + argc, std::string( "--help" ) ) ) {
        printHelpMessage( first_pass_arguments, second_pass_arguments );
        std::exit( 0 );
    }

    if( std::count( argv, argv + argc, std::string( "--version" ) ) ) {
        printVersionMessage();
        std::exit( 0 );
    }

    // skip program name
    --argc;
    ++argv;

    process_args( argv, argc, first_pass_arguments );
    process_args( argv, argc, second_pass_arguments );

    return result;
}

bool assure_essential_dirs_exist()
{
    using namespace PATH_INFO;
    std::vector<std::string> essential_paths{
        config_dir(),
        savedir(),
        templatedir(),
        user_font(),
        user_sound().get_unrelative_path().u8string(),
        user_gfx().get_unrelative_path().u8string()
    };
    for( const std::string &path : essential_paths ) {
        if( !assure_dir_exist( path ) ) {
            popup( _( "Unable to make directory \"%s\".  Check permissions." ), path );
            return false;
        }
    }
    return true;
}

#if defined(MP_SERVER)
// The server's whole game: no main menu, the world named on the command line,
// and every client choosing their survivor in their own join menu. One session,
// then the process stops -- the roster is fixed, so there is nobody left to
// play a second one with.
static void run_server_session( const std::string &world_name, const std::optional<int> &hurry_ms,
                                const std::optional<int> &hurry_combat_ms )
{
    if( world_name.empty() ) {
        mp_server_log( "no world given: start the server with --world <name>" );
        return;
    }
    // Before the world: a refused length is the operator's mistake, and the
    // session should run on the defaults rather than let it surface later.
    // An unset flag is still at its default here -- nothing has configured a
    // session yet -- so filling it in is exactly "leave it as it is".
    const int manual_ms = hurry_ms.value_or( MP_HURRY_MS );
    const int combat_ms = hurry_combat_ms.value_or( MP_HURRY_COMBAT_MS );
    const bool lengths_accepted = mp_set_hurry_lengths( manual_ms, combat_ms );
    if( !lengths_accepted ) {
        // The setter already logged the detail to the debug log; this is the
        // line an operator watching the console will actually see.
        mp_server_log( "hurry lengths refused: manual " + std::to_string( manual_ms ) +
                       " ms, combat " + std::to_string( combat_ms ) +
                       " ms; running on the defaults" );
    }
    std::string err;
    if( !mp_server_open_world( world_name, err ) ) {
        mp_server_log( "cannot start the session: " + err );
        // Tell the clients why before the wires close.
        mp_request_session_end( err );
        mp_server_end_session();
        return;
    }
    if( lengths_accepted ) {
        // Opening the world starts the player threads, and the session-start
        // reset they run restores the shipped defaults. The operator's lengths
        // belong to the session, not to the turn, so they go back on here, once
        // the world is up and before any turn can use them.
        mp_set_hurry_lengths( manual_ms, combat_ms );
    }
    {
        shared_ptr_fast<ui_adaptor> ui = g->create_or_get_main_ui_adaptor();
        get_event_bus().send<event_type::game_begin>( getVersionString() );
        mp_server_log( "game started in world \"" + world_generator->active_world->world_name +
                       "\"" );
        while( !g->do_turn() ) {
        }
        mp_server_log( "game ended" );
        // Stops the threads and leaves the process on player 0's context, the
        // stack the main screen above registered in and must die in.
        mp_server_end_session();
    }
}
#endif

// Runs the game proper: everything after the curses interface is up. Both the
// single-player path and the multiplayer server reach this, so the ~120 lines
// of startup and the main-menu loop are not duplicated (src/mp/mp_server_main.cpp
// hands off to it through main()'s MP_SERVER fork).
static int run_game( cli_opts &cli )
{
    set_language_from_options();

    rng_set_engine_seed( cli.seed );

    game_ui::init_ui();

    g = std::make_unique<game>();

    // The context must exist before the main menu runs: the menu's ui_adaptors
    // register into the active stack at construction time, and game::setup()
    // (called from the menu when a new game starts or a save loads) switches
    // that stack to the context's. Ensuring the context here keeps those
    // adaptors under the same stack for their whole lifetime instead of
    // orphaning them mid-destruction (cata_assert owner == active_stack).
    // It is also what makes the server's input queue (which lives on the
    // context) reachable before setup().
    mp_ensure_player_context();

    // First load and initialize everything that does not
    // depend on the mods.
    try {
        g->load_static_data();
        if( cli.verifyexit ) {
            exit_handler( 0 );
        }
        if( cli.check_mods ) {
            init_colors();
            const std::vector<mod_id> mods( cli.opts.begin(), cli.opts.end() );
            exit( g->check_mod_data( mods ) && !debug_has_error_been_observed() ? 0 : 1 );
        }
    } catch( const std::exception &err ) {
        debugmsg( "%s", err.what() );
        exit_handler( -999 );
    }

    // Load the colors of ImGui to match the colors set by the user.
    cataimgui::init_colors();

    // set decimal point for float input widgets
    // uses system locale, because that's what imgui uses to parse and display floats
    ImGui::GetPlatformIO().Platform_LocaleDecimalPoint =
        static_cast<unsigned char>( *localeconv()->decimal_point );

    // Override existing settings from cli  options
    if( cli.disable_ascii_art ) {
        get_options().get_option( "ENABLE_ASCII_ART" ).setValue( "false" );
        get_options().get_option( "ENABLE_ASCII_TITLE" ).setValue( "false" );
    }

    if( cli.noverify ) {
        get_options().get_option( "SKIP_VERIFICATION" ).setValue( "true" );
    }

    // Now we do the actual game.

#if defined(DEBUG_CURSES_CURSOR)
    catacurses::curs_set( 2 );
#else
    // I have no clue what this comment is on about
    // Any value works well enough for debugging at least
    catacurses::curs_set( 0 ); // Invisible cursor here, because MAPBUFFER.load() is crash-prone
#endif

#if !defined(_WIN32)
    struct sigaction sigIntHandler;
    sigIntHandler.sa_handler = exit_handler;
    sigemptyset( &sigIntHandler.sa_mask );
    sigIntHandler.sa_flags = 0;
    sigaction( SIGINT, &sigIntHandler, nullptr );
#endif

    if( !assure_essential_dirs_exist() ) {
        exit_handler( -999 );
        return 0;
    }

#if defined(LOCALIZE)
    if( get_option<std::string>( "USE_LANG" ).empty() && !SystemLocale::Language().has_value() ) {
#if defined(TILES)
        display_buffer_draw_scope draw_scope;
        if( !display_buffer_scope_is_invalid() ) {
#endif
            imclient->new_frame(); // we have to prime the pump, because of reasons
            imclient->end_frame();
            const std::string lang = select_language();
            get_options().get_option( "USE_LANG" ).setValue( lang );
            set_language_from_options();
#if defined(TILES)
        }
#endif
    }
#endif
    replay_buffered_debugmsg_prompts();

#if defined(MP_SERVER)
    if( cli.server_requested ) {
        run_server_session( cli.world.value_or( std::string() ), cli.hurry_ms,
                            cli.hurry_combat_ms );
        exit_handler( -999 );
        return 0;
    }
#endif

    main_menu::queued_world_to_load = cli.world.value_or( std::string() );

    while( true ) {
        main_menu menu;
        if( !menu.opening_screen() ) {
            break;
        }

        shared_ptr_fast<ui_adaptor> ui = g->create_or_get_main_ui_adaptor();
        get_event_bus().send<event_type::game_begin>( getVersionString() );
        while( true ) {
            if( g->do_turn() ) {
                break;
            }
        }
    }

    exit_handler( -999 );
    return 0;
}

}  // namespace

void exit_handler( int s )
{
    const int old_timeout = inp_mngr.get_timeout();
    inp_mngr.reset_timeout();
    if( s != 2 || query_yn( _( "Really Quit?  All unsaved changes will be lost." ) ) ) {
        // Before anything is torn down: g.reset() and the static destructors
        // that run after exit() can still raise debugmsgs, and the interactive
        // prompt is no longer safe to open once imclient is gone.
#if defined(MP_SERVER)
        // Before deinitDebug(): the log line still has a debug log to mirror
        // itself into, and stderr is the last thing the operator will see.
        mp_server_log( "server stopped" );
#endif
        debug_set_shutting_down();
        deinitDebug();

        int exit_status = 0;
        g.reset();

        catacurses::endwin();

#if defined(__ANDROID__)
        // Avoid capturing SIGABRT on exit on Android in crash report
        // Can be removed once the SIGABRT on exit problem is fixed
        signal( SIGABRT, SIG_DFL );
#endif

        imclient.reset();
        exit( exit_status );
    }
    inp_mngr.set_timeout( old_timeout );
    ui_manager::redraw_invalidated();
    catacurses::doupdate();
}

#if defined(MP_SERVER)
void mp_server_request_exit()
{
    // The connected client is gone. Run the same clean shutdown the rest of
    // the game uses; the mp backend calls this from the input path, so the
    // game logic itself never learns about the transport.
    exit_handler( 0 );
}
#endif

#if defined(EMSCRIPTEN)
EM_ASYNC_JS( void, mount_idbfs, (), {
    console.log( "Mounting IDBFS for persistence..." );
    FS.mkdir( '/home/web_user/.cataclysm-dda' );
    FS.mount( IDBFS, {}, '/home/web_user/.cataclysm-dda' );
    await new Promise( function( resolve, reject )
    {
        FS.syncfs( true, function( err ) {
            if( err ) {
                reject( err );
            } else {
                console.log( "Successfully mounted IDBFS." );
                resolve();
            }
        } );
    } );

    let fsNeedsSync = false;
    window.setFsNeedsSync = function setFsNeedsSync()
    {
        if( !fsNeedsSync ) {
            requestAnimationFrame( syncFs );
        }
        fsNeedsSync = true;
    };

    function syncFs()
    {
        console.log( "Persisting to IDBFS..." );
        FS.syncfs( false, function( err ) {
            fsNeedsSync = false;
            if( err ) {
                console.error( err );
            } else {
                console.log( "Successfully persisted to IDBFS..." );
            }
        } );
    }
} );
#endif

#if defined(USE_WINMAIN)
int APIENTRY WinMain( _In_ HINSTANCE /* hInstance */, _In_opt_ HINSTANCE /* hPrevInstance */,
                      _In_ LPSTR /* lpCmdLine */, _In_ int /* nCmdShow */ )
{
    int argc = __argc;
    char **argv = __argv;
#elif defined(__ANDROID__)
extern "C" int SDL_main( int argc, char **argv ) {
#else
int main( int argc, const char *argv[] )
{
#endif

    cata::init_allocator();

    ordered_static_globals();
    init_crash_handlers();
    reset_floating_point_mode();
#if defined(FLATBUFFERS_LOCALE_INDEPENDENT) && (FLATBUFFERS_LOCALE_INDEPENDENT > 0)
    flatbuffers::ClassicLocale::Get();
#endif

#if defined(EMSCRIPTEN)
    mount_idbfs();
#endif

    on_out_of_scope json_member_reporting_guard{ [] {
            // Disable reporting unvisited members if stack unwinding leaves main early.
            Json::globally_report_unvisited_members( false );
        } };

#if defined(_WIN32) and defined(TILES)
    const HANDLE std_output { GetStdHandle( STD_OUTPUT_HANDLE ) }, std_error { GetStdHandle( STD_ERROR_HANDLE ) };
    if( std_output != INVALID_HANDLE_VALUE and std_error != INVALID_HANDLE_VALUE ) {
        if( AttachConsole( ATTACH_PARENT_PROCESS ) ) {
            if( std_output == nullptr ) {
                freopen_s( &CONOUT, "CONOUT$", "w", stdout );
            }
            if( std_error == nullptr ) {
                freopen_s( &CONOUT, "CONOUT$", "w", stderr );
            }
        }
    }
#endif
#if defined(__ANDROID__)
    // Start the standard output logging redirector
    start_logger( "cdda" );

    // On Android first launch, we copy all data files from the APK into the app's writeable folder so std::io stuff works.
    // Use the external storage so it's publicly modifiable data (so users can mess with installed data, save games etc.)
    std::string external_storage_path( GetAndroidExternalStoragePath() );

    PATH_INFO::init_base_path( external_storage_path );
#else
    // Set default file paths
#if defined(PREFIX)
    PATH_INFO::init_base_path( std::string( PREFIX ) );
#else
    PATH_INFO::init_base_path( "" );
#endif
#endif

#if defined(__ANDROID__)
    PATH_INFO::init_user_dir( external_storage_path );
#else
#   if defined(USE_HOME_DIR) || defined(USE_XDG_DIR) || defined(EMSCRIPTEN)
    PATH_INFO::init_user_dir( "" );
#   else
    PATH_INFO::init_user_dir( "." );
#   endif
#endif
    PATH_INFO::set_standard_filenames();

    MAP_SHARING::setDefaults();

    cli_opts cli = parse_commandline( argc, const_cast<const char **>( argv ) );

    if( !dir_exist( PATH_INFO::datadir() ) ) {
        printf( "Fatal: Can't find data directory \"%s\"\nPlease ensure the current working directory is correct or specify data directory with --datadir.  Perhaps you meant to start \"cataclysm-launcher\"?\n",
                PATH_INFO::datadir().c_str() );
        exit( 1 );
    }

    if( !assure_dir_exist( PATH_INFO::user_dir() ) ) {
        printf( "Can't open or create %s. Check permissions.\n",
                PATH_INFO::user_dir().c_str() );
        exit( 1 );
    }

#if defined(EMSCRIPTEN)
    setupDebug( DebugOutput::std_err );
#else
    setupDebug( DebugOutput::file );
#endif
    // NOLINTNEXTLINE(cata-tests-must-restore-global-state)
    json_error_output_colors = json_error_output_colors_t::color_tags;

    /**
     * OS X does not populate locale env vars correctly (they usually default to
     * "C") so don't bother trying to set the locale based on them.
     */
#if !defined(MACOSX)
    if( setlocale( LC_ALL, "" ) == nullptr ) {
        DebugLog( D_WARNING, D_MAIN ) << "Error while setlocale(LC_ALL, '').";
    } else {
#endif
        try {
            std::locale::global( std::locale( "" ) );
        } catch( const std::exception & ) {
            // if user default locale retrieval isn't implemented by system
            try {
                // default to basic C locale
                std::locale::global( std::locale::classic() );
            } catch( const std::exception &err ) {
                debugmsg( "%s", err.what() );
                exit_handler( -999 );
            }
        }
#if !defined(MACOSX)
    }
#endif

    DebugLog( D_INFO, DC_ALL ) << "[main] C locale set to " << setlocale( LC_ALL, nullptr );
    DebugLog( D_INFO, DC_ALL ) << "[main] C++ locale set to " << std::locale().name();

#if defined(TILES) || defined(SDL_SOUND)
    {
        const SDLVersionInfo compiled = GetCompiledSDLVersion();
        DebugLog( D_INFO, DC_ALL ) << "SDL version used during compile is "
                                   << compiled.major << "."
                                   << compiled.minor << "."
                                   << compiled.patch;

        const SDLVersionInfo linked = GetLinkedSDLVersion();
        DebugLog( D_INFO, DC_ALL ) << "SDL version used during linking and in runtime is "
                                   << linked.major << "."
                                   << linked.minor << "."
                                   << linked.patch;
    }
#endif

#if !defined(TILES)
    get_options().init();
    get_options().load();
#endif

    // --server and --connect are always accepted by the argument parser, but
    // the code behind them is compiled in only for the builds that carry it.
    // Say so and stop: silently starting an ordinary single-player game instead
    // looks exactly like a multiplayer session that refuses to connect, and the
    // mistake is impossible to spot from the outside.
#if !defined(MP_SERVER)
    if( cli.server_requested ) {
        std::cerr << "This build has no multiplayer server: rebuild with MP_SERVER=1 "
                  "to use --server." << std::endl;
        return 1;
    }
#endif
#if !defined(MULTIPLAYER)
    if( !cli.connect_to.empty() ) {
        std::cerr << "This build has no multiplayer client: rebuild with MULTIPLAYER=1 "
                  "to use --connect." << std::endl;
        return 1;
    }
#endif

#if defined(MP_SERVER)
    // The server's options are one resolved set by the time anything listens:
    // the command line over the config file, the file over the built-in
    // defaults. A bad file, or a port or world nobody named, is the operator's
    // mistake, and the server refuses to start rather than guess -- falling
    // through to a single-player game would look like a server that silently
    // ignored the flags.
    if( cli.server_requested ) {
        mp_server_log_enable();
        const std::string config_path = PATH_INFO::config_dir() + "mp/server.json";
        mp_server_config from_file;
        std::string err;
        if( !mp_load_server_config( config_path, from_file, err ) ) {
            mp_server_log( "refusing to start: " + err );
            return 1;
        }
        cli_server_overrides overrides;
        overrides.port = cli.server_port;
        overrides.world = cli.world;
        overrides.players = cli.players;
        overrides.hurry_ms = cli.hurry_ms;
        overrides.hurry_combat_ms = cli.hurry_combat_ms;
        int port = 0;
        std::string world;
        int players = 1;
        if( !mp_apply_cli_server_config( from_file, overrides, port, world, players, err ) ) {
            mp_server_log( "refusing to start: " + err );
            return 1;
        }
        cli.server_port = port;
        cli.world = world;
        cli.players = players;
        // The countdown lengths are not part of the resolved trio above: the
        // setter owns their range check, so the parser only has to pick which
        // source named them.
        cli.hurry_ms = overrides.hurry_ms ? overrides.hurry_ms : from_file.hurry_ms;
        cli.hurry_combat_ms = overrides.hurry_combat_ms ? overrides.hurry_combat_ms :
                              from_file.hurry_combat_ms;
    }
    if( cli.server_requested &&
        mp_server_main( *cli.server_port, *cli.world, *cli.players ) != 0 ) {
        return 1;
    }
#endif

#if defined(MULTIPLAYER)
    if( !cli.connect_to.empty() ) {
        return mp_client_main( cli.connect_to );
    }
#endif

    // in test mode don't initialize curses to avoid escape sequences being inserted into output stream,
    // except for a multiplayer server, whose curses backend is a buffer that needs setup either way
    if( !test_mode || cli.server_requested ) {
        try {
            // set minimum FULL_SCREEN sizes
            FULL_SCREEN_WIDTH = EVEN_MINIMUM_TERM_WIDTH;
            FULL_SCREEN_HEIGHT = EVEN_MINIMUM_TERM_HEIGHT;
            catacurses::init_interface();
        } catch( const std::exception &err ) {
            // can't use any curses function as it has not been initialized
            std::cerr << "Error while initializing the interface: " << err.what() << std::endl;
            DebugLog( D_ERROR, DC_ALL ) << "Error while initializing the interface: " << err.what() << "\n";
            return 1;
        }
    } else if( cli.check_mods ) {
        get_options().init();
        get_options().load();
    }

    return run_game( cli );
}
