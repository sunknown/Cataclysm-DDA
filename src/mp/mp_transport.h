#pragma once
#ifndef CATA_SRC_MP_MP_TRANSPORT_H
#define CATA_SRC_MP_MP_TRANSPORT_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// A bidirectional, ordered, reliable message channel.
//
// Messages are whole payloads: framing, if the underlying medium needs it, is
// the implementation's business. Two implementations exist -- TCP for real
// play and an in-memory pair for tests.
class mp_transport
{
    public:
        virtual ~mp_transport() = default;

        // Queues a message for delivery. Never blocks; a peer that cannot keep
        // up causes the outgoing buffer to grow, which the caller handles.
        // Safe from any thread, alongside a recv() on another: a server's world
        // thread says goodbye on a wire that a player thread is reading.
        virtual void send( std::vector<uint8_t> payload ) = 0;

        // Returns the next message. `timeout_ms` of 0 polls, a negative value
        // blocks until a message arrives or the peer goes away.
        virtual std::optional<std::vector<uint8_t>> recv( int timeout_ms ) = 0;

        // How many payload bytes are queued but not yet delivered to the peer.
        // A slow peer makes this grow without bound; the server uses it to
        // decide when the backlog needs collapsing (see
        // drop_pending_screen_messages). The default of 0 lets a transport
        // opt out of backpressure entirely.
        virtual size_t pending_bytes() const {
            return 0;
        }

        // Drops every queued message that carries a screen update, leaving
        // control messages untouched. Screen state is idempotent, so a client
        // that fell behind is better served by one full redraw than by
        // replaying the whole stale history; dropping here is what lets the
        // server send that redraw instead. The default does nothing because
        // the abstract transport cannot know how a subclass buffers.
        virtual void drop_pending_screen_messages() {}

        // How the peer should be named in the server's log. The default suits
        // any transport with no address to speak of (the in-memory loopback);
        // the TCP transport reports the address that connected.
        virtual std::string peer_description() const {
            return "peer";
        }

        virtual bool connected() const = 0;
        virtual void close() = 0;
};

#endif // CATA_SRC_MP_MP_TRANSPORT_H
