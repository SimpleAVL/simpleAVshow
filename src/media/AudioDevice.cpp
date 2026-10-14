#include "media/AudioDevice.h"

namespace sav {
std::optional<DeviceDescription> playbackDevice(const std::vector<DeviceDescription>& devices, const QString& rememberedName) {
    if (!rememberedName.isEmpty()) {
        for (const auto& device : devices) if (device.name == rememberedName) return device;
        return std::nullopt;
    }
    for (const auto& device : devices) if (device.isDefault) return device;
    return devices.empty() ? std::nullopt : std::optional<DeviceDescription>(devices.front());
}
std::vector<std::pair<RtAudio::Api, QString>> AudioDevice::apis() const {
    std::vector<RtAudio::Api> compiled; RtAudio::getCompiledApi(compiled);
    std::vector<std::pair<RtAudio::Api, QString>> result;
    for (auto api : compiled) result.emplace_back(api, QString::fromStdString(RtAudio::getApiDisplayName(api)));
    return result;
}
std::vector<DeviceDescription> AudioDevice::devices(RtAudio::Api api) {
    auto describe = [](RtAudio& audio) {
        std::vector<DeviceDescription> result;
        for (unsigned id : audio.getDeviceIds()) {
            const auto info = audio.getDeviceInfo(id);
            if (info.outputChannels) result.push_back({id, QString::fromStdString(info.name), info.outputChannels, info.isDefaultOutput});
        }
        return result;
    };
#ifdef Q_OS_WIN
    // RtAudio's ASIO backend refuses to probe a second instance while any ASIO
    // stream is open. The stream owner already has a cached device inventory,
    // including while paused; use it so Outputs does not report a missing device.
    if (audio_ && audio_->getCurrentApi() == RtAudio::WINDOWS_ASIO && audio_->isStreamOpen() &&
        (api == RtAudio::WINDOWS_ASIO || api == RtAudio::UNSPECIFIED)) return describe(*audio_);
#endif
    RtAudio probe(api); return describe(probe);
}
bool AudioDevice::open(RtAudio::Api api, unsigned device, unsigned channels, unsigned buffer, QString& error) {
    close();
    try {
        failed_.store(false); { std::lock_guard lock(errorMutex_); error_.clear(); }
        // Called from driver threads; warnings are informational, errors mean the stream is unusable.
        audio_ = std::make_unique<RtAudio>(api, [this](RtAudioErrorType type, const std::string& text) {
            if (type == RTAUDIO_NO_ERROR || type == RTAUDIO_WARNING) return;
            { std::lock_guard lock(errorMutex_); error_ = QString::fromStdString(text); }
            failed_.store(true);
        });
        RtAudio::StreamParameters output; output.deviceId = device; output.nChannels = channels; output.firstChannel = 0;
        RtAudio::StreamOptions options; options.streamName = "SimpleAVShow"; options.flags = RTAUDIO_SCHEDULE_REALTIME;
        channels_ = channels; buffer_ = buffer; underruns_.store(0);
        if (audio_->openStream(&output, nullptr, RTAUDIO_FLOAT32, sampleRate, &buffer_, callback, this, &options) != RTAUDIO_NO_ERROR)
            throw std::runtime_error(audio_->getErrorText());
        if (audio_->startStream() != RTAUDIO_NO_ERROR) throw std::runtime_error(audio_->getErrorText());
        return true;
    } catch (const std::exception& e) { error = QString::fromUtf8(e.what()); close(); return false; }
}
void AudioDevice::close() {
    if (audio_) {
        if (audio_->isStreamRunning()) audio_->stopStream();
        if (audio_->isStreamOpen()) audio_->closeStream();
        audio_.reset();
    }
    channels_ = 0;
}
int AudioDevice::callback(void* output, void*, unsigned frames, double, RtAudioStreamStatus status, void* context) {
    auto& device = *static_cast<AudioDevice*>(context);
    device.callbacks_.fetch_add(1, std::memory_order_relaxed);
    if (status) device.underruns_.fetch_add(1, std::memory_order_relaxed);
    device.transport_.render(static_cast<float*>(output), frames, device.channels_);
    return 0;
}
}
