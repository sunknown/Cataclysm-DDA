#pragma once
#ifndef CATA_SRC_MP_MP_SCHED_H
#define CATA_SRC_MP_MP_SCHED_H

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <vector>

// The baton. Exactly one participant runs at a time; the right to run is passed
// explicitly. There is no real parallelism, which is what keeps world mutation
// as sequential as it is in single player -- and what makes a ThreadSanitizer
// report a genuine missed handoff rather than noise.
//
// Participants are identified by index: 0..n-1 are the players, and
// mp_sched::world is the orchestrating thread.
class mp_sched
{
    public:
        static constexpr size_t world = static_cast<size_t>( -1 );

        // Creates the scheduler with `players` player participants. The calling
        // thread becomes the world participant and starts out holding the baton.
        explicit mp_sched( size_t players );
        ~mp_sched();

        mp_sched( const mp_sched & ) = delete;
        mp_sched &operator=( const mp_sched & ) = delete;

        // Hands the baton to `to` and blocks until this participant holds it
        // again. The two halves are one call because every yield in the game is
        // a yield-and-resume: the code that gave up the baton always wants to
        // continue where it left off.
        void yield_to( size_t from, size_t to );

        // Blocks until `who` holds the baton. Used by a freshly started player
        // thread, which has nothing to hand over yet.
        void wait_for_turn( size_t who );

        // Hands the baton to `to` without waiting to get it back. Used by a
        // player thread that is finishing for the turn.
        void release_to( size_t from, size_t to );

        // Who holds the baton right now.
        size_t holder() const;

        // Whether the calling thread is the current holder. The debug assert
        // "only the holder runs" is written in terms of this.
        bool holds( size_t who ) const;

        // Ends every player participant: each is woken in turn and told to
        // return from its body. Called by the destructor and by session
        // teardown.
        void shut_down();

        // Whether shut_down() has been called. A player thread body checks it
        // after every resume and returns when it is set.
        bool stopping() const;

        // Whether `who` is currently parked on wait_for_turn (or on the resume
        // half of yield_to), i.e. ready to be handed the baton. The world polls
        // this to find who wants to run next. A slot blocked on the wire inside
        // an input wait is NOT waiting for the baton and reports false.
        bool waiting_for_baton( size_t who ) const;

        // Blocks the world participant until any player becomes ready to run, or
        // until `timeout_ms` passes. Returns true when a player is ready. This
        // is what replaces a spin: the world sleeps instead of burning a core
        // while a partner reads a menu.
        bool wait_for_request( int timeout_ms );

        // Declares that the world is holding the baton on purpose: the barrier
        // is unsatisfied and it is waiting for somebody to come back from an
        // input wait. Set by the orchestrator when it closes out a waiting turn
        // and cleared as soon as the barrier is satisfied.
        //
        // Only the watchdog reads it. Without it, "a partner read a menu for
        // half a minute" -- the design working exactly as specified -- logs as
        // a stalled participant, and the phase gate's log fills with false
        // alarms until nobody reads it.
        void set_world_waiting( bool waiting );
        bool world_waiting() const;

    private:
        // Asserts that `to` names a real participant. Handing the baton to an
        // index nobody waits on parks it forever.
        void check_target( size_t to ) const;

        mutable std::mutex mutex_;
        std::condition_variable cv_;
        size_t players_ = 0;
        size_t holder_ = world;
        bool stopping_ = false;
        bool world_waiting_ = false;
        // Per-participant "parked on wait_for_turn / the resume half of
        // yield_to" flag, indexed by player slot (0..players_-1). The world
        // participant has no entry: it never waits for the baton, it waits for
        // somebody to ask for it (wait_for_request).
        std::vector<char> waiting_for_baton_;
};

// The world participant's polling cadence while the barrier is held: it sleeps
// up to this long for a player to become ready before deciding the tick must
// wait. A few round-trips -- the loopback test transport runs at ~5 ms latency,
// so a keypress lands within one or two polls. Too small spins the world, too
// large makes waiting turns sluggish.
constexpr int MP_POLL_MS = 50;

// The scheduler of the running session, or nullptr in a single-player session
// (where there is no baton because there is nothing to pass it to).
mp_sched *mp_active_sched();
void mp_set_active_sched( mp_sched *sched );

// Debug-build assert that the calling thread is the baton holder for `who`.
// A no-op in release builds. Catches "running without holding", which the
// watchdog cannot see -- the watchdog only catches "holding too long".
void mp_assert_holds( size_t who );

// Whether the session is shutting down; false when there is no scheduler at
// all. Read by player_act's loop, which must unwind when the session ends:
// get_input_event hands back a timeout event without the baton, and a loop that
// kept going would call get_input_event again and trip mp_assert_holds.
bool mp_sched_stopping();

// Starts the watchdog thread. It logs the current holder whenever the baton has
// not moved for longer than `threshold_ms`, then keeps watching. Stopped by
// mp_sched::shut_down().
void mp_sched_start_watchdog( mp_sched &sched, int threshold_ms );

#endif // CATA_SRC_MP_MP_SCHED_H
