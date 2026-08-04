#include "mp/mp_server_log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <optional>
#include <string>

#if defined(_WIN32)
#include "platform_win.h"
#else
#include <sys/time.h>
#endif

#include "debug.h"

namespace
{

bool g_enabled = false;
std::optional<std::chrono::steady_clock::time_point> g_connected_at;

// "[HH:MM:SS] ", on the wall clock the operator's own shell prompt shows. The
// game's debug log stamps its lines the same way (debug.cpp, get_time), so a
// console line and its debug.log twin can be lined up by eye.
std::string timestamp()
{
    int hour = 0;
    int minute = 0;
    int second = 0;
#if defined(_WIN32)
    SYSTEMTIME time = {};
    GetLocalTime( &time );
    hour = time.wHour;
    minute = time.wMinute;
    second = time.wSecond;
#else
    const std::time_t tt = std::time( nullptr );
    std::tm local = {};
    localtime_r( &tt, &local );
    hour = local.tm_hour;
    minute = local.tm_min;
    second = local.tm_sec;
#endif
    char buf[16];
    std::snprintf( buf, sizeof( buf ), "[%02d:%02d:%02d] ", hour, minute, second );
    return std::string( buf );
}

} // namespace

void mp_server_log_enable()
{
    g_enabled = true;
}

bool mp_server_log_enabled()
{
    return g_enabled;
}

void mp_server_log( const std::string &line )
{
    // The debug log gets the line either way: it costs nothing, and an
    // operator who kept a debug.log should find the same session there.
    DebugLog( D_INFO, D_MAIN ) << "mp server: " << line;
    if( !g_enabled ) {
        return;
    }
    // stderr rather than stdout: it is unbuffered, so a line written just
    // before the process exits still reaches the console, and a redirected
    // stdout never swallows the log.
    std::cerr << timestamp() << "mp: " << line << std::endl;
}

void mp_server_log_mark_connected()
{
    g_connected_at = std::chrono::steady_clock::now();
}

std::string mp_server_log_session_length()
{
    if( !g_connected_at ) {
        return std::string();
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now() - *g_connected_at ).count();
    const long long hours = elapsed / 3600;
    const long long minutes = ( elapsed % 3600 ) / 60;
    const long long seconds = elapsed % 60;
    char buf[32];
    if( hours > 0 ) {
        std::snprintf( buf, sizeof( buf ), "%lldh%02lldm%02llds", hours, minutes, seconds );
    } else if( minutes > 0 ) {
        std::snprintf( buf, sizeof( buf ), "%lldm%02llds", minutes, seconds );
    } else {
        std::snprintf( buf, sizeof( buf ), "%llds", seconds );
    }
    return std::string( buf );
}

void mp_server_log_reset_for_test()
{
    g_enabled = false;
    g_connected_at.reset();
}
