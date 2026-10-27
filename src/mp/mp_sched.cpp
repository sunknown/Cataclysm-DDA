#include "mp/mp_sched.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "cata_assert.h"
#include "debug.h"
#include "mp/mp_context.h"

namespace
{
// Written by the world thread (mp_set_active_sched) and read by every player
// thread (mp_sched_stopping, mp_assert_holds call sites). A plain pointer here
// would be a data race in the strict sense the moment a player thread starts
// reading it concurrently with the world thread's write; atomic with the
// default (sequentially consistent) ordering avoids inventing a weaker one
// that would need its own separate justification.
std::atomic<mp_sched *> g_active_sched{ nullptr };
} // namespace

mp_sched::mp_sched( const size_t players ) : players_( players ),
    waiting_for_baton_( players_, 0 )
{
}

// Handing the baton to a participant that does not exist would park it forever:
// nobody is waiting on that index, so nobody ever hands it back. Caught here
// rather than diagnosed later from a hang.
void mp_sched::check_target( const size_t to ) const
{
    cata_assert( to == world || to < players_ );
}

mp_sched::~mp_sched()
{
    shut_down();
}

void mp_sched::yield_to( const size_t from, const size_t to )
{
    check_target( to );
    std::unique_lock<std::mutex> lock( mutex_ );
    cata_assert( holder_ == from );
    holder_ = to;
    if( from != world ) {
        // The caller is now parked waiting for the baton back. Flag first,
        // notify second, exactly as in wait_for_turn: the two orders are
        // equivalent only because the mutex is held across both, and writing
        // them the same way everywhere keeps that from being load-bearing.
        waiting_for_baton_[from] = 1;
    }
    ++parked_;
    cv_.notify_all();
    cv_.wait( lock, [this, from] {
        return holder_ == from || stopping_;
    } );
    if( from != world ) {
        waiting_for_baton_[from] = 0;
    }
    // Past this point nothing here touches the object again except releasing
    // the lock, so decrementing under the lock -- before shut_down() can ever
    // observe parked_ == 0 -- is what makes that wait meaningful.
    --parked_;
    cv_.notify_all();
}

void mp_sched::wait_for_turn( const size_t who )
{
    // world (SIZE_MAX) is not a legal argument here -- unlike yield_to's and
    // release_to's `to`, which check_target() lets be world. Without this,
    // wait_for_turn(world) would index waiting_for_baton_ at SIZE_MAX: a
    // heap write far out of bounds, silently, in the one class whose whole
    // point is to fail loudly.
    cata_assert( who < players_ );
    std::unique_lock<std::mutex> lock( mutex_ );
    // Flag first, notify second: a world parked in wait_for_request must wake
    // and see that a requester appeared, or it would sleep out its whole poll.
    waiting_for_baton_[who] = 1;
    ++parked_;
    cv_.notify_all();
    cv_.wait( lock, [this, who] {
        return holder_ == who || stopping_;
    } );
    waiting_for_baton_[who] = 0;
    --parked_;
    cv_.notify_all();
}

void mp_sched::release_to( const size_t from, const size_t to )
{
    check_target( to );
    std::lock_guard<std::mutex> lock( mutex_ );
    cata_assert( holder_ == from );
    holder_ = to;
    cv_.notify_all();
}

bool mp_sched::waiting_for_baton( const size_t who ) const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return who < waiting_for_baton_.size() && waiting_for_baton_[who] != 0;
}

bool mp_sched::wait_for_request( const int timeout_ms, const std::vector<bool> &candidates )
{
    std::unique_lock<std::mutex> lock( mutex_ );
    ++parked_;
    const bool ready = cv_.wait_for( lock, std::chrono::milliseconds( timeout_ms ),
    [this, &candidates] {
        // Without this, a world parked here at shutdown sleeps out whatever
        // is left of timeout_ms instead of unwinding immediately.
        if( stopping_ ) {
            return true;
        }
        const size_t n = std::min( candidates.size(), waiting_for_baton_.size() );
        for( size_t i = 0; i < n; ++i ) {
            if( candidates[i] && waiting_for_baton_[i] != 0 ) {
                return true;
            }
        }
        return false;
    } );
    --parked_;
    cv_.notify_all();
    return ready;
}

void mp_sched::set_world_waiting( const bool waiting )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    world_waiting_ = waiting;
}

bool mp_sched::world_waiting() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return world_waiting_;
}

size_t mp_sched::holder() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return holder_;
}

bool mp_sched::holds( const size_t who ) const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return holder_ == who;
}

void mp_sched::shut_down()
{
    std::unique_lock<std::mutex> lock( mutex_ );
    stopping_ = true;
    cv_.notify_all();
    // Every caller parked in wait_for_turn/yield_to/wait_for_request must
    // reacquire mutex_ and touch waiting_for_baton_ (or, for wait_for_request,
    // just the lock) before it is done. If the destructor let mutex_ and
    // waiting_for_baton_ be destroyed first, that touch would be a
    // use-after-free. Waiting here for parked_ == 0 means every SUCH caller --
    // one already inside the wait when this ran, or one that parks while this
    // is waiting -- has fully left before this call returns. It does NOT wait
    // for a caller that has not yet entered any of those three functions; see
    // the destructor's comment.
    //
    // A thread waiting on the unwind right in unwind_if_stopping() is counted
    // too, and it does not wake on stopping_: it leaves when the unwinder
    // ahead of it hands the right on. So this wait lasts until every player
    // but the last has finished its unwind. That is finite as long as each
    // unwind is, because the holder of the right is never parked in here.
    cv_.wait( lock, [this] { return parked_ == 0; } );
    lock.unlock();

    watchdog_stop_ = true;
    if( watchdog_.joinable() ) {
        watchdog_.join();
    }
    watchdog_started_ = false;
}

bool mp_sched::stopping() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return stopping_;
}

bool mp_sched::unwind_if_stopping( const size_t who )
{
    // Same reason as wait_for_turn: world (SIZE_MAX) is not a player, and the
    // world never unwinds a menu -- it is the thread blocked in shut_down().
    cata_assert( who < players_ );
    std::unique_lock<std::mutex> lock( mutex_ );
    if( !stopping_ ) {
        return false;
    }
    if( unwinder_ == who ) {
        // A nested menu loop of the same unwind calling back in. Another
        // thread naming this slot would be a caller that read someone else's
        // slot out of the shared active context: let it through and two
        // threads unwind at once, the exact failure the right exists for.
        cata_assert( unwinder_thread_ == std::this_thread::get_id() );
        return true;
    }
    // No stopping_ in the predicate: it is already set, and it is monotone.
    // What ends this wait is the holder ahead of us handing the right back,
    // which its scope guard does when its thread body ends.
    ++parked_;
    cv_.wait( lock, [this] {
        return unwinder_ == world;
    } );
    unwinder_ = who;
    unwinder_thread_ = std::this_thread::get_id();
    --parked_;
    cv_.notify_all();
    return true;
}

void mp_sched::release_unwind( const size_t who )
{
    // world would compare equal to the "nobody holds it" value and pass as a
    // release; a player index is the only thing that can hold the right.
    cata_assert( who < players_ );
    std::lock_guard<std::mutex> lock( mutex_ );
    if( unwinder_ != who ) {
        return;
    }
    unwinder_ = world;
    unwinder_thread_ = std::thread::id();
    cv_.notify_all();
}

int mp_sched::watchdog_report_count() const
{
    return watchdog_reports_.load();
}

mp_sched *mp_active_sched()
{
    return g_active_sched.load();
}

void mp_set_active_sched( mp_sched *const sched )
{
    g_active_sched.store( sched );
}

bool mp_sched_stopping()
{
    const mp_sched *const sched = mp_active_sched();
    return sched && sched->stopping();
}

bool mp_input_must_not_wait()
{
    return mp_active_sched() && ( mp_in_world_phase() || mp_sched_stopping() );
}

void mp_assert_holds( const size_t who )
{
#if !defined(NDEBUG)
    if( mp_sched *const sched = mp_active_sched() ) {
        // "Running without holding the baton" is the failure the watchdog
        // cannot see: it only measures how long the baton sat still. Catching
        // it here names the offending participant at the moment it runs.
        cata_assert( sched->holds( who ) );
    }
#else
    ( void )who;
#endif
}

void mp_sched_start_watchdog( mp_sched &sched, const int threshold_ms )
{
    // A second start without an intervening shut_down() would assign over a
    // still-joinable std::thread, which calls std::terminate. That is a
    // programming error at the call site, not a race worth surviving quietly.
    cata_assert( !sched.watchdog_started_ );
    sched.watchdog_started_ = true;
    sched.watchdog_stop_ = false;
    sched.watchdog_ = std::thread( [&sched, threshold_ms]() {
        size_t last = sched.holder();
        auto since = std::chrono::steady_clock::now();
        bool reported = false;
        while( !sched.watchdog_stop_ ) {
            std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
            const size_t now_holder = sched.holder();
            if( now_holder != last ) {
                last = now_holder;
                since = std::chrono::steady_clock::now();
                reported = false;
                continue;
            }
            const auto held = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - since ).count();
            if( held > threshold_ms && !reported ) {
                if( last == mp_sched::world && sched.world_waiting() ) {
                    // Not a stall. The world holds the baton for as long as a
                    // partner reads a menu, and that is the design working.
                    // Reporting it would make the one message that means "a
                    // handoff was missed" indistinguishable from normal play.
                    continue;
                }
                // Report once per stall, not once per poll: a genuinely stuck
                // participant would otherwise fill the log faster than anyone
                // could read it.
                DebugLog( D_WARNING, D_MAIN )
                        << "mp sched: participant "
                        << ( last == mp_sched::world ? std::string( "world" ) :
                             std::to_string( last ) )
                        << " has held the baton for " << held << " ms";
                reported = true;
                sched.watchdog_reports_.fetch_add( 1 );
            }
        }
    } );
}
