#pragma once
#ifndef CATA_SRC_MP_MP_SCHED_H
#define CATA_SRC_MP_MP_SCHED_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
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
        // Calls shut_down(), which blocks until every waiter parked inside this
        // object at the time has left. That is NOT the whole precondition for
        // destroying safely: a thread that has not yet entered a wait_for_turn/
        // yield_to/wait_for_request call when shut_down() finishes draining is
        // not counted, nor is the unwinder still to call release_unwind(), so
        // either can still be about to touch a member that is mid-
        // destruction. The owner must join every thread that might call any
        // mp_sched method -- player threads and anyone else -- before the
        // mp_sched object itself goes out of scope or is deleted.
        ~mp_sched();

        mp_sched( const mp_sched & ) = delete;
        mp_sched &operator=( const mp_sched & ) = delete;

        // Hands the baton to `to` and blocks until this participant holds it
        // again. The two halves are one call because every yield in the game is
        // a yield-and-resume: the code that gave up the baton always wants to
        // continue where it left off. Can return WITHOUT the baton -- holder()
        // will not be `from`'s own id -- if shut_down() was called while this
        // call was parked; callers must check stopping() before assuming they
        // hold it.
        void yield_to( size_t from, size_t to );

        // Blocks until `who` holds the baton. Used by a freshly started player
        // thread, which has nothing to hand over yet. `who` must be a player
        // index (< the `players` given to the constructor); mp_sched::world is
        // not a legal argument here, unlike yield_to/release_to's `to`. Can
        // return WITHOUT the baton if shut_down() was called while parked;
        // callers must check stopping() before assuming they hold it.
        void wait_for_turn( size_t who );

        // Hands the baton to `to` without waiting to get it back. Used by a
        // player thread that is finishing for the turn.
        void release_to( size_t from, size_t to );

        // Who holds the baton right now.
        size_t holder() const;

        // Monotonic count of baton handoffs. Incremented under mutex_ wherever
        // holder_ changes (release_to and the handoff half of yield_to), so a
        // watcher that samples this sees every handoff even when two samples
        // find the same holder: the watchdog uses it to tell "the holder never
        // changed" from "the holder changed and came back between my polls".
        size_t holder_generation() const;

        // Up to the last 256 trace records, oldest first, each formatted
        // "<generation> <holder> <reason>" with holder "world" or a slot
        // number. Written under mutex_ at every handoff and by
        // mp_sched_trace_phase(). Test-visible so a test can prove what the
        // watchdog sampled without scraping DebugLog; a manual run mirrors
        // each record there as "mp sched trace: ..." when MP_SCHED_TRACE is
        // set in the environment.
        std::vector<std::string> trace_snapshot() const;

        // Whether the calling thread is the current holder. The debug assert
        // "only the holder runs" is written in terms of this.
        bool holds( size_t who ) const;

        // Signals every participant to end, then blocks until every waiter
        // that was parked inside this object (in wait_for_turn, the resume
        // half of yield_to, wait_for_request, or waiting on the unwind right in
        // unwind_if_stopping) has woken and left -- so no such waiter is still
        // touching mutex_ or waiting_for_baton_ when this call returns. The
        // first three wake on stopping_ itself; a thread waiting on the unwind
        // right leaves only once the unwinders ahead of it have finished, so
        // this call can outlast all but the last player's unwind. Called by
        // the destructor and by session teardown. Idempotent: safe to call
        // more than once.
        //
        // This is necessary but not sufficient for safe destruction; see the
        // destructor's comment for the rest of the precondition it does not
        // cover.
        void shut_down();

        // Whether shut_down() has been called. A player thread body checks it
        // after every resume and returns when it is set.
        bool stopping() const;

        // The unwind right: the shutdown counterpart of the baton.
        //
        // shut_down() wakes every parked player at once, on purpose, and a
        // woken player holds NO baton -- yet a player woken inside a menu
        // still has that whole menu stack to unwind, and an unwind runs game
        // code against process-wide single-writer state: ui_manager's active
        // stack, the active-context pointer, the avatar swapped into g->u.
        // Two unwinding at once trip ~ui_adaptor()'s owner == active_stack
        // assert. The right restores "exactly one player context runs at a
        // time" for the teardown too: at most one player holds it, from the
        // moment that player learns of the shutdown until its thread has
        // fully exited its per-player loop.
        //
        // It is not the baton, and cannot be. A baton is taken and given back
        // around one turn; an unwind re-enters get_input_event() an unbounded
        // number of times as each nested menu loop notices the shutdown and
        // returns, and the right has to be held across all of them.
        //
        // unwind_if_stopping( who ): returns false at once, touching nothing,
        // when shut_down() has not been called. Otherwise blocks until no
        // other player holds the right, takes it for `who`, and returns true.
        // Idempotent: a caller that already holds the right returns true at
        // once, which is what makes the repeated get_input_event() re-entries
        // of one unwind safe. Checking and taking are one call so that no
        // caller can observe the shutdown without also serialising on it.
        // `who` must be a player index, and a re-entry must come from the
        // thread that took the right (asserted: a thread naming a slot that
        // is not its own is exactly the slip that would let two unwind).
        //
        // release_unwind( who ): gives the right back and wakes the next
        // waiter. A no-op when `who` does not hold it -- a thread that ends
        // without ever having seen the shutdown, or a test body that returns
        // early -- so an unconditional scope guard around the whole thread
        // body is the intended shape, and it cannot release someone else's.
        //
        // What it guarantees: no two players run unwind code at once, and
        // the thread granted the right was granted it under the mutex, after
        // the previous holder's whole loop body had ended.
        //
        // What it does NOT do, which the owner and the callers must:
        //  - Repoint the process-wide context. Whoever is granted the right
        //    must mp_activate_player( who ) before running game code, for the
        //    same reason the baton-reacquire path in get_input_event() does:
        //    the previous unwinder left its own context active.
        //  - Release the baton. A woken player does not hold it, and a stop
        //    path must return WITHOUT release_to(): at teardown the world
        //    needs nothing back, and release_to() asserts real ownership.
        //  - Release itself. Every player thread body must hold a scope guard
        //    calling release_unwind( slot ) for its whole lifetime; a thread
        //    that exits holding the right parks every later unwinder forever,
        //    and with them shut_down().
        //  - Restore the world phase. The last unwinder leaves its own slot
        //    active, avatar swapped into g->u; the owner must call
        //    mp_activate_world() once it has joined the player threads and
        //    before it touches the slots.
        //  - Make a menu terminate. The right serialises unwinds; a menu loop
        //    that ignores the substituted QUIT holds it forever, and
        //    shut_down() with it, since its waiters are counted in parked_.
        //
        // Waiting on the right is counted in parked_ like every other wait, so
        // shut_down() does not return while a thread is still parked on it.
        // That cannot deadlock: the holder is never itself parked in this
        // object (every other wait here wakes on stopping_), so its unwind
        // runs to its scope guard and hands the right on, and the last holder
        // is never waiting at all.
        bool unwind_if_stopping( size_t who );
        void release_unwind( size_t who );

        // Whether `who` is currently parked on wait_for_turn (or on the resume
        // half of yield_to), i.e. ready to be handed the baton. The world polls
        // this to find who wants to run next. A slot blocked on the wire inside
        // an input wait is NOT waiting for the baton and reports false.
        bool waiting_for_baton( size_t who ) const;

        // The join phase's per-seat counterpart of shut_down(): one participant
        // leaving while the session goes on. A client that quits before the
        // party starts (its menu's Quit, Escape, or a dropped wire) frees its
        // seat, and its thread still has a menu open: it has to unwind that
        // menu without the rest of the session ending, because every other
        // participant keeps waiting for its replacement.
        //
        // Marked by the world, and readable by the participant itself through
        // mp_input_must_not_wait(): the marked seat -- and only it -- answers
        // QUIT to its next input wait, which is the same unwind Escape takes.
        // Cleared when the seat is taken again, so a replacement client's menu
        // behaves like a first one.
        void request_quit( size_t who );
        void clear_quit( size_t who );
        bool quit_requested( size_t who ) const;

        // Ends a seat whose client is gone after it had already chosen, still
        // during the join phase. request_quit() is not enough there: a player
        // that has chosen has no menu open -- its thread is parked on
        // wait_for_turn waiting for the party to start -- so the QUIT answer
        // reaches nobody and the world would block for ever joining the thread
        // while the other players wait on a seat whose client no longer exists.
        // This marks the seat (as request_quit does, so an input wait still
        // answers QUIT) and wakes a thread parked on wait_for_turn, which then
        // returns without the baton. Cleared by clear_quit(), like request_quit.
        void abandon_seat( size_t who );

        // Blocks the world participant until one of the players named in
        // `candidates` (indexed by slot) is parked waiting for the baton, or
        // until `timeout_ms` passes, or until shut_down(). Returns true when a
        // candidate is ready. This is what replaces a spin: the world sleeps
        // instead of burning a core while a partner reads a menu.
        //
        // The candidates are the world's to name because only the world knows
        // who still owes the turn. A player who has already acted re-parks on
        // wait_for_turn at once, so "anyone parked" is true for the whole of a
        // waiting turn, and a wait on it returns immediately -- the world then
        // spins at 100% CPU for as long as the partner reads. The set cannot
        // go stale while the world sleeps: it sleeps holding the baton, so no
        // player can change the game state it was computed from. A slot past
        // the end of `candidates` is not a candidate.
        bool wait_for_request( int timeout_ms, const std::vector<bool> &candidates );

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

        // How many times the watchdog owned by this instance has logged a
        // stall. Not used by production code -- it exists so a test can
        // assert "reported once, not repeatedly" and "suppressed while
        // world_waiting()" deterministically, without scraping DebugLog's
        // output stream.
        int watchdog_report_count() const;

    private:
        // Asserts that `to` names a real participant. Handing the baton to an
        // index nobody waits on parks it forever.
        void check_target( size_t to ) const;

        // Appends "<generation> <holder> <reason>" to trace_ and returns it.
        // The caller holds mutex_; the return value lets it emit the DebugLog
        // mirror after unlocking, so no I/O happens under the scheduler lock.
        std::string record_trace_locked( const char *reason );

        // record_trace_locked() under the lock, then the DebugLog mirror.
        void trace_phase( const char *phase );

        friend void mp_sched_trace_phase( const char *phase );

        mutable std::mutex mutex_;
        std::condition_variable cv_;
        size_t players_ = 0;
        size_t holder_ = world;
        // Incremented under mutex_ with every assignment to holder_; see
        // holder_generation().
        size_t holder_generation_ = 0;
        // Ring of the last 256 trace records; see trace_snapshot(). Written
        // only under mutex_; read by tests and by the DebugLog mirror.
        std::deque<std::string> trace_;
        bool stopping_ = false;
        bool world_waiting_ = false;
        // Per-participant "parked on wait_for_turn / the resume half of
        // yield_to" flag, indexed by player slot (0..players_-1). The world
        // participant has no entry: it never waits for the baton, it waits for
        // somebody to ask for it (wait_for_request).
        std::vector<char> waiting_for_baton_;
        // Per-participant "your seat was released during the join phase" flag,
        // indexed the same way. Set and cleared only by the world, read by the
        // participant itself through mp_input_must_not_wait(); see
        // request_quit().
        std::vector<char> quit_requested_;
        // Per-participant "your seat's client is gone and you are not in a menu"
        // flag: the wake-up half of abandon_seat(). Kept apart from
        // quit_requested_ because request_quit() must NOT wake a participant
        // that has not drawn its menu yet -- it would then run the menu without
        // the baton -- while abandon_seat() applies only to a participant that
        // has already chosen and is parked on the baton.
        std::vector<char> abandoned_;
        // Holder of the unwind right (see unwind_if_stopping), or
        // mp_sched::world while nobody holds it, and the thread that took it.
        // The thread id is only there to catch a re-entry naming a slot that
        // belongs to another thread; it is never used to decide who runs.
        size_t unwinder_ = world;
        std::thread::id unwinder_thread_;
        // Count of callers currently blocked inside a cv_.wait/wait_for in
        // wait_for_turn, yield_to, unwind_if_stopping or wait_for_request --
        // i.e. still holding a reference to this object's members that
        // shut_down() must not let become dangling. Incremented under mutex_
        // right before parking,
        // decremented under mutex_ right after waking, so shut_down() can wait
        // on "parked_ == 0" and know every such caller has fully finished
        // touching the object.
        size_t parked_ = 0;

        // The watchdog thread and its stop flag belong to the instance they
        // watch, not to the process: shut_down() must only ever stop and join
        // the watchdog started for THIS scheduler, never one still running for
        // a different instance. See mp_sched_start_watchdog().
        std::thread watchdog_;
        std::atomic<bool> watchdog_stop_{ false };
        // Set once mp_sched_start_watchdog() has been called for this
        // instance and cleared once shut_down() has stopped it. Asserted
        // against on a second start with no intervening shut_down(), which
        // would otherwise assign over a still-joinable std::thread and call
        // std::terminate.
        bool watchdog_started_ = false;
        std::atomic<int> watchdog_reports_{ 0 };

        friend void mp_sched_start_watchdog( mp_sched &sched, int threshold_ms );
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

// Whether whoever is asking for input right now must get an answer at once
// instead of a wait: the session is shutting down, the join phase released the
// asker's own seat (see mp_sched::request_quit), or the world is asking --
// it holds the baton, so the asker is the world thread: an NPC opening a
// conversation or a monster's question in the world's half of the turn, an
// effect's popup in a player's upkeep (avatar_turn_post runs on the world
// thread under that player's context). The world has no player of its own,
// and waiting on any one player would ask them for the baton the asker holds.
// Both get a timeout from the backend, which handle_input() turns into QUIT:
// every menu's cancel.
bool mp_input_must_not_wait();

// Whether the baton holder's seat was released during the join phase, so that
// its menu has to close the way Escape closes it. A subset of
// mp_input_must_not_wait() -- the part that is about this participant leaving
// rather than the session ending -- for the menus that distinguish the two: a
// seat released this way must cancel even when the session is not stopping.
bool mp_seat_released();

// Whether a menu open on this thread has to unwind now: the session is
// stopping, or this player's seat was released during the join phase. This is
// the question every menu loop asks after its input wait, and it is one
// function so that the loops which have an action to cancel on (uilist's
// QUIT), the loops that have to quit out of a text field
// (string_input_popup) and the character creator's tab loop -- which has no
// action to reach at all when its current tab owns a uilist -- cannot drift
// apart. The world's own menus are answered for by mp_input_must_not_wait()
// instead and are not unwound: the world has no seat to lose.
bool mp_menu_must_unwind();

// Records one world-phase marker in the active scheduler's trace, when there is
// one. A no-op in single player. Used by do_turn() to name what the world was
// doing while it held the baton, so a watchdog report can be read against it.
void mp_sched_trace_phase( const char *phase );

// Starts the watchdog thread owned by `sched`. It logs the current holder
// whenever the baton has not moved for longer than `threshold_ms`, then keeps
// watching. Stopped by sched.shut_down(). Calling this twice on the same
// `sched` without an intervening shut_down() trips a debug assert -- it would
// otherwise assign over a still-joinable std::thread and call std::terminate.
void mp_sched_start_watchdog( mp_sched &sched, int threshold_ms );

#endif // CATA_SRC_MP_MP_SCHED_H
