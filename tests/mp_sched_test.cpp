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
    CHECK( sched.wait_for_request( 100, { true } ) );
    // ...but only one the world still owes the turn to. A player who already
    // acted re-parks at once, and waking on them is the 100% CPU spin of a
    // waiting turn: the world must sleep out the whole timeout instead.
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE( sched.wait_for_request( 50, { false } ) );
    CHECK( std::chrono::steady_clock::now() - start >= std::chrono::milliseconds( 45 ) );

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
    CHECK_FALSE( sched.wait_for_request( 50, { true } ) );
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

TEST_CASE( "wait_for_request_wakes_when_a_player_parks_after_the_world_is_already_waiting",
           "[mp]" )
{
    // a_parked_player_reports_waiting_for_the_baton only exercises
    // wait_for_request's initial predicate check: the player is already
    // parked before the world calls it, so seeing the flag set never has to
    // rely on being woken. This is the reverse and more interesting
    // ordering -- the world blocks first, on an empty waiting_for_baton_,
    // and only later does a player arrive and set it -- so wait_for_request
    // must actually be woken by wait_for_turn's notify, not merely observe a
    // flag that was already set when it first checked.
    mp_sched sched( 1 );
    std::atomic<bool> world_parked{ false };
    std::atomic<bool> ready{ false };

    std::thread world_side( [&]() {
        world_parked = true;
        ready = sched.wait_for_request( 5000, { true } );
    } );

    while( !world_parked.load() ) {
        std::this_thread::yield();
    }
    // Not a correctness requirement -- wait_for_request would still be
    // correct if the player beat it to the mutex -- but gives the world a
    // real chance to be inside cv_.wait_for before the player arrives, which
    // is the ordering this test is aimed at.
    std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );

    std::thread player( [&]() {
        sched.wait_for_turn( 0 );
    } );

    world_side.join();
    CHECK( ready.load() );

    // The player is still parked (nobody ever handed it the baton); shut_down
    // is what lets it leave.
    sched.shut_down();
    player.join();
}

TEST_CASE( "mp_set_active_sched_is_visible_to_a_different_thread", "[mp]" )
{
    // Finding 3: mp_set_active_sched is called by the world thread while
    // mp_active_sched/mp_sched_stopping are read by player threads, with no
    // mutex between them -- only std::atomic<mp_sched *> makes that legal.
    // mp_sched_stopping_is_false_without_a_scheduler exercises the same
    // functions but entirely on one thread, so it could not have exercised
    // the cross-thread race that made the plain pointer a bug: this test
    // creates that race on purpose so a TSAN run actually covers it.
    mp_sched sched( 1 );
    mp_set_active_sched( nullptr );

    std::atomic<bool> reader_saw_it{ false };
    std::atomic<bool> stop_reader{ false };

    std::thread reader( [&]() {
        while( !stop_reader.load() ) {
            if( mp_active_sched() == &sched ) {
                reader_saw_it = true;
            }
            // The function a player thread actually calls every loop
            // iteration in the real integration -- exercise that path, not
            // just the raw accessor.
            mp_sched_stopping();
        }
    } );

    // Racing with the reader's loop above on purpose: this is the write half
    // of the exact cross-thread access finding 3 was about.
    mp_set_active_sched( &sched );

    // Generous, not tight: this isn't asserting a particular interleaving,
    // only that the write becomes visible to the concurrently-spinning
    // reader within a bound loose enough not to flake under load.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 2 );
    while( !reader_saw_it.load() && std::chrono::steady_clock::now() < deadline ) {
        std::this_thread::yield();
    }
    stop_reader = true;
    reader.join();

    CHECK( reader_saw_it.load() );
    mp_set_active_sched( nullptr );
}

TEST_CASE( "shut_down_does_not_return_until_a_parked_waiter_has_left", "[mp]" )
{
    // Regression test for finding 1: shut_down() (and so the destructor)
    // must not return -- and the object must not start being destroyed --
    // while a wait_for_turn caller is still between waking inside cv_.wait
    // and actually leaving the function. Destroying sched at the end of the
    // inner scope, before player is joined, is exactly the ordering finding
    // 1 flagged as a use-after-free: without shut_down() draining parked_,
    // the woken player could reacquire a destroyed mutex_ or write through a
    // destroyed waiting_for_baton_. Repeated, to give a race that only shows
    // up intermittently more chances in one run.
    for( int rep = 0; rep < 20; ++rep ) {
        std::atomic<bool> started{ false };
        std::thread player;
        {
            mp_sched sched( 1 );
            player = std::thread( [&sched, &started]() {
                started = true;
                sched.wait_for_turn( 0 );
            } );
            while( !started.load() ) {
                std::this_thread::yield();
            }
            while( !sched.waiting_for_baton( 0 ) ) {
                std::this_thread::yield();
            }
            // sched's destructor runs here, at the end of this scope, before
            // player is joined below.
        }
        player.join();
    }
    SUCCEED( "every destructor returned only after its parked waiter had left" );
}

TEST_CASE( "a_baton_handoff_is_traced_with_its_generation", "[mp]" )
{
    mp_sched sched( 2 );
    mp_set_active_sched( &sched );
    const size_t gen0 = sched.holder_generation();

    sched.release_to( mp_sched::world, 1 );
    CHECK( sched.holder_generation() == gen0 + 1 );
    mp_sched_trace_phase( "turn-start" );

    const std::vector<std::string> trace = sched.trace_snapshot();
    REQUIRE( trace.size() >= 2 );
    CHECK( trace.back().find( "turn-start" ) != std::string::npos );

    sched.release_to( 1, mp_sched::world );
    CHECK( sched.holder_generation() == gen0 + 2 );
    mp_set_active_sched( nullptr );
}

TEST_CASE( "rapid_handoffs_do_not_report_a_stall", "[mp]" )
{
    mp_sched sched( 2 );
    mp_set_active_sched( &sched );

    // Ping-pong faster than the watchdog's 100 ms poll: the world hands the
    // baton to the player and waits for it back. The old watchdog sampled
    // holder() once per poll and so saw `world` on nearly every poll while the
    // generation was moving constantly; it accumulated a stall that never
    // existed.
    std::thread player( [&]() {
        while( true ) {
            sched.wait_for_turn( 1 );
            if( mp_sched_stopping() ) {
                return;
            }
            sched.release_to( 1, mp_sched::world );
        }
    } );
    // Start the watchdog only once the player is parked: the setup gap before
    // the first handoff is not part of the property under test.
    while( !sched.waiting_for_baton( 1 ) ) {
        std::this_thread::yield();
    }
    mp_sched_start_watchdog( sched, /*threshold_ms=*/300 );

    // 1000 ms, not the 300 ms the old test used: a scheduling stall under
    // `-j $(nproc)` (or a loaded CI box) must not turn a fast ping-pong into a
    // legitimate report. Ten watchdog polls fit in this window, while a
    // watchdog that still counted sampled stalls would cross 300 ms in three.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( 1000 );
    while( std::chrono::steady_clock::now() < deadline ) {
        sched.yield_to( mp_sched::world, 1 );
    }
    // Park the world on purpose for the teardown, so a report during shutdown
    // (not part of this property) cannot make the assertion below flap.
    sched.set_world_waiting( true );
    sched.shut_down();
    player.join();
    mp_set_active_sched( nullptr );

    CHECK( sched.watchdog_report_count() == 0 );
}

TEST_CASE( "the_watchdog_leaves_quietly_when_the_session_stops", "[mp]" )
{
    mp_sched sched( 1 );
    // Above the whole test's lifetime and far above the watchdog's 100 ms poll
    // period, so an accumulated stall cannot be what the assertion below
    // catches; a report here would mean the watchdog reported during shutdown,
    // after shut_down() set stopping_.
    mp_sched_start_watchdog( sched, /*threshold_ms=*/1000 );
    std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
    // shut_down() joins the watchdog; a hang here fails the test by timeout.
    sched.shut_down();
    CHECK( sched.watchdog_report_count() == 0 );
}

TEST_CASE( "the_watchdog_reports_a_stall_once_not_repeatedly", "[mp]" )
{
    // Finding 4: nothing exercised the watchdog before this. threshold_ms is
    // small to keep the test fast; watchdog_report_count() is a
    // test-observable counter (mp_sched.h) so this does not need to scrape
    // DebugLog's output stream to know whether a report fired.
    mp_sched sched( 1 );
    mp_sched_start_watchdog( sched, /*threshold_ms=*/10 );

    // Nobody ever takes the baton: the world holds it from construction,
    // unmoving, which is exactly a stall. Generous bound, not a tight
    // margin -- the watchdog's own poll cadence is a fixed 100 ms, so this
    // just needs to outlast a couple of polls.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 2 );
    while( sched.watchdog_report_count() == 0 && std::chrono::steady_clock::now() < deadline ) {
        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }
    REQUIRE( sched.watchdog_report_count() == 1 );

    // Wait through several more of the watchdog's 100 ms poll cycles. The
    // holder still has not moved, so a watchdog that reported on every poll
    // instead of once per stall would have incremented by now; one that
    // reports once per stall stays at 1.
    std::this_thread::sleep_for( std::chrono::milliseconds( 350 ) );
    CHECK( sched.watchdog_report_count() == 1 );

    sched.shut_down();
}

TEST_CASE( "a_declared_wait_suppresses_the_watchdogs_report", "[mp]" )
{
    mp_sched sched( 1 );
    sched.set_world_waiting( true );
    mp_sched_start_watchdog( sched, /*threshold_ms=*/10 );

    // Same stall as above -- the world holds the baton, unmoving -- but
    // declared on purpose this time, so it must never be reported.
    std::this_thread::sleep_for( std::chrono::milliseconds( 400 ) );
    CHECK( sched.watchdog_report_count() == 0 );

    sched.shut_down();
}

// Blocks until every player of `sched` is parked on wait_for_turn, so a
// following shut_down() wakes them all at once -- the broadcast the unwind right
// exists to serialise.
static void wait_until_all_parked( const mp_sched &sched, const size_t players )
{
    for( size_t i = 0; i < players; ++i ) {
        while( !sched.waiting_for_baton( i ) ) {
            std::this_thread::yield();
        }
    }
}

TEST_CASE( "only_one_player_unwinds_at_a_time_after_a_shutdown", "[mp]" )
{
    // shut_down() wakes every parked player at once, by design. Each one here
    // then does what an unwinding player does: takes the right, re-enters it
    // (a second menu frame asking for input), and runs for a while. Without the
    // right all three would be inside together -- they wake in the same
    // instant and each stays inside for milliseconds. Each also tries to
    // release its neighbour's right while holding its own, which must be a
    // no-op: a release that could free someone else's right would let a
    // third player in alongside the holder. Repeated, because the property is
    // about an interleaving.
    constexpr size_t players = 3;
    for( int rep = 0; rep < 10; ++rep ) {
        mp_sched sched( players );
        std::atomic<int> inside{ 0 };
        std::atomic<int> max_inside{ 0 };
        std::atomic<int> took{ 0 };
        std::atomic<int> reentered{ 0 };
        std::vector<std::thread> threads;
        for( size_t i = 0; i < players; ++i ) {
            threads.emplace_back( [&, i]() {
                sched.wait_for_turn( i );
                if( !sched.unwind_if_stopping( i ) ) {
                    return;
                }
                ++took;
                const int now = ++inside;
                int prev = max_inside.load();
                while( now > prev && !max_inside.compare_exchange_weak( prev, now ) ) {}
                if( sched.unwind_if_stopping( i ) ) {
                    ++reentered;
                }
                sched.release_unwind( ( i + 1 ) % players );
                std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
                --inside;
                sched.release_unwind( i );
            } );
        }
        wait_until_all_parked( sched, players );
        sched.shut_down();
        for( std::thread &t : threads ) {
            t.join();
        }
        CHECK( took.load() == static_cast<int>( players ) );
        CHECK( reentered.load() == static_cast<int>( players ) );
        CHECK( max_inside.load() == 1 );
    }
}

#endif // defined(MULTIPLAYER)
