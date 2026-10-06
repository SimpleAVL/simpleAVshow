#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <vector>

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

// Capacity is chosen while disarmed; push/pop never allocate.
template<class T> class DynamicQueue {
public:
    void reset(std::size_t capacity) { data_.resize(capacity + 1); head_.store(0); tail_.store(0); }
    bool push(const T& item) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next = (head + 1) % data_.size();
        if (next == tail_.load(std::memory_order_acquire)) return false;
        data_[head] = item; head_.store(next, std::memory_order_release); return true;
    }
    bool pop(T& item) noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        item = data_[tail]; tail_.store((tail + 1) % data_.size(), std::memory_order_release); return true;
    }
private:
    std::vector<T> data_{1};
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};
}
