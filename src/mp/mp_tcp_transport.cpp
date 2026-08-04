#include "mp/mp_tcp_transport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "mp/mp_protocol.h"

namespace
{

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch() ).count();
}

} // namespace

mp_tcp_transport::mp_tcp_transport( const mp_socket_t fd )
    : fd_( fd ), connected_( fd != mp_socket_invalid ) {}

mp_tcp_transport::~mp_tcp_transport()
{
    close();
}

void mp_tcp_transport::send( std::vector<uint8_t> payload )
{
    if( !connected_ ) {
        // A closed wire silently drops the message, matching the loopback
        // transport's early return on a closed channel.
        return;
    }
    outbound_.emplace_back( mp_frame_encode( payload ) );
    flush_outbound();
}

std::optional<std::vector<uint8_t>> mp_tcp_transport::recv( const int timeout_ms )
{
    // Push whatever we owe the peer before settling down to wait for them; a
    // reply is usually the answer to what we just sent.
    flush_outbound();
    if( !connected() ) {
        return std::nullopt;
    }

    // A message that already arrived during an earlier read is served without
    // touching the socket again.
    if( std::optional<std::vector<uint8_t>> msg = reader_.next() ) {
        return msg;
    }

    // Wait for a complete message, reassembling partial frames across several
    // reads. The deadline covers the whole call, so a message that arrives in
    // dribs and drabs still fits within the one budget.
    const int64_t deadline = timeout_ms < 0 ? -1 : now_ms() + timeout_ms;
    while( connected() ) {
        int remaining = -1;
        if( deadline >= 0 ) {
            const int64_t left = deadline - now_ms();
            if( left < 0 && timeout_ms != 0 ) {
                // A positive timeout is spent. A zero timeout deliberately
                // falls through to the poll(0) below so recv(0) is a genuine
                // immediate check even when the millisecond tick landed between
                // the two clock reads.
                return std::nullopt;
            }
            remaining = static_cast<int>( std::min<int64_t>( std::max<int64_t>( left, 0 ),
                                       INT32_MAX ) );
        }

        const mp_socket_poll_result pr = mp_socket_poll( fd_, true, false, remaining );
        if( pr == mp_socket_poll_result::timeout ) {
            return std::nullopt;
        }
        if( pr == mp_socket_poll_result::error ) {
            disconnect();
            return std::nullopt;
        }

        std::array<uint8_t, 16384> buf;
        const int n = mp_socket_recv( fd_, buf.data(), static_cast<int>( buf.size() ) );
        if( n == 0 || n < 0 ) {
            // n == 0: the peer closed cleanly. n < 0: a socket error, e.g. a
            // reset. Both mean the peer is gone; crucially, both must also
            // unblock a recv(-1), so the server's input loop can notice the
            // drop and tear down instead of hanging forever.
            disconnect();
            return std::nullopt;
        }
        reader_.feed( buf.data(), static_cast<size_t>( n ) );
        if( reader_.failed() ) {
            disconnect();
            return std::nullopt;
        }
        if( std::optional<std::vector<uint8_t>> msg = reader_.next() ) {
            return msg;
        }
        // No complete frame yet: loop to wait for the rest of it.
    }
    return std::nullopt;
}

size_t mp_tcp_transport::pending_bytes() const
{
    size_t total = 0;
    for( const std::vector<uint8_t> &frame : outbound_ ) {
        total += frame.size();
    }
    if( !outbound_.empty() ) {
        // The front frame's already-sent prefix is no longer undelivered.
        total -= std::min( outbound_head_sent_, outbound_.front().size() );
    }
    return total;
}

void mp_tcp_transport::drop_pending_screen_messages()
{
    // outbound_ holds whole frames, so dropping one cannot corrupt framing:
    // the head frame may be partially sent -- its length header and the start
    // of its body are already on the wire, so it must always survive, even a
    // screen frame. Every frame after it is a whole frame: screen updates are
    // dropped, control messages are kept, and the survivors stay whole.
    size_t i = ( !outbound_.empty() && outbound_head_sent_ > 0 ) ? 1 : 0;
    while( i < outbound_.size() ) {
        const std::vector<uint8_t> &frame = outbound_[i];
        // body[0] is the mp_msg_kind (see mp_wrap); a frame with no body
        // cannot be a screen update.
        const bool is_screen = frame.size() > 4 &&
                               frame[4] == static_cast<uint8_t>( mp_msg_kind::screen );
        if( is_screen ) {
            outbound_.erase( outbound_.begin() + i );
        } else {
            ++i;
        }
    }
}

bool mp_tcp_transport::connected() const
{
    return connected_ && fd_ != mp_socket_invalid && !reader_.failed();
}

void mp_tcp_transport::close()
{
    if( !connected_ ) {
        return;
    }
    connected_ = false;
    mp_socket_close( fd_ );
    fd_ = mp_socket_invalid;
    outbound_.clear();
    outbound_head_sent_ = 0;
    // reader_ is left alone on purpose: connected() is already false, so no
    // caller can observe its buffered bytes again.
}

void mp_tcp_transport::flush_outbound()
{
    while( connected_ && !outbound_.empty() ) {
        const std::vector<uint8_t> &front = outbound_.front();
        const size_t remaining = front.size() - outbound_head_sent_;
        const int n = mp_socket_send( fd_, front.data() + outbound_head_sent_,
                                      static_cast<int>( remaining ) );
        if( n > 0 ) {
            outbound_head_sent_ += static_cast<size_t>( n );
            if( outbound_head_sent_ == front.size() ) {
                // The front frame is fully written; move on to the next one.
                outbound_.pop_front();
                outbound_head_sent_ = 0;
            }
            continue;
        }
        if( n < 0 ) {
            // A fatal write error, e.g. the peer reset the connection.
            disconnect();
            return;
        }
        // n == 0: the kernel send buffer is full. The rest waits for the next
        // send() or recv(), both of which re-enter flush_outbound.
        return;
    }
}

void mp_tcp_transport::disconnect()
{
    if( !connected_ ) {
        return;
    }
    connected_ = false;
    mp_socket_close( fd_ );
    fd_ = mp_socket_invalid;
    outbound_.clear();
    outbound_head_sent_ = 0;
}

std::optional<mp_listener> mp_listener::bind( const int port, std::string &err )
{
    mp_socket_startup();

    const mp_socket_t fd = mp_socket_create_tcp();
    if( fd == mp_socket_invalid ) {
        err = "socket() failed: " + mp_socket_last_error_string();
        return std::nullopt;
    }

    if( !mp_socket_bind( fd, port, err ) ) {
        mp_socket_close( fd );
        return std::nullopt;
    }
    if( !mp_socket_listen( fd, 8 ) ) {
        err = "listen() failed: " + mp_socket_last_error_string();
        mp_socket_close( fd );
        return std::nullopt;
    }
    // The listening socket must be non-blocking so accept() can honour its
    // timeout through mp_socket_poll.
    if( !mp_socket_set_nonblocking( fd ) ) {
        err = "could not make the listening socket non-blocking: " +
              mp_socket_last_error_string();
        mp_socket_close( fd );
        return std::nullopt;
    }

    return mp_listener( fd );
}

mp_listener::mp_listener( const mp_socket_t fd )
    : fd_( fd ) {}

mp_listener::mp_listener( mp_listener &&other ) noexcept
    : fd_( other.fd_ )
{
    other.fd_ = mp_socket_invalid;
}

mp_listener &mp_listener::operator=( mp_listener &&other ) noexcept
{
    if( this != &other ) {
        if( fd_ != mp_socket_invalid ) {
            mp_socket_close( fd_ );
        }
        fd_ = other.fd_;
        other.fd_ = mp_socket_invalid;
    }
    return *this;
}

mp_listener::~mp_listener()
{
    if( fd_ != mp_socket_invalid ) {
        mp_socket_close( fd_ );
    }
}

int mp_listener::port() const
{
    return mp_socket_local_port( fd_ );
}

std::shared_ptr<mp_transport> mp_listener::accept( const int timeout_ms )
{
    if( fd_ == mp_socket_invalid ) {
        return nullptr;
    }
    const mp_socket_poll_result pr = mp_socket_poll( fd_, true, false, timeout_ms );
    if( pr == mp_socket_poll_result::timeout || pr == mp_socket_poll_result::error ) {
        return nullptr;
    }
    const mp_socket_t accepted = mp_socket_accept( fd_ );
    if( accepted == mp_socket_invalid ) {
        return nullptr;
    }
    // Everything is set on the accepted socket explicitly rather than trusted
    // to be inherited from the listening socket: non-blocking, no Nagle, and
    // SIGPIPE suppression (the latter is a per-send flag on Linux, but on macOS
    // it is SO_NOSIGPIPE and the brief mandates it on both ends).
    if( !mp_socket_set_nonblocking( accepted ) || !mp_socket_set_nodelay( accepted )
        || !mp_socket_set_nosigpipe( accepted ) ) {
        mp_socket_close( accepted );
        return nullptr;
    }
    return std::make_shared<mp_tcp_transport>( accepted );
}

std::shared_ptr<mp_transport> mp_tcp_connect( const std::string &host, const int port,
        std::string &err )
{
    mp_socket_startup();

    const mp_socket_t fd = mp_socket_create_tcp();
    if( fd == mp_socket_invalid ) {
        err = "socket() failed: " + mp_socket_last_error_string();
        return nullptr;
    }
    if( !mp_socket_set_nodelay( fd ) ) {
        err = "setsockopt(TCP_NODELAY) failed: " + mp_socket_last_error_string();
        mp_socket_close( fd );
        return nullptr;
    }
    if( !mp_socket_set_nonblocking( fd ) ) {
        err = "could not make the socket non-blocking: " + mp_socket_last_error_string();
        mp_socket_close( fd );
        return nullptr;
    }
    if( !mp_socket_connect( fd, host, port, err ) ) {
        mp_socket_close( fd );
        return nullptr;
    }
    return std::make_shared<mp_tcp_transport>( fd );
}
