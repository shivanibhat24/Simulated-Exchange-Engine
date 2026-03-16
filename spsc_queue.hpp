#pragma once
#include <atomic>
#include <array>
#include <optional>

// Cache-line separated head and tail prevent false sharing between
// producer and consumer threads. Diagnose with: perf c2c record ./exchange
template<typename T, size_t Cap>
class SPSCQueue {
    static_assert((Cap & (Cap - 1)) == 0, "Cap must be power of two");

    alignas(64) std::atomic<size_t> tail_{0};  // written by producer
    alignas(64) std::atomic<size_t> head_{0};  // written by consumer
    alignas(64) std::array<T, Cap>  buf_{};

public:
    // Producer thread only
    bool push(const T& v) noexcept {
        const size_t t    = tail_.load(std::memory_order_relaxed);
        const size_t next = (t + 1) & (Cap - 1);
        if (next == head_.load(std::memory_order_acquire)) return false;
        buf_[t] = v;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer thread only
    std::optional<T> pop() noexcept {
        const size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire)) return std::nullopt;
        T v = buf_[h];
        head_.store((h + 1) & (Cap - 1), std::memory_order_release);
        return v;
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }
};
