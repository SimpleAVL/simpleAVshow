#pragma once
#include "core/Model.h"
#include <atomic>
#include <chrono>

namespace sav {
// One disk worker, one callback consumer. Slots are exclusively owned while
// writing/reading; publication uses release/acquire, never a callback mutex.
class AudioBuffer {
public:
    static constexpr Frame blockFrames = 8192;
    static constexpr std::size_t slotCount = 4;
    // Transient read failures retry within the read-ahead; they latch once they persist this long.
    static constexpr std::chrono::milliseconds retryInterval{25}, retryWindow{2000};
    AudioBuffer(std::shared_ptr<const AudioData> data, Frame offset, bool loop, Frame initial);
    bool sample(Frame frame, float& left, float& right) noexcept;
    // Callback only: read ahead from this source frame before the clip needs it.
    void request(Frame frame) noexcept;
    // Disk worker only (or while the callback is closed). Returns false when a read attempt failed transiently.
    bool service();
    // Pre-roll while the callback is closed; any failure latches.
    void prebuffer();
    bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }
    // While the callback and worker are stopped: read ahead from a new entry, keeping resident
    // blocks. Returns whether the entry block is already resident.
    bool retarget(Frame frame) noexcept;
    bool matches(const AudioData* data, Frame offset, bool loop) const noexcept { return data_.get() == data && offset_ == offset && loop_ == loop; }
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
    std::chrono::steady_clock::time_point firstFailure_{}, retryAt_{}; // Worker only.
    bool failing_ = false, strict_ = false;
    void release() noexcept;
};
}
