#pragma once
#include <array>
#include <atomic>
#include <cstddef>

namespace sav {
// One producer and one consumer only. No allocation or locks in the audio callback.
template<class T, std::size_t N> class Queue {
public:
    bool push(const T& item) noexcept {
        auto head = head_.load(std::memory_order_relaxed);
        auto next = (head + 1) % N;
        if (next == tail_.load(std::memory_order_acquire)) return false;
        data_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }
    bool pop(T& item) noexcept {
        auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        item = data_[tail];
        tail_.store((tail + 1) % N, std::memory_order_release);
        return true;
    }
    void clear() noexcept { T item; while (pop(item)) {} }
private:
    std::array<T, N> data_{};
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};
}
