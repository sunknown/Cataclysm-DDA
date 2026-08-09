#include "cata_catch.h"

#if defined(MULTIPLAYER)
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "mp/mp_sched.h"

// The baton's whole promise is that exactly one participant runs at a time.
// These tests state that promise in a way that fails loudly if it breaks.

TEST_CASE( "the_baton_starts_with_the_world", "[mp]" )
{
    mp_sched sched( 2 );
    CHECK( sched.holder() == mp_sched::world );
}

TEST_CASE( "only_one_participant_runs_at_a_time", "[mp]" )
{
    constexpr size_t players = 3;
    constexpr int rounds = 200;

    mp_sched sched( players );
    // Incremented on entry and decremented on exit by every participant. If two
    // ever ran at once this would exceed 1, which is exactly the bug the baton
    // exists to prevent.
    std::atomic<int> inside{ 0 };
    std::atomic<int> max_inside{ 0 };
    std::atomic<bool> failed{ false };

    std::vector<std::thread> threads;
    for( size_t i = 0; i < players; ++i ) {
        threads.emplace_back( [&sched, &inside, &max_inside, &failed, i]() {
            for( int r = 0; r < rounds; ++r ) {
                sched.wait_for_turn( i );
                if( sched.stopping() ) {
                    return;
                }
                const int now = ++inside;
                if( now > 1 ) {
                    failed = true;
                }
                int prev = max_inside.load();
                while( now > prev && !max_inside.compare_exchange_weak( prev, now ) ) {}
                --inside;
                // The ring formula (i+1)%players only ever names players, never
                // world (world == SIZE_MAX), so nothing brings the baton home to
                // main's yield_to() below unless the very last handoff breaks the
                // ring. Under a strict round robin where every worker runs the
                // same `rounds` count, that last handoff is deterministic: worker
                // players-1 takes the final turn on round rounds-1. Deviation from
                // the brief, ruled by the coordinator -- see task-6-report.md.
                if( i == players - 1 && r == rounds - 1 ) {
                    sched.release_to( i, mp_sched::world );
                } else {
                    sched.release_to( i, ( i + 1 ) % players );
                }
            }
        } );
    }

    // Kick the round robin off, then take the baton back at the end.
    sched.yield_to( mp_sched::world, 0 );
    sched.shut_down();
    for( std::thread &t : threads ) {
        t.join();
    }

    CHECK_FALSE( failed.load() );
    CHECK( max_inside.load() == 1 );
}

TEST_CASE( "yield_to_returns_only_when_the_baton_comes_back", "[mp]" )
{
    mp_sched sched( 1 );
    std::atomic<bool> player_ran{ false };

    std::thread player( [&sched, &player_ran]() {
        sched.wait_for_turn( 0 );
        player_ran = true;
        sched.release_to( 0, mp_sched::world );
    } );

    sched.yield_to( mp_sched::world, 0 );
    // The world only gets here after the player handed the baton back, so the
    // player's work is guaranteed visible -- no polling, no sleeping.
    CHECK( player_ran.load() );
    CHECK( sched.holder() == mp_sched::world );

    sched.shut_down();
    player.join();
}

TEST_CASE( "shut_down_wakes_every_waiting_participant", "[mp]" )
{
    mp_sched sched( 2 );
    std::atomic<int> returned{ 0 };

    std::vector<std::thread> threads;
    for( size_t i = 0; i < 2; ++i ) {
        threads.emplace_back( [&sched, &returned, i]() {
            sched.wait_for_turn( i );
            ++returned;
        } );
    }

    // Nobody was ever handed the baton: without shut_down both threads would
    // block forever, which is the failure mode this guards.
    sched.shut_down();
    for( std::thread &t : threads ) {
        t.join();
    }
    CHECK( returned.load() == 2 );
}

TEST_CASE( "a_parked_player_reports_waiting_for_the_baton", "[mp]" )
{
    mp_sched sched( 1 );

    std::thread player( [&sched]() {
        sched.wait_for_turn( 0 );
    } );

    // wait_for_turn sets the flag before parking, so this poll terminates.
    while( !sched.waiting_for_baton( 0 ) ) {
        std::this_thread::yield();
    }
    CHECK( sched.waiting_for_baton( 0 ) );
    // A requester is already parked: wait_for_request sees it immediately.
    CHECK( sched.wait_for_request( 100 ) );

    sched.shut_down();
    player.join();
    CHECK_FALSE( sched.waiting_for_baton( 0 ) );
}

TEST_CASE( "wait_for_request_times_out_when_nobody_is_ready", "[mp]" )
{
    mp_sched sched( 1 );

    // Nobody is parked and nobody becomes ready within the timeout: the world
    // must not spin, it must return false so do_turn can close out a waiting
    // turn.
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE( sched.wait_for_request( 50 ) );
    const auto elapsed = std::chrono::steady_clock::now() - start;
    // Actually waited out the timeout rather than returning on a spurious wake.
    CHECK( elapsed >= std::chrono::milliseconds( 45 ) );
}

TEST_CASE( "a_declared_wait_is_off_by_default", "[mp]" )
{
    mp_sched sched( 1 );

    // Default false, so a scheduler nobody told about waiting turns still gets
    // its stalls reported. The flag suppresses the watchdog, and a suppression
    // that defaults to on would hide the one bug the watchdog exists to find.
    CHECK_FALSE( sched.world_waiting() );
    sched.set_world_waiting( true );
    CHECK( sched.world_waiting() );
    sched.set_world_waiting( false );
    CHECK_FALSE( sched.world_waiting() );
}

TEST_CASE( "mp_sched_stopping_is_false_without_a_scheduler", "[mp]" )
{
    // player_act's loop condition calls this on every iteration in single
    // player, where there is no scheduler at all.
    mp_set_active_sched( nullptr );
    CHECK_FALSE( mp_sched_stopping() );

    mp_sched sched( 1 );
    mp_set_active_sched( &sched );
    CHECK_FALSE( mp_sched_stopping() );
    sched.shut_down();
    CHECK( mp_sched_stopping() );
    mp_set_active_sched( nullptr );
}

#endif // defined(MULTIPLAYER)
