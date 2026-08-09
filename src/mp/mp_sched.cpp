#include "mp/mp_sched.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "cata_assert.h"
#include "debug.h"

namespace
{
mp_sched *g_active_sched = nullptr;
std::thread g_watchdog;
std::atomic<bool> g_watchdog_stop{ false };
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
    cv_.notify_all();
    cv_.wait( lock, [this, from] {
        return holder_ == from || stopping_;
    } );
    if( from != world ) {
        waiting_for_baton_[from] = 0;
    }
}

void mp_sched::wait_for_turn( const size_t who )
{
    std::unique_lock<std::mutex> lock( mutex_ );
    // Flag first, notify second: a world parked in wait_for_request must wake
    // and see that a requester appeared, or it would sleep out its whole poll.
    waiting_for_baton_[who] = 1;
    cv_.notify_all();
    cv_.wait( lock, [this, who] {
        return holder_ == who || stopping_;
    } );
    waiting_for_baton_[who] = 0;
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

bool mp_sched::wait_for_request( const int timeout_ms )
{
    std::unique_lock<std::mutex> lock( mutex_ );
    return cv_.wait_for( lock, std::chrono::milliseconds( timeout_ms ), [this] {
        for( const char flag : waiting_for_baton_ ) {
            if( flag != 0 ) {
                return true;
            }
        }
        return false;
    } );
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
    {
        std::lock_guard<std::mutex> lock( mutex_ );
        stopping_ = true;
    }
    cv_.notify_all();
    g_watchdog_stop = true;
    if( g_watchdog.joinable() ) {
        g_watchdog.join();
    }
}

bool mp_sched::stopping() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return stopping_;
}

mp_sched *mp_active_sched()
{
    return g_active_sched;
}

void mp_set_active_sched( mp_sched *const sched )
{
    g_active_sched = sched;
}

bool mp_sched_stopping()
{
    const mp_sched *const sched = mp_active_sched();
    return sched && sched->stopping();
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
    g_watchdog_stop = false;
    g_watchdog = std::thread( [&sched, threshold_ms]() {
        size_t last = sched.holder();
        auto since = std::chrono::steady_clock::now();
        bool reported = false;
        while( !g_watchdog_stop ) {
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
                DebugLog( D_WARNING, D_MAIN ) << "mp sched: participant "
                                              << ( last == mp_sched::world ? std::string( "world" ) :
                                                   std::to_string( last ) )
                                              << " has held the baton for " << held << " ms";
                reported = true;
            }
        }
    } );
}
