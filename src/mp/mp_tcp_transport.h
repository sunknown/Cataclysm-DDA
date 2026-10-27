#pragma once
#ifndef CATA_SRC_MP_MP_TCP_TRANSPORT_H
#define CATA_SRC_MP_MP_TCP_TRANSPORT_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
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
        std::string peer_description() const override;
        bool connected() const override;
        void close() override;

        mp_tcp_transport( const mp_tcp_transport & ) = delete;
        mp_tcp_transport &operator=( const mp_tcp_transport & ) = delete;

    private:
        // Both expect out_mutex_ held.
        void flush_outbound();
        void disconnect();

        // Fixed for the transport's whole life and released only by the
        // destructor. A server closes a player's transport from the world
        // thread while that player's thread may be blocked reading it, so a
        // close must not change or free the descriptor under the reader; it
        // shuts the socket down instead, which also wakes the reader.
        const mp_socket_t fd_;
        // Read once at construction: the socket is still connected then, and
        // the disconnect log needs the address after it is not.
        std::string peer_;
        // Written by whichever thread ends the connection and read by others
        // (mp_server_peer_connected() asks every session's), hence atomic.
        // Every way the stream can fail -- a reset, end-of-stream, a corrupt
        // frame -- goes through disconnect(), so this alone answers
        // connected().
        std::atomic<bool> connected_;
        mp_frame_reader reader_;
        // Guards the outbound queue and every write to the socket. The player
        // whose transport this is writes their screen to it, but the world
        // writes to it too -- the goodbye when a session ends -- and two
        // writers interleaving bytes would corrupt the frame stream.
        mutable std::mutex out_mutex_;
        // Complete framed messages waiting to be written, in send order. Each
        // element is one whole [4-byte big-endian length][body] frame; a frame
        // is never split across elements. Keeping whole frames is what lets
        // drop_pending_screen_messages discard a single screen update without
        // any chance of corrupting the stream.
        std::deque<std::vector<uint8_t>> outbound_;
        // How many bytes of the front frame have already been handed to the
        // socket. The front frame is popped only once this reaches its size,
        // so outbound_.front() is always a whole frame (possibly with a sent
        // prefix), never a mid-frame fragment.
        size_t outbound_head_sent_ = 0;
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
