#pragma once
#ifndef CATA_SRC_MP_MP_SOCKET_H
#define CATA_SRC_MP_MP_SOCKET_H

#include <cstdint>
#include <string>

// A thin platform shim over the raw socket APIs, so the transports never have
// to know whether they are running on POSIX or Winsock. Every function here is
// about sockets and nothing else: no game types, no framing, no message
// semantics. Windows paths are written but cannot be compiled on this machine,
// so they are kept minimal and correct-by-construction.

#ifdef _WIN32
#include <winsock2.h>
using mp_socket_t = SOCKET;
#else
using mp_socket_t = int;
#endif

// The sentinel "no socket" value, comparable against any mp_socket_t.
#ifdef _WIN32
static constexpr mp_socket_t mp_socket_invalid = INVALID_SOCKET;
#else
static constexpr mp_socket_t mp_socket_invalid = -1;
#endif

// Must be called before any socket use. Idempotent: a no-op on POSIX, where
// sockets need no global setup; on Windows this runs WSAStartup once.
void mp_socket_startup();

// Creates a TCP socket (IPv4). Returns mp_socket_invalid on failure.
mp_socket_t mp_socket_create_tcp();

// Closes a socket, ignoring errors so a close path never masks the error that
// triggered it. Accepts mp_socket_invalid as a no-op.
void mp_socket_close( mp_socket_t fd );

// Switches the socket to non-blocking mode. Returns false on failure.
bool mp_socket_set_nonblocking( mp_socket_t fd );

// Disables Nagle's algorithm so small interactive messages go out immediately
// rather than being held back to coalesce. Returns false on failure.
bool mp_socket_set_nodelay( mp_socket_t fd );

// Ensures sends on `fd` never raise SIGPIPE. On Linux this is already handled
// per send by the MSG_NOSIGNAL flag in mp_socket_send and there is nothing to
// set up; on the BSDs (macOS) it sets SO_NOSIGPIPE on the socket once; Windows
// never raises SIGPIPE at all. Returns false on failure where the platform
// needs a setsockopt call.
bool mp_socket_set_nosigpipe( mp_socket_t fd );

// Binds the socket to all local interfaces on `port`; port 0 asks the OS for a
// free one. Returns false with `err` set on failure.
bool mp_socket_bind( mp_socket_t fd, int port, std::string &err );

// Puts a bound socket into the listening state, accepting up to `backlog`
// pending connections. Returns false on failure.
bool mp_socket_listen( mp_socket_t fd, int backlog );

// The local port the socket is bound to, used to report the OS-assigned port
// after binding to 0. Returns 0 when it cannot be determined.
int mp_socket_local_port( mp_socket_t fd );

// Accepts one incoming connection from a listening socket, returning the new
// socket or mp_socket_invalid. The caller is expected to have polled for
// readability first.
mp_socket_t mp_socket_accept( mp_socket_t fd );

// Connects the socket to `host:port`. The socket may be non-blocking; a
// connect that reports "in progress" is waited on here until the handshake
// finishes. Returns false with `err` set on failure.
bool mp_socket_connect( mp_socket_t fd, const std::string &host, int port, std::string &err );

// Sends up to `len` bytes. Returns the number of bytes actually sent (which
// may be less than `len` on a partial write), 0 when the send buffer is full
// and the call would block, or -1 on a fatal error. Never raises SIGPIPE.
int mp_socket_send( mp_socket_t fd, const uint8_t *data, int len );

// Reads up to `len` bytes. Returns the number of bytes read, 0 on a clean
// end-of-stream, or -1 on error. The caller is expected to have polled for
// readability first, so a would-block result is treated as an error.
int mp_socket_recv( mp_socket_t fd, uint8_t *data, int len );

// A human-readable description of the most recent socket error.
std::string mp_socket_last_error_string();

enum class mp_socket_poll_result {
    timeout, // the deadline passed with no event of interest
    ready,   // the socket is readable and/or writable as requested
    error,   // the socket is in an unrecoverable state
};

// Waits up to `timeout_ms` (negative for indefinitely) for the socket to
// become readable (want_read) and/or writable (want_write).
mp_socket_poll_result mp_socket_poll( mp_socket_t fd, bool want_read, bool want_write,
                                      int timeout_ms );

#endif // CATA_SRC_MP_MP_SOCKET_H
