#pragma once
#include "core/Transport.h"
#include <RtAudio.h>
#include <QString>
#include <atomic>
#include <memory>
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
    bool running() const { return audio_ && audio_->isStreamRunning(); }
    unsigned channels() const { return channels_; }
    unsigned bufferFrames() const { return buffer_; }
    unsigned underruns() const { return underruns_.load(); }
private:
    Transport& transport_;
    std::unique_ptr<RtAudio> audio_;
    unsigned channels_ = 0, buffer_ = 0;
    std::atomic<unsigned> underruns_{0};
    static int callback(void*, void*, unsigned, double, RtAudioStreamStatus, void*);
};
}
