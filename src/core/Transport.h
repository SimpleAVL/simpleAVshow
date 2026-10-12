#pragma once
#include "core/Model.h"
#include "core/Queue.h"
#include "core/AudioBuffer.h"
#include <atomic>
#include <thread>

namespace sav {
static_assert(std::atomic<Frame>::is_always_lock_free && std::atomic<float>::is_always_lock_free,
    "The audio transport requires lock-free atomics");
enum class State { Stopped, Playing, Paused, Held, Ended };
enum class Action { Play, Pause, Seek, StopAll };
struct Command { Action action; Frame position = 0; };
struct MidiEvent { int marker = -1; Frame time = 0; std::uint64_t epoch = 0; };

class Transport {
public:
    static constexpr int fadeFrames = 120; // 2.5 ms de-click ramps for cut and mid-source voices.
    ~Transport();
    // The device and MIDI consumer MUST be stopped before configure.
    // The Show remains immutable until the device is closed.
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
    unsigned bufferWaits() const noexcept { return bufferWaits_.load(); }
    unsigned readRetries() const noexcept { return readRetries_.load(); }
    bool audioReadFailed() const noexcept { return audioReadFailed_.load(); }
    std::uint64_t epoch() const noexcept { return epoch_.load(std::memory_order_acquire); }
private:
    enum class VoiceState { Dormant, Running, Finished };
    // source is the async cursor; at is this sample's resolved frame and next follows the last mixed one.
    // audible: mixed on the last rendered sample. Envelope keys cache each automation segment; pan gains
    // are recomputed only when pan changes.
    struct Voice {
        VoiceState state = VoiceState::Dormant; Frame source = 0, at = 0, next = 0; float left = 0, right = 0;
        bool ready = false, audible = false; int attack = 0;
        std::size_t volumeKey = 0, balanceKey = 0; double pan = 2, leftGain = 1, rightGain = 1, gain = 0;
    };
    // A cut voice keeps reading its source past the cut, fading from its last gains to silence.
    struct Release { Frame source = 0; double gain = 0, leftGain = 1, rightGain = 1; int frames = 0; };
    std::shared_ptr<const Show> show_;
    std::vector<Voice> voices_;
    std::vector<Release> releases_;
    std::vector<std::size_t> starts_, markers_, active_, releasing_;
    std::size_t nextStart_ = 0, nextMarker_ = 0;
    std::vector<std::unique_ptr<AudioBuffer>> buffers_;
    std::thread audioWorker_;
    std::atomic<bool> audioStop_{true}, audioReadFailed_{false};
    std::atomic<unsigned> bufferWaits_{0}, readRetries_{0};
    Queue<Command, 128> commands_;
    DynamicQueue<MidiEvent> midi_;
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
    void stopAudioWorker();
    void locate(Frame position) noexcept;
    Frame entry(std::size_t clip, Frame position) const noexcept;
    void prime() noexcept;
    void cut(std::size_t clip) noexcept;
};
}
