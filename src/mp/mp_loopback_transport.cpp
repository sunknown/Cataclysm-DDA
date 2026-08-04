#include "mp/mp_loopback_transport.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace
{

using clock = std::chrono::steady_clock;

struct loopback_channel {
    // `connected()` is declared const, so the mutex guarding the queue must be
    // mutable to be lockable from that method.
    mutable std::mutex mutex;
    std::deque<std::pair<clock::time_point, std::vector<uint8_t>>> queue;
    bool closed = false;
};

class loopback_transport : public mp_transport
{
    public:
        loopback_transport( std::shared_ptr<loopback_channel> outgoing,
                            std::shared_ptr<loopback_channel> incoming,
                            const int latency_ms )
            : outgoing_( std::move( outgoing ) ), incoming_( std::move( incoming ) ),
              latency_ms_( latency_ms ) {}

        void send( std::vector<uint8_t> payload ) override {
            std::lock_guard<std::mutex> lock( outgoing_->mutex );
            if( outgoing_->closed ) {
                return;
            }
            outgoing_->queue.emplace_back( clock::now() + std::chrono::milliseconds( latency_ms_ ),
                                           std::move( payload ) );
        }

        std::optional<std::vector<uint8_t>> recv( const int timeout_ms ) override {
            const clock::time_point deadline = timeout_ms < 0
                                               ? clock::time_point::max()
                                               : clock::now() + std::chrono::milliseconds( timeout_ms );
            while( true ) {
                {
                    std::lock_guard<std::mutex> lock( incoming_->mutex );
                    if( !incoming_->queue.empty() && incoming_->queue.front().first <= clock::now() ) {
                        std::vector<uint8_t> payload = std::move( incoming_->queue.front().second );
                        incoming_->queue.pop_front();
                        return payload;
                    }
                    if( incoming_->closed && incoming_->queue.empty() ) {
                        return std::nullopt;
                    }
                }
                if( clock::now() >= deadline ) {
                    return std::nullopt;
                }
                // A one millisecond poll is coarse, but this transport only
                // ever runs in tests, where responsiveness does not matter.
                std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
            }
        }

        bool connected() const override {
            std::lock_guard<std::mutex> lock( incoming_->mutex );
            return !incoming_->closed;
        }

        void close() override {
            // Both channels are locked in a single canonical acquisition.
            // Locking them one at a time would let two threads close the two
            // ends of a pair concurrently and deadlock: a's outgoing channel
            // is b's incoming (and vice versa), so a->close() and b->close()
            // would acquire the two channels in opposite orders and could
            // interleave. std::lock takes both without imposing any order, so
            // concurrent closes can never block on each other. The unique_locks
            // are deferred so std::lock acquires them, and their destructors
            // release both channels again when this function returns.
            std::unique_lock<std::mutex> lock_out( outgoing_->mutex, std::defer_lock );
            std::unique_lock<std::mutex> lock_in( incoming_->mutex, std::defer_lock );
            std::lock( lock_out, lock_in );
            outgoing_->closed = true;
            incoming_->closed = true;
        }

    private:
        std::shared_ptr<loopback_channel> outgoing_;
        std::shared_ptr<loopback_channel> incoming_;
        int latency_ms_ = 0;
};

} // namespace

mp_loopback_pair mp_make_loopback_pair( const int simulated_latency_ms )
{
    auto a_to_b = std::make_shared<loopback_channel>();
    auto b_to_a = std::make_shared<loopback_channel>();
    mp_loopback_pair pair;
    pair.a = std::make_shared<loopback_transport>( a_to_b, b_to_a, simulated_latency_ms );
    pair.b = std::make_shared<loopback_transport>( b_to_a, a_to_b, simulated_latency_ms );
    return pair;
}
