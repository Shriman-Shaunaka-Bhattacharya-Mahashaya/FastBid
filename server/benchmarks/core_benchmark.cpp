// Core-only workload. Heap construction and sample sorting are outside timing.
// Global allocation guard covers standard C++ allocation, including aligned new.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include "core/matching_engine.hpp"
#include "core/product_catalog.hpp"
static bool guarded = false;
static std::size_t allocations = 0;
void* operator new(std::size_t n) { if (guarded) ++allocations; if (void* p = std::malloc(n)) return p; throw std::bad_alloc(); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t n, std::align_val_t a) { if (guarded) ++allocations; void* p{}; if (posix_memalign(&p, static_cast<std::size_t>(a), n)) throw std::bad_alloc(); return p; }
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
int main() {
    using namespace fastbid;
    using E = MatchingEngine<PRODUCT_LEVELS, 1048576, 65536, PRODUCT_COUNT>;
    auto in = std::make_unique<E::InboundQueue>(); auto out = std::make_unique<E::OutboundQueue>();
    auto engine = std::make_unique<E>(*in, *out,
        MatchingEngineConfig{PRODUCT_BOOKS[0], std::span(PRODUCT_BOOKS).subspan(1)});
    constexpr std::size_t N = 100000;
    static uint64_t ns[N];
    auto run = [&](std::size_t i) {
        const auto& product = PRODUCT_BOOKS[(i / 2) % PRODUCT_COUNT];
        OrderMessage m{}; m.type = MessageType::NEW_ORDER; m.instrument_id = product.instrument_id;
        m.side = i % 2 ? Side::SELL : Side::BUY; m.order_type = OrderType::LIMIT; m.price = product.reference_price; m.quantity = 100;
        m.client_order_id = i + 1;
        if (!in->try_push(m)) std::abort();
        engine->process_batch(); EngineEvent e{}; while (out->try_pop(e)) {}
    };
    for (std::size_t i = 0; i < 10000; ++i) run(i);
    guarded = true;
    const auto all_start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < N; ++i) {
        const auto start = std::chrono::steady_clock::now(); run(i);
        ns[i] = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - all_start).count();
    guarded = false; std::sort(ns, ns + N);
    std::printf("alternating rest/full-fill, production capacities, %zu requests: p50=%llu p99=%llu p999=%llu ns, %.0f req/s, C++ allocations=%zu\n", N,
        static_cast<unsigned long long>(ns[N/2]), static_cast<unsigned long long>(ns[N*99/100]), static_cast<unsigned long long>(ns[N*999/1000]), N / elapsed, allocations);
    return allocations || engine->pool().allocated() ? 1 : 0;
}
