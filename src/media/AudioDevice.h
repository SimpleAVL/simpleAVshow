#pragma once
#include "core/Transport.h"
#include <RtAudio.h>
#include <QString>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

namespace sav {
struct DeviceDescription { unsigned id; QString name; unsigned channels; bool isDefault; };
// A remembered interface must never silently fall back to another destination.
std::optional<DeviceDescription> playbackDevice(const std::vector<DeviceDescription>& devices, const QString& rememberedName);
class AudioDevice {
public:
    explicit AudioDevice(Transport& transport) : transport_(transport) {}
    ~AudioDevice() { close(); }
    std::vector<std::pair<RtAudio::Api, QString>> apis() const;
    std::vector<DeviceDescription> devices(RtAudio::Api api);
    bool open(RtAudio::Api api, unsigned device, unsigned channels, unsigned buffer, QString& error);
    void close();
    // Stops callbacks while keeping the stream open, so re-arming avoids a slow driver reopen (ASIO).
    void pause() { if (audio_ && audio_->isStreamRunning()) audio_->stopStream(); }
    bool resume() { return audio_ && !failed_.load() && audio_->isStreamOpen() && (audio_->isStreamRunning() || audio_->startStream() == RTAUDIO_NO_ERROR); }
    // A reported stream error (e.g. an unplugged interface) counts as stopped even if RtAudio still says running.
    bool running() const { return audio_ && !failed_.load() && audio_->isStreamRunning(); }
    // Increments once per device callback; a running stream whose count stalls has stopped delivering audio.
    std::uint64_t callbacks() const { return callbacks_.load(std::memory_order_relaxed); }
    QString lastError() const { std::lock_guard lock(errorMutex_); return error_; }
    unsigned channels() const { return channels_; }
    unsigned bufferFrames() const { return buffer_; }
    unsigned underruns() const { return underruns_.load(); }
private:
    Transport& transport_;
    std::unique_ptr<RtAudio> audio_;
    unsigned channels_ = 0, buffer_ = 0;
    std::atomic<unsigned> underruns_{0};
    std::atomic<std::uint64_t> callbacks_{0};
    std::atomic<bool> failed_{false};
    mutable std::mutex errorMutex_;
    QString error_;
    static int callback(void*, void*, unsigned, double, RtAudioStreamStatus, void*);
};
}
