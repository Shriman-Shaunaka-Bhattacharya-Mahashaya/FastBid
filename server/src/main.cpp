#include <atomic>
#include <memory>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <thread>
#include <sys/mman.h>
#include "core/matching_engine.hpp"
#include "core/product_catalog.hpp"
#include "instrumentation/stats.hpp"
#include "platform/cycle_clock.hpp"
#include "platform/core_allocator.hpp"
#include "network/tcp_gateway.hpp"
#include "network/udp_publisher.hpp"
namespace {
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> stop_requested{false};
void signal_handler(int) { stop_requested.store(true, std::memory_order_relaxed); }
const char* env(const char* key, const char* fallback) { auto v = std::getenv(key); return v ? v : fallback; }
uint16_t port(const char* key, const char* fallback) {
    const char* value = env(key, fallback); char* end{}; errno = 0;
    auto p = std::strtol(value, &end, 10);
    if (errno || *end || p < 1 || p > 65535) { std::fprintf(stderr, "Invalid %s\n", key); std::exit(1); }
    return static_cast<uint16_t>(p);
}
}
int main() {
    using namespace fastbid;
    using Clock = std::chrono::steady_clock;
    using Engine = MatchingEngine<PRODUCT_LEVELS, 1048576, 65536, PRODUCT_COUNT>;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::signal(SIGINT, signal_handler); std::signal(SIGTERM, signal_handler);
    CycleClock::calibrate();
    CoreAllocator allocator;
    static Engine::InboundQueue inbound;
    static Engine::OutboundQueue outbound;
    static SPSCQueue<MarketSnapshot, 64> snapshots;
    static StatsCollector<> stats;
    std::atomic<bool> ingress_stopped{false}, engine_done{false};
    std::atomic<uint64_t> processed_total{0};
    auto engine = std::make_unique<Engine>(inbound, outbound,
        MatchingEngineConfig{PRODUCT_BOOKS[0], std::span(PRODUCT_BOOKS).subspan(1)});
    TCPGateway gateway(inbound, outbound, {env("FASTBID_TCP_HOST", "127.0.0.1"), port("FASTBID_TCP_PORT", "9000"), 1024});
    UDPPublisher publisher({env("FASTBID_UDP_MCAST_ADDR", "239.255.0.1"), port("FASTBID_UDP_MCAST_PORT", "5000")});
    gateway.init(); publisher.init();
    if (std::string(env("FASTBID_LOCK_MEMORY", "0")) == "1" && mlockall(MCL_CURRENT)) std::perror("mlockall");
    std::thread worker([&] {
        CoreAllocator::apply_to_current_thread(allocator.get_engine_assignment());
        auto next_snapshot = Clock::now();
        uint64_t request_start = 0, count = 0;
        bool measuring = false;
        while (!ingress_stopped.load(std::memory_order_acquire) || !inbound.empty() || engine->has_pending()) {
            auto start = CycleClock::now();
            auto n = engine->process_batch(1);
            if (n) { request_start = start; measuring = true; ++count; processed_total.store(count, std::memory_order_relaxed); }
            if (measuring && !engine->has_pending()) { stats.record(CycleClock::to_nanoseconds(CycleClock::now() - request_start)); measuring = false; }
            const auto now = Clock::now();
            if (!engine->has_pending() && now >= next_snapshot) {
                for (const auto& product : PRODUCT_BOOKS) {
                    MarketSnapshot snap{}; engine->book(product.instrument_id)->snapshot(snap);
                    (void)snapshots.try_push(snap);
                }
                next_snapshot = now + std::chrono::milliseconds(50);
            }
            if (!n && (!engine->has_pending() || outbound.available() < 2)) std::this_thread::yield();
        }
        engine_done.store(true, std::memory_order_release);
    });
    CoreAllocator::apply_to_current_thread(allocator.get_io_assignment());
    std::array<MarketSnapshot, PRODUCT_COUNT> latest{};
    std::array<bool, PRODUCT_COUNT> snapshot_ready{};
    LatencyStats latency{};
    auto last_stats = Clock::now(), next_publish = last_stats, shutdown_deadline = Clock::time_point::max();
    uint64_t previous_count = 0;
    std::puts("[Main] Ready; SIGINT/SIGTERM stops admission and drains accepted requests.");
    while (true) {
        if (stop_requested.load(std::memory_order_relaxed) && !ingress_stopped.exchange(true, std::memory_order_acq_rel)) std::puts("[Main] Draining accepted requests...");
        const bool stopping = ingress_stopped.load(std::memory_order_relaxed);
        const auto activity = gateway.poll(!stopping);
        EngineEvent e{}; std::size_t reports = 0;
        while (reports < 4096 && outbound.try_pop(e)) { gateway.send_event(e); publisher.publish_event(e); ++reports; }
        MarketSnapshot incoming{};
        while (snapshots.try_pop(incoming)) {
            for (std::size_t i = 0; i < PRODUCT_COUNT; ++i) {
                if (PRODUCT_BOOKS[i].instrument_id == incoming.header.instrument_id) {
                    latest[i] = incoming; snapshot_ready[i] = true; break;
                }
            }
        }
        const auto now = Clock::now();
        if (now - last_stats >= std::chrono::seconds(1)) {
            latency = stats.compute();
            const auto count = processed_total.load(std::memory_order_relaxed);
            latency.orders_per_sec = static_cast<uint64_t>(static_cast<double>(count - previous_count) / std::chrono::duration<double>(now - last_stats).count());
            previous_count = count; last_stats = now;
            std::printf("[Stats] requests/s=%llu samples=%llu p50=%llu p99=%llu ns sample_drops=%llu udp_drops=%llu\n",
                static_cast<unsigned long long>(latency.orders_per_sec), static_cast<unsigned long long>(latency.sample_count),
                static_cast<unsigned long long>(latency.p50_ns), static_cast<unsigned long long>(latency.p99_ns),
                static_cast<unsigned long long>(stats.dropped()), static_cast<unsigned long long>(publisher.dropped()));
        }
        if (now >= next_publish) {
            for (std::size_t i = 0; i < PRODUCT_COUNT; ++i) {
                if (!snapshot_ready[i]) continue;
                snapshot_ready[i] = false;
                auto& snap = latest[i];
                snap.p50_ns = latency.p50_ns; snap.p99_ns = latency.p99_ns; snap.orders_per_sec = latency.orders_per_sec;
                snap.samples = latency.sample_count; snap.dropped_samples = stats.dropped(); publisher.publish_snapshot(snap);
            }
            next_publish = now + std::chrono::milliseconds(50);
        }
        if (engine_done.load(std::memory_order_acquire) && outbound.empty()) {
            if (shutdown_deadline == Clock::time_point::max()) shutdown_deadline = now + std::chrono::seconds(2);
            if (!gateway.output_pending()) break;
            if (now >= shutdown_deadline) { std::fputs("[Main] Shutdown report flush deadline exceeded\n", stderr); break; }
        }
        if (!activity && !reports) std::this_thread::yield();
    }
    worker.join();
    std::puts("[Main] Shutdown complete; in-memory resting orders are discarded.");
}
