#pragma once
#ifndef CATA_SRC_MP_MP_LOOPBACK_TRANSPORT_H
#define CATA_SRC_MP_MP_LOOPBACK_TRANSPORT_H

#include <memory>

#include "mp/mp_transport.h"

struct mp_loopback_pair {
    std::shared_ptr<mp_transport> a;
    std::shared_ptr<mp_transport> b;
};

// Creates two transports wired to each other in memory. `simulated_latency_ms`
// delays delivery in each direction, so tests can catch code that only works
// at zero round-trip time.
mp_loopback_pair mp_make_loopback_pair( int simulated_latency_ms = 0 );

#endif // CATA_SRC_MP_MP_LOOPBACK_TRANSPORT_H
