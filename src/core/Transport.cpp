#include "core/Transport.h"
#include <stdexcept>

namespace sav {
void Transport::configure(std::shared_ptr<const Show> show, Frame position) {
    if (!show || show->clips.size() > maxClips || show->markers.size() > maxMarkers)
        throw std::invalid_argument("Show exceeds transport limits");
    show_ = std::move(show);
    commands_.clear(); midi_.clear();
    voices_.fill({}); fired_.fill(false);
    position_ = std::clamp(position, Frame{0}, show_->duration);
    state_ = State::Stopped; heldMarker_ = -1;
    epoch_.fetch_add(1);
    clipped_.store(false); overflow_.store(false);
    for (std::size_t i = 0; i < show_->markers.size(); ++i) fired_[i] = show_->markers[i].time < position_;
    publish(0);
}

void Transport::dispatch(int marker) noexcept {
    if (!show_->markers[marker].message.empty() && !midi_.push({marker, position_, epoch_.load()}))
        overflow_.store(true);
}

void Transport::apply(Command command) noexcept {
    switch (command.action) {
    case Action::Play:
        if (state_ == State::Playing || (position_ >= show_->duration && state_ != State::Held)) break;
        // A hold is already consumed on arrival. Resume messages are dispatched once here.
        if (state_ == State::Held) {
            for (std::size_t i = 0; i < show_->markers.size(); ++i)
                if (fired_[i] && show_->markers[i].hold && show_->markers[i].time == position_ && show_->markers[i].timing == MidiTiming::Resume)
                    dispatch(static_cast<int>(i));
        }
        state_ = State::Playing; heldMarker_ = -1;
        break;
    case Action::Pause:
        if (state_ == State::Playing) state_ = State::Paused;
        break;
    case Action::Seek: {
        const Frame next = std::clamp(command.position, Frame{0}, show_->duration);
        for (std::size_t i = 0; i < show_->clips.size(); ++i) {
            const auto& clip = show_->clips[i];
            if (!clip.contains(next)) voices_[i] = {};
        }
        position_ = next;
        heldMarker_ = -1;
        // Seeking does not send MIDI. Landing exactly on a marker arms it for Play.
        for (std::size_t i = 0; i < show_->markers.size(); ++i) fired_[i] = show_->markers[i].time < next;
        if (state_ != State::Playing) state_ = State::Paused;
        epoch_.fetch_add(1);
        break;
    }
    case Action::StopAll:
        voices_.fill({}); state_ = State::Stopped; heldMarker_ = -1;
        epoch_.fetch_add(1); clipped_.store(false);
        break;
    }
}

void Transport::render(float* output, unsigned count, unsigned channels) noexcept {
    if (output) std::fill(output, output + static_cast<std::size_t>(count) * channels, 0.f);
    if (!show_) return;
    Command command;
    while (commands_.pop(command)) apply(command);
    float peak = 0;
    for (unsigned f = 0; f < count; ++f) {
        const bool advancingAtBoundary = state_ == State::Playing;
        if (state_ == State::Playing) {
            // The marker boundary is evaluated before rendering this sample. Never overshoot a hold.
            for (std::size_t m = 0; m < show_->markers.size(); ++m) {
                const auto& marker = show_->markers[m];
                if (!fired_[m] && marker.time == position_) {
                    fired_[m] = true;
                    if (marker.timing == MidiTiming::Arrival) dispatch(static_cast<int>(m));
                    if (marker.hold) { state_ = State::Held; heldMarker_ = static_cast<int>(m); }
                    else if (marker.timing == MidiTiming::Resume) dispatch(static_cast<int>(m));
                }
            }
            if (state_ == State::Playing && position_ >= show_->duration) {
                state_ = State::Ended; voices_.fill({});
            }
        }
        for (std::size_t i = 0; i < show_->clips.size(); ++i) {
            const auto& clip = show_->clips[i];
            if (!clip.audio || clip.track < 0 || static_cast<std::size_t>(clip.track) >= show_->tracks.size()) continue;
            const auto& track = show_->tracks[clip.track];
            if (track.visual) continue;
            auto& voice = voices_[i];
            Frame source = 0;
            if (track.async) {
                if (!clip.contains(position_) && state_ == State::Playing) voice = {};
                if (voice.state == VoiceState::Dormant && clip.contains(position_) && advancingAtBoundary && state_ != State::Ended) {
                    voice.state = VoiceState::Running;
                    voice.source = clip.offset + position_ - clip.start;
                }
                if (voice.state != VoiceState::Running) continue;
                source = voice.source++;
            } else {
                if (state_ != State::Playing || !clip.contains(position_)) continue;
                source = clip.offset + position_ - clip.start;
            }
            const Frame length = clip.audio->length();
            if (length <= 0) continue;
            if (source >= length) {
                if (clip.loop) source = clip.offset + (source - clip.offset) % std::max(Frame{1}, length - clip.offset);
                else { if (track.async) voice.state = VoiceState::Finished; continue; }
            }
            if (source < 0 || source >= length || track.muted) continue;
            const Frame local = position_ - clip.start;
            const double gain = track.gain * clip.volume.at(local, clip.gain);
            const double pan = std::clamp(track.pan + clip.balance.at(local, clip.pan), -1., 1.);
            const double leftGain = clip.audio->mono ? std::cos((pan + 1.) * 0.7853981633974483) : (pan > 0 ? 1. - pan : 1.);
            const double rightGain = clip.audio->mono ? std::sin((pan + 1.) * 0.7853981633974483) : (pan < 0 ? 1. + pan : 1.);
            if (output && track.left >= 0 && static_cast<unsigned>(track.left) < channels)
                output[static_cast<std::size_t>(f) * channels + track.left] += static_cast<float>(clip.audio->samples[source * 2] * gain * leftGain);
            if (output && track.right >= 0 && static_cast<unsigned>(track.right) < channels)
                output[static_cast<std::size_t>(f) * channels + track.right] += static_cast<float>(clip.audio->samples[source * 2 + 1] * gain * rightGain);
        }
        if (output) for (unsigned c = 0; c < channels; ++c) {
            float& sample = output[static_cast<std::size_t>(f) * channels + c];
            peak = std::max(peak, std::abs(sample));
            if (std::abs(sample) > 1.f) clipped_.store(true, std::memory_order_relaxed);
            sample = std::clamp(sample, -1.f, 1.f);
        }
        if (state_ == State::Playing) ++position_;
    }
    publish(peak);
}

void Transport::publish(float peak) noexcept {
    int active = 0;
    for (std::size_t i = 0; show_ && i < show_->clips.size(); ++i) active += voices_[i].state == VoiceState::Running;
    publishedPosition_.store(position_, std::memory_order_release);
    publishedState_.store(state_, std::memory_order_release);
    publishedMarker_.store(heldMarker_);
    publishedVoices_.store(active);
    publishedPeak_.store(peak);
}
}
