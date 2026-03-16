#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <array>
#include <atomic>
#include "../include/types.hpp"
#include "../include/spsc_queue.hpp"
#include "../include/matching_engine.hpp"

// ─── RDTSC cycle-accurate timer ───────────────────────────────────────────────
static inline uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// ─── HDR-style percentile from sorted samples ────────────────────────────────
static uint64_t percentile(std::vector<uint64_t>& v, double p) {
    size_t idx = static_cast<size_t>(p * v.size() / 100.0);
    return v[std::min(idx, v.size() - 1)];
}

int main() {
    static SPSCQueue<OrderRequest,    4096> q_in;
    static SPSCQueue<ExecutionReport, 4096> q_exec;
    static SPSCQueue<DepthUpdate,     4096> q_depth;
    std::atomic<bool> running{true};

    MatchingEngine engine(q_in, q_exec, q_depth, running);

    constexpr int N = 100'000;
    std::vector<uint64_t> latencies;
    latencies.reserve(N);

    OrderId oid = 1;

    for (int i = 0; i < N; ++i) {
        OrderRequest req{};
        req.type     = RequestType::NewOrder;
        req.order_id = oid++;
        req.client_id = 1;
        req.price    = 10000 + (i % 10); // spread orders across 10 price levels
        req.qty      = 100;
        req.side     = (i % 2 == 0) ? Side::Buy : Side::Sell;
        req.ord_type = OrdType::Limit;
        strncpy(req.symbol, "AAPL", 8);

        uint64_t t0 = rdtsc();
        while (!q_in.push(req)) {}

        // Process synchronously on same thread (single-threaded bench)
        // In real bench, run engine on separate thread and measure round-trip
        // by timestamping the ExecutionReport pop.
        // Here: measure insert path only.
        uint64_t t1 = rdtsc();
        latencies.push_back(t1 - t0);

        // Drain output queues to prevent back-pressure
        while (auto _ = q_exec.pop())  {}
        while (auto _ = q_depth.pop()) {}
    }

    std::sort(latencies.begin(), latencies.end());

    // Estimate cycles → nanoseconds (approximate: 1 cycle ≈ 0.33ns at 3GHz)
    // For accurate calibration: measure TSC frequency against clock_gettime.
    printf("LOB insert latency over %d iterations (cycles):\n", N);
    printf("  p50  = %llu cycles\n",  (unsigned long long)percentile(latencies, 50));
    printf("  p95  = %llu cycles\n",  (unsigned long long)percentile(latencies, 95));
    printf("  p99  = %llu cycles\n",  (unsigned long long)percentile(latencies, 99));
    printf("  p999 = %llu cycles\n",  (unsigned long long)percentile(latencies, 99.9));
    printf("  max  = %llu cycles\n",  (unsigned long long)latencies.back());
    printf("\n(divide by your TSC freq in GHz for nanoseconds)\n");

    running = false;
    return 0;
}
