#pragma once
#include "core/Model.h"
#include "core/Queue.h"
#include <atomic>

namespace sav {
static_assert(std::atomic<Frame>::is_always_lock_free && std::atomic<float>::is_always_lock_free,
    "The audio transport requires lock-free atomics");
enum class State { Stopped, Playing, Paused, Held, Ended };
enum class Action { Play, Pause, Seek, StopAll };
struct Command { Action action; Frame position = 0; };
struct MidiEvent { int marker = -1; Frame time = 0; std::uint64_t epoch = 0; };

class Transport {
public:
    // The device MUST be closed before configure. The Show remains immutable until closed.
    void configure(std::shared_ptr<const Show> show, Frame position = 0);
    bool command(Command command) noexcept { return commands_.push(command); }
    void render(float* output, unsigned count, unsigned channels) noexcept;
    bool takeMidi(MidiEvent& event) noexcept { return midi_.pop(event); }
    Frame position() const noexcept { return publishedPosition_.load(std::memory_order_acquire); }
    State state() const noexcept { return publishedState_.load(std::memory_order_acquire); }
    int heldMarker() const noexcept { return publishedMarker_.load(); }
    int asyncVoices() const noexcept { return publishedVoices_.load(); }
    float peak() const noexcept { return publishedPeak_.load(); }
    bool clipped() const noexcept { return clipped_.load(); }
    bool eventOverflow() const noexcept { return overflow_.load(); }
    std::uint64_t epoch() const noexcept { return epoch_.load(std::memory_order_acquire); }
private:
    enum class VoiceState { Dormant, Running, Finished };
    struct Voice { VoiceState state = VoiceState::Dormant; Frame source = 0; };
    std::shared_ptr<const Show> show_;
    std::array<Voice, maxClips> voices_{};
    std::array<bool, maxMarkers> fired_{};
    Queue<Command, 128> commands_;
    Queue<MidiEvent, 1024> midi_;
    Frame position_ = 0;
    State state_ = State::Stopped;
    int heldMarker_ = -1;
    std::atomic<Frame> publishedPosition_{0};
    std::atomic<State> publishedState_{State::Stopped};
    std::atomic<int> publishedMarker_{-1}, publishedVoices_{0};
    std::atomic<float> publishedPeak_{0};
    std::atomic<bool> clipped_{false}, overflow_{false};
    std::atomic<std::uint64_t> epoch_{0};
    void apply(Command command) noexcept;
    void dispatch(int marker) noexcept;
    void publish(float peak) noexcept;
};
}
