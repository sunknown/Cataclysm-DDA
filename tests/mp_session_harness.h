#pragma once
#ifndef CATA_TESTS_MP_SESSION_HARNESS_H
#define CATA_TESTS_MP_SESSION_HARNESS_H

#if defined(MP_SERVER)

#include <cstddef>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "mp/mp_cell.h"
#include "mp/mp_client_screen.h"
#include "mp/mp_sched.h"
#include "mp/mp_transport.h"

// A whole server session inside one process: N clients on loopback transports,
// scripted keystrokes, and access to every client's screen for comparison.
//
// The latency is not optional. Without it, code that only works at zero
// round-trip time gets written and passes, and the first real session finds it.
class mp_session_harness
{
    public:
        explicit mp_session_harness( size_t players, int latency_ms = 5 );
        ~mp_session_harness();

        // Sends one keystroke to the server as if player `slot` had pressed it.
        void press( size_t slot, int key );

        // Runs turns until `predicate` holds or `max_turns` pass. Returns false
        // on running out of turns, so a test fails with a message rather than
        // hanging.
        bool run_until( const std::function<bool()> &predicate, int max_turns = 100 );

        // The last screen the server sent this client, as the client applied it.
        const mp_screen_grid &screen( size_t slot ) const;

        // Drains every client's socket and applies the deltas, so screen() is
        // current. run_until() calls it after each turn.
        void pump_clients();

    private:
        std::vector<std::shared_ptr<mp_transport>> client_wires_;
        // The client side is free functions over a grid and a state struct
        // (src/mp/mp_client_screen.h), not an object, so the harness holds the
        // two the same way the real client does.
        std::vector<mp_screen_grid> client_grids_;
        std::vector<mp_client_state> client_states_;
        std::unique_ptr<mp_sched> sched_;
        std::vector<std::thread> threads_;
};

// True when two grids hold the same cells. mp_screen_grid has no comparison of
// its own -- it is a wire buffer, not a value type -- and the leak this whole
// phase guards against (one player seeing another's screen) is exactly a
// cell-for-cell equality question.
bool mp_grids_equal( const mp_screen_grid &a, const mp_screen_grid &b );

#endif // MP_SERVER

#endif // CATA_TESTS_MP_SESSION_HARNESS_H
