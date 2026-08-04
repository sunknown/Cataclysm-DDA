#include "mp/mp_socket.h"

#include <cerrno>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace
{

bool mp_socket_interrupted()
{
#ifdef _WIN32
    return ::WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

bool mp_socket_would_block()
{
#ifdef _WIN32
    const int e = ::WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
#if EAGAIN == EWOULDBLOCK
    return errno == EAGAIN;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
#endif
}

// Suppresses SIGPIPE for sends on `fd`. Linux does it per send with
// MSG_NOSIGNAL (see mp_socket_send); the BSDs disable the signal once at setup
// with SO_NOSIGPIPE; Windows never raises SIGPIPE at all.
void mp_socket_suppress_sigpipe( const mp_socket_t fd )
{
#if defined(__APPLE__) || defined(__MACH__)
    const int one = 1;
    ::setsockopt( fd, SOL_SOCKET, SO_NOSIGPIPE,
                  reinterpret_cast<const char *>( &one ), sizeof( one ) );
#else
    // Linux suppresses SIGPIPE per send via MSG_NOSIGNAL (mp_socket_send);
    // Windows never raises SIGPIPE at all. Nothing to set up here.
    ( void )fd;
#endif
}

} // namespace

void mp_socket_startup()
{
#ifdef _WIN32
    static bool started = false;
    if( !started ) {
        WSADATA data;
        if( ::WSAStartup( MAKEWORD( 2, 2 ), &data ) == 0 ) {
            started = true;
        }
    }
#endif
}

mp_socket_t mp_socket_create_tcp()
{
    mp_socket_startup();
#ifdef _WIN32
    const mp_socket_t fd = ::socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
#else
    const mp_socket_t fd = ::socket( AF_INET, SOCK_STREAM, 0 );
#endif
    if( fd != mp_socket_invalid ) {
        mp_socket_suppress_sigpipe( fd );
    }
    return fd;
}

void mp_socket_close( const mp_socket_t fd )
{
    if( fd == mp_socket_invalid ) {
        return;
    }
#ifdef _WIN32
    ::closesocket( fd );
#else
    ::close( fd );
#endif
}

bool mp_socket_set_nonblocking( const mp_socket_t fd )
{
#ifdef _WIN32
    u_long mode = 1;
    return ::ioctlsocket( fd, FIONBIO, &mode ) == 0;
#else
    const int flags = ::fcntl( fd, F_GETFL, 0 );
    if( flags < 0 ) {
        return false;
    }
    return ::fcntl( fd, F_SETFL, flags | O_NONBLOCK ) == 0;
#endif
}

bool mp_socket_set_nodelay( const mp_socket_t fd )
{
    const int one = 1;
    return ::setsockopt( fd, IPPROTO_TCP, TCP_NODELAY,
                         reinterpret_cast<const char *>( &one ), sizeof( one ) ) == 0;
}

bool mp_socket_bind( const mp_socket_t fd, const int port, std::string &err )
{
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons( static_cast<uint16_t>( port ) );
    if( ::bind( fd, reinterpret_cast<const sockaddr *>( &addr ), sizeof( addr ) ) < 0 ) {
        err = "bind(port " + std::to_string( port ) + ") failed: " + mp_socket_last_error_string();
        return false;
    }
    return true;
}

bool mp_socket_listen( const mp_socket_t fd, const int backlog )
{
    return ::listen( fd, backlog ) == 0;
}

int mp_socket_local_port( const mp_socket_t fd )
{
    sockaddr_in addr = {};
#ifdef _WIN32
    int addr_len = sizeof( addr );
#else
    socklen_t addr_len = sizeof( addr );
#endif
    if( ::getsockname( fd, reinterpret_cast<sockaddr *>( &addr ), &addr_len ) < 0 ) {
        return 0;
    }
    return static_cast<int>( ntohs( addr.sin_port ) );
}

mp_socket_t mp_socket_accept( const mp_socket_t fd )
{
    return ::accept( fd, nullptr, nullptr );
}

bool mp_socket_connect( const mp_socket_t fd, const std::string &host, const int port,
                        std::string &err )
{
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *resolved = nullptr;
    const std::string port_str = std::to_string( port );
    const int gai = ::getaddrinfo( host.c_str(), port_str.c_str(), &hints, &resolved );
    if( gai != 0 ) {
        err = "cannot resolve '" + host + "': " + ::gai_strerror( gai );
        return false;
    }

#ifdef _WIN32
    const int rc = ::connect( fd, resolved->ai_addr, static_cast<int>( resolved->ai_addrlen ) );
#else
    const int rc = ::connect( fd, resolved->ai_addr, resolved->ai_addrlen );
#endif
    ::freeaddrinfo( resolved );

    if( rc == 0 ) {
        return true;
    }

    // A non-blocking connect completes asynchronously: poll until the
    // handshake is done, then read the outcome from SO_ERROR.
    bool in_progress = false;
#ifdef _WIN32
    const int e = ::WSAGetLastError();
    in_progress = e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEINTR;
#else
    const int e = errno;
    in_progress = e == EINPROGRESS || e == EWOULDBLOCK || e == EAGAIN ||
                  e == EALREADY || e == EINTR;
#endif
    if( !in_progress ) {
        err = "connect to " + host + ":" + port_str + " failed: " + mp_socket_last_error_string();
        return false;
    }

    constexpr int connect_timeout_ms = 5000;
    const mp_socket_poll_result pr = mp_socket_poll( fd, false, true, connect_timeout_ms );
    if( pr == mp_socket_poll_result::timeout ) {
        err = "connect to " + host + ":" + port_str + " timed out";
        return false;
    }
    if( pr == mp_socket_poll_result::error ) {
        err = "connect to " + host + ":" + port_str + " failed: " + mp_socket_last_error_string();
        return false;
    }

    int so_error = 0;
#ifdef _WIN32
    int len = sizeof( so_error );
#else
    socklen_t len = sizeof( so_error );
#endif
    if( ::getsockopt( fd, SOL_SOCKET, SO_ERROR,
                      reinterpret_cast<char *>( &so_error ), &len ) < 0 ) {
        err = "connect to " + host + ":" + port_str + " failed: " + mp_socket_last_error_string();
        return false;
    }
    if( so_error != 0 ) {
#ifdef _WIN32
        err = "connect to " + host + ":" + port_str + " failed: error " + std::to_string( so_error );
#else
        err = "connect to " + host + ":" + port_str + " failed: " + std::strerror( so_error );
#endif
        return false;
    }
    return true;
}

int mp_socket_send( const mp_socket_t fd, const uint8_t *const data, const int len )
{
    int flags = 0;
#if defined(MSG_NOSIGNAL)
    flags |= MSG_NOSIGNAL;
#endif
    int n;
    do {
#ifdef _WIN32
        n = ::send( fd, reinterpret_cast<const char *>( data ), len, flags );
#else
        n = static_cast<int>( ::send( fd, data, static_cast<size_t>( len ), flags ) );
#endif
    } while( n < 0 && mp_socket_interrupted() );
    if( n < 0 && mp_socket_would_block() ) {
        return 0;
    }
    return n;
}

int mp_socket_recv( const mp_socket_t fd, uint8_t *const data, const int len )
{
    int n;
    do {
#ifdef _WIN32
        n = ::recv( fd, reinterpret_cast<char *>( data ), len, 0 );
#else
        n = static_cast<int>( ::recv( fd, data, static_cast<size_t>( len ), 0 ) );
#endif
    } while( n < 0 && mp_socket_interrupted() );
    return n;
}

std::string mp_socket_last_error_string()
{
#ifdef _WIN32
    const int e = ::WSAGetLastError();
    char *buffer = nullptr;
    const DWORD len = ::FormatMessageA(
                          FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                          FORMAT_MESSAGE_IGNORE_INSERTS,
                          nullptr, static_cast<DWORD>( e ), 0,
                          reinterpret_cast<char *>( &buffer ), 0, nullptr );
    std::string out;
    if( len > 0 && buffer != nullptr ) {
        out.assign( buffer, len );
        ::LocalFree( buffer );
    } else {
        out = "unknown socket error " + std::to_string( e );
    }
    while( !out.empty() && ( out.back() == '\n' || out.back() == '\r' ) ) {
        out.pop_back();
    }
    return out;
#else
    return std::strerror( errno );
#endif
}

mp_socket_poll_result mp_socket_poll( const mp_socket_t fd, const bool want_read,
                                      const bool want_write, const int timeout_ms )
{
#ifdef _WIN32
    WSAPOLLFD pfd = {};
#else
    pollfd pfd = {};
#endif
    pfd.fd = fd;
    if( want_read ) {
        pfd.events |= POLLIN;
    }
    if( want_write ) {
        pfd.events |= POLLOUT;
    }

    int ret;
    do {
#ifdef _WIN32
        ret = ::WSAPoll( &pfd, 1, timeout_ms );
#else
        ret = ::poll( &pfd, 1, timeout_ms );
#endif
    } while( ret < 0 && mp_socket_interrupted() );
    if( ret < 0 ) {
        return mp_socket_poll_result::error;
    }
    if( ret == 0 ) {
        return mp_socket_poll_result::timeout;
    }
    if( pfd.revents & POLLNVAL ) {
        return mp_socket_poll_result::error;
    }
    if( pfd.revents & ( POLLERR | POLLHUP ) ) {
        // The peer is gone. When POLLIN is also reported there may still be
        // buffered data worth draining; the transport's read observes the EOF
        // itself (recv returning 0), so report ready for that case.
        if( pfd.revents & POLLIN ) {
            return mp_socket_poll_result::ready;
        }
        return mp_socket_poll_result::error;
    }
    if( pfd.revents & ( POLLIN | POLLOUT ) ) {
        return mp_socket_poll_result::ready;
    }
    return mp_socket_poll_result::timeout;
}
