#pragma once
#ifndef CATA_SRC_MP_MP_FRAME_H
#define CATA_SRC_MP_MP_FRAME_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// Message framing for the multiplayer transports.
//
// The wire carries "length plus body": a four byte big-endian payload length
// followed by exactly that many bytes. Both transports (TCP and the in-memory
// loopback used by tests) hand raw bytes to mp_frame_reader, which reassembles
// whole payloads regardless of how the stream was chopped up.

// Largest payload we will accept. A full redraw of a very large terminal is a
// few hundred kilobytes, so this is generous; its purpose is to keep a corrupt
// or hostile length header from provoking an unbounded allocation.
constexpr size_t MP_MAX_FRAME_BYTES = 8u * 1024u * 1024u;

std::vector<uint8_t> mp_frame_encode( const std::vector<uint8_t> &payload );

class mp_frame_reader
{
    public:
        // Appends raw bytes as they arrive from the peer.
        void feed( const uint8_t *data, size_t len );

        // Returns the next complete payload, or nullopt when the buffered
        // bytes do not yet contain one.
        std::optional<std::vector<uint8_t>> next();

        // True once the length header at the head of the buffered stream was
        // found to be malformed. The stream cannot be resynchronised after
        // that, so the caller must drop the connection.
        bool failed() const {
            return failed_;
        }

    private:
        std::vector<uint8_t> buffer_;
        bool failed_ = false;
};

#endif // CATA_SRC_MP_MP_FRAME_H
