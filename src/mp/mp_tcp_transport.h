#pragma once
#ifndef CATA_SRC_MP_MP_TCP_TRANSPORT_H
#define CATA_SRC_MP_MP_TCP_TRANSPORT_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "mp/mp_frame.h"
#include "mp/mp_socket.h"
#include "mp/mp_transport.h"

// A mp_transport over a real TCP socket. All sockets involved are non-blocking;
// a send that the peer cannot absorb accumulates in an outbound buffer which is
// drained on the next send() or recv(), and incoming bytes are reassembled into
// whole payloads by a mp_frame_reader.
class mp_tcp_transport : public mp_transport
{
    public:
        // Takes ownership of `fd`, which must be an already-connected socket.
        explicit mp_tcp_transport( mp_socket_t fd );
        ~mp_tcp_transport() override;

        void send( std::vector<uint8_t> payload ) override;
        std::optional<std::vector<uint8_t>> recv( int timeout_ms ) override;
        size_t pending_bytes() const override;
        void drop_pending_screen_messages() override;
        bool connected() const override;
        void close() override;

        mp_tcp_transport( const mp_tcp_transport & ) = delete;
        mp_tcp_transport &operator=( const mp_tcp_transport & ) = delete;

    private:
        void flush_outbound();
        void disconnect();

        mp_socket_t fd_ = mp_socket_invalid;
        bool connected_ = true;
        mp_frame_reader reader_;
        std::vector<uint8_t> outbound_;
};

// Listens for incoming TCP connections. One listener owns one listening
// socket, which it closes when destroyed. It is movable but not copyable, so
// it can be returned by value from mp_listener::bind.
class mp_listener
{
    public:
        // Binds to all interfaces on `port` (0 asks the OS for a free port) and
        // starts listening. Returns nullopt with `err` set on failure.
        static std::optional<mp_listener> bind( int port, std::string &err );

        // The port this listener is bound to; the OS-assigned one when bind
        // was given 0.
        int port() const;

        // Waits up to timeout_ms for a connection and returns a transport for
        // it, or nullptr on timeout or error.
        std::shared_ptr<mp_transport> accept( int timeout_ms );

        ~mp_listener();

        mp_listener( const mp_listener & ) = delete;
        mp_listener &operator=( const mp_listener & ) = delete;
        mp_listener( mp_listener && ) noexcept;
        mp_listener &operator=( mp_listener && ) noexcept;

    private:
        explicit mp_listener( mp_socket_t fd );
        mp_socket_t fd_ = mp_socket_invalid;
};

// Opens a TCP connection to `host:port`, returning a connected transport, or
// nullptr with `err` set on failure.
std::shared_ptr<mp_transport> mp_tcp_connect( const std::string &host, int port, std::string &err );

#endif // CATA_SRC_MP_MP_TCP_TRANSPORT_H
