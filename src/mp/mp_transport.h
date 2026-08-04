#pragma once
#ifndef CATA_SRC_MP_MP_TRANSPORT_H
#define CATA_SRC_MP_MP_TRANSPORT_H

#include <cstdint>
#include <optional>
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
        virtual void send( std::vector<uint8_t> payload ) = 0;

        // Returns the next message. `timeout_ms` of 0 polls, a negative value
        // blocks until a message arrives or the peer goes away.
        virtual std::optional<std::vector<uint8_t>> recv( int timeout_ms ) = 0;

        virtual bool connected() const = 0;
        virtual void close() = 0;
};

#endif // CATA_SRC_MP_MP_TRANSPORT_H
