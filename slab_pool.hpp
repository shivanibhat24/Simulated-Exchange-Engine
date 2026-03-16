#pragma once
#include <array>
#include <cassert>
#include <cstddef>
#include <stdexcept>

// Pre-allocates N objects at startup. alloc() and free() are O(1),
// involve no system calls, and never fragment. The free list is an
// intrusive stack: unused slots store their own index in the first bytes.
template<typename T, size_t N>
class SlabPool {
    union Slot {
        T       obj;
        size_t  next_free;
        Slot() : next_free(0) {}
    };

    std::array<Slot, N> slots_;
    size_t              free_head_ = 0;
    size_t              free_count_ = N;

public:
    SlabPool() {
        for (size_t i = 0; i < N - 1; ++i)
            slots_[i].next_free = i + 1;
        slots_[N - 1].next_free = N; // sentinel
    }

    T* alloc() {
        if (free_count_ == 0) throw std::bad_alloc{};
        Slot& s    = slots_[free_head_];
        free_head_ = s.next_free;
        --free_count_;
        new (&s.obj) T{};
        return &s.obj;
    }

    void free(T* p) {
        auto* s       = reinterpret_cast<Slot*>(p);
        p->~T();
        s->next_free  = free_head_;
        free_head_    = static_cast<size_t>(s - slots_.data());
        ++free_count_;
    }

    size_t available() const noexcept { return free_count_; }
};
