#include "core/AudioBuffer.h"

namespace sav {
AudioBuffer::AudioBuffer(std::shared_ptr<const AudioData> data, Frame offset, bool loop, Frame initial)
    : data_(std::move(data)), offset_(offset), loop_(loop), requested_(initial / blockFrames * blockFrames) {}

void AudioBuffer::release(int& held, int other) noexcept {
    if (held >= 0 && held != other) slots_[held].state.store(Ready, std::memory_order_release);
    held = -1;
}
void AudioBuffer::request(Frame frame) noexcept {
    const Frame first = frame / blockFrames * blockFrames;
    if (current_ >= 0 && slots_[current_].first != first) release(current_, tail_);
    requested_.store(first, std::memory_order_release);
}
bool AudioBuffer::retarget(Frame frame) noexcept {
    const Frame first = frame / blockFrames * blockFrames;
    release(tail_, current_); release(current_, tail_);
    requested_.store(first, std::memory_order_release); failing_ = false;
    return std::any_of(slots_.begin(), slots_.end(), [first](const Slot& slot) { return slot.state.load(std::memory_order_acquire) == Ready && slot.first == first; });
}
bool AudioBuffer::read(int& held, int other, Frame frame, float& left, float& right) noexcept {
    const Frame first = frame / blockFrames * blockFrames;
    if (held >= 0 && slots_[held].first != first) release(held, other);
    if (held < 0 && other >= 0 && slots_[other].first == first) held = other;
    if (held < 0) {
        for (std::size_t i = 0; i < slotCount; ++i) {
            int ready = Ready;
            if (!slots_[i].state.compare_exchange_strong(ready, Reading, std::memory_order_acquire)) continue;
            if (slots_[i].first == first) { held = static_cast<int>(i); break; }
            slots_[i].state.store(Ready, std::memory_order_release);
        }
    }
    if (held < 0) return false;
    const auto index = static_cast<std::size_t>(frame - first) * 2;
    left = slots_[held].samples[index]; right = slots_[held].samples[index + 1];
    return true;
}

void AudioBuffer::prebuffer() { strict_ = true; service(); strict_ = false; }
bool AudioBuffer::service() {
    if (failed()) return true;
    const auto now = std::chrono::steady_clock::now();
    if (failing_ && now < retryAt_) return true;
    const Frame first = requested_.load(std::memory_order_acquire);
    std::array<Frame, slotCount> wanted{};
    wanted.fill(-1);
    Frame next = first;
    for (std::size_t i = 0; i < slotCount; ++i) {
        if (next >= data_->length()) {
            if (!loop_) break;
            next = offset_ / blockFrames * blockFrames;
        }
        wanted[i] = next; next += blockFrames;
    }
    for (const Frame block : wanted) {
        if (block < 0) continue;
        bool present = false;
        for (auto& slot : slots_) {
            const int state = slot.state.load(std::memory_order_acquire);
            if ((state == Ready || state == Reading) && slot.first == block) { present = true; break; }
        }
        if (present) continue;
        for (auto& slot : slots_) {
            int state = slot.state.load(std::memory_order_acquire);
            if (state != Empty && (state != Ready || std::find(wanted.begin(), wanted.end(), slot.first) != wanted.end())) continue;
            if (!slot.state.compare_exchange_strong(state, Writing, std::memory_order_acquire)) continue;
            try {
                slot.samples.fill(0);
                data_->storage->read(block, slot.samples.data(), static_cast<std::size_t>(std::min(blockFrames, data_->length() - block)));
                slot.first = block;
                slot.state.store(Ready, std::memory_order_release);
                failing_ = false;
            } catch (const AudioSourceChanged&) {
                slot.state.store(Empty, std::memory_order_release);
                failed_.store(true, std::memory_order_release);
                return true;
            } catch (...) {
                slot.state.store(Empty, std::memory_order_release);
                if (!failing_) firstFailure_ = now;
                failing_ = true; retryAt_ = now + retryInterval;
                if (strict_ || now - firstFailure_ >= retryWindow) failed_.store(true, std::memory_order_release);
                return false;
            }
            break;
        }
    }
    return true;
}
}
