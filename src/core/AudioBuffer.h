#pragma once
#include "core/Model.h"
#include <atomic>

namespace sav {
// One disk worker, one callback consumer. Slots are exclusively owned while
// writing/reading; publication uses release/acquire, never a callback mutex.
class AudioBuffer {
public:
    static constexpr Frame blockFrames = 8192;
    static constexpr std::size_t slotCount = 4;
    AudioBuffer(std::shared_ptr<const AudioData> data, Frame offset, bool loop, Frame initial);
    bool sample(Frame frame, float& left, float& right) noexcept;
    void service(); // Disk worker only (or while the callback is closed).
    bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }
private:
    enum Ownership { Empty, Writing, Ready, Reading };
    struct Slot {
        std::atomic<int> state{Empty};
        Frame first = -1;
        std::array<float, blockFrames * 2> samples{};
    };
    std::shared_ptr<const AudioData> data_;
    Frame offset_;
    bool loop_;
    std::array<Slot, slotCount> slots_;
    std::atomic<Frame> requested_;
    std::atomic<bool> failed_{false};
    int current_ = -1; // Callback only.
};
}
