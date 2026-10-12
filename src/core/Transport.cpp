#include "core/Transport.h"
#include <chrono>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace sav {
namespace {
// Envelope::at with a cached segment: key is the first keyframe after time. Playback moves
// forward, so the cached or following segment usually matches without a binary search.
double level(const Envelope& envelope, Frame time, double fallback, std::size_t& key) noexcept {
    const auto& p = envelope.points;
    if (p.empty()) return fallback;
    if (time <= p.front().time) return p.front().value;
    const auto holds = [&](std::size_t k) { return k > 0 && k <= p.size() && p[k - 1].time <= time && (k == p.size() || time < p[k].time); };
    if (!holds(key) && !holds(++key)) key = static_cast<std::size_t>(std::upper_bound(p.begin(), p.end(), time,
        [](Frame t, const Keyframe& k) { return t < k.time; }) - p.begin());
    if (key == p.size()) return p.back().value;
    const auto& a = p[key - 1]; const auto& b = p[key];
    const double t = static_cast<double>(time - a.time) / (b.time - a.time);
    return a.value + t * (b.value - a.value);
}
}
Transport::~Transport() { stopAudioWorker(); }
void Transport::stopAudioWorker() {
    audioStop_.store(true);
    if (audioWorker_.joinable()) audioWorker_.join();
}
void Transport::locate(Frame position) noexcept {
    active_.clear();
    for (const auto i : starts_) if (show_->clips[i].contains(position)) active_.push_back(i);
    nextStart_ = static_cast<std::size_t>(std::upper_bound(starts_.begin(), starts_.end(), position,
        [this](Frame t, std::size_t i) { return t < show_->clips[i].start; }) - starts_.begin());
    nextMarker_ = static_cast<std::size_t>(std::lower_bound(markers_.begin(), markers_.end(), position,
        [this](std::size_t i, Frame t) { return show_->markers[i].time < t; }) - markers_.begin());
    prime();
}
Frame Transport::entry(std::size_t i, Frame position) const noexcept {
    const auto& c = show_->clips[i];
    Frame source = c.offset + (c.contains(position) ? position - c.start : 0);
    if (source >= c.audio->length()) source = c.loop ? c.offset + (source - c.offset) % std::max(Frame{1}, c.audio->length() - c.offset) : c.offset;
    return source;
}
// Point every disk source at the frame it will play next, so clips that already
// played read ahead again after a seek or Stop All instead of missing on entry.
void Transport::prime() noexcept {
    for (const auto i : starts_) if (buffers_[i])
        buffers_[i]->request(voices_[i].state == VoiceState::Running ? voices_[i].source : entry(i, position_));
}
void Transport::configure(std::shared_ptr<const Show> show, Frame position) {
    if (!show || show->duration <= 0 || show->duration > maxFrame) throw std::invalid_argument("Invalid show duration");
    stopAudioWorker();
    audioReadFailed_.store(false);
    // Edits re-arm often: unchanged disk clips keep their read-ahead and only retarget their entry.
    std::unordered_map<std::string_view, std::size_t> previous;
    if (show_) for (std::size_t i = 0; i < buffers_.size(); ++i) if (buffers_[i] && !buffers_[i]->failed()) previous.emplace(show_->clips[i].id, i);
    const auto retired = std::move(show_); auto reusable = std::move(buffers_);
    show_ = std::move(show);
    commands_.clear(); midi_.reset(std::max(std::size_t{1024}, show_->markers.size()));
    voices_.assign(show_->clips.size(), {});
    starts_.clear(); active_.clear(); active_.reserve(show_->clips.size());
    buffers_.clear(); buffers_.resize(show_->clips.size());
    position_ = std::clamp(position, Frame{0}, show_->duration);
    for (std::size_t i = 0; i < show_->clips.size(); ++i) {
        const auto& c = show_->clips[i];
        if (!c.audio || c.track < 0 || static_cast<std::size_t>(c.track) >= show_->tracks.size() || show_->tracks[c.track].visual) continue;
        starts_.push_back(i);
        if (c.audio->storage) {
            const Frame from = entry(i, position_); bool resident = false;
            if (const auto found = previous.find(c.id); found != previous.end() && reusable[found->second] && reusable[found->second]->matches(c.audio.get(), c.offset, c.loop)) {
                buffers_[i] = std::move(reusable[found->second]); resident = buffers_[i]->retarget(from);
            } else buffers_[i] = std::make_unique<AudioBuffer>(c.audio, c.offset, c.loop, from);
            if (!resident) buffers_[i]->prebuffer(); // Before opening the device.
            if (buffers_[i]->failed()) audioReadFailed_.store(true);
        }
    }
    std::stable_sort(starts_.begin(), starts_.end(), [this](auto a, auto b) { return show_->clips[a].start < show_->clips[b].start; });
    markers_.resize(show_->markers.size()); std::iota(markers_.begin(), markers_.end(), 0);
    std::stable_sort(markers_.begin(), markers_.end(), [this](auto a, auto b) { return show_->markers[a].time < show_->markers[b].time; });
    locate(position_);
    state_ = State::Stopped; heldMarker_ = -1;
    epoch_.fetch_add(1);
    clipped_.store(false); overflow_.store(false); bufferWaits_.store(0); readRetries_.store(0);
    if (std::any_of(buffers_.begin(), buffers_.end(), [](const auto& buffer) { return bool(buffer); })) {
        audioStop_.store(false);
        audioWorker_ = std::thread([this] {
            while (!audioStop_.load()) {
                for (auto& buffer : buffers_) if (buffer) {
                    if (!buffer->service()) readRetries_.fetch_add(1, std::memory_order_relaxed);
                    if (buffer->failed()) audioReadFailed_.store(true);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }
    publish(0);
}

void Transport::dispatch(int marker) noexcept {
    if (!show_->markers[marker].message.empty() && !midi_.push({marker, position_, epoch_.load()})) overflow_.store(true);
}
void Transport::apply(Command command) noexcept {
    switch (command.action) {
    case Action::Play:
        if (state_ == State::Playing || (position_ >= show_->duration && state_ != State::Held)) break;
        if (state_ == State::Held) {
            // Only the consumed group at this hold can send resume messages.
            auto begin = nextMarker_;
            while (begin > 0 && show_->markers[markers_[begin - 1]].time == position_) --begin;
            for (auto m = begin; m < nextMarker_; ++m) {
                const auto i = markers_[m]; const auto& marker = show_->markers[i];
                if (marker.hold && marker.timing == MidiTiming::Resume) dispatch(static_cast<int>(i));
            }
        }
        state_ = State::Playing; heldMarker_ = -1;
        break;
    case Action::Pause:
        if (state_ == State::Playing) state_ = State::Paused;
        break;
    case Action::Seek: {
        const Frame next = std::clamp(command.position, Frame{0}, show_->duration);
        for (std::size_t i = 0; i < show_->clips.size(); ++i) if (!show_->clips[i].contains(next)) voices_[i] = {};
        position_ = next; heldMarker_ = -1; locate(next);
        if (state_ != State::Playing) state_ = State::Paused;
        epoch_.fetch_add(1);
        break;
    }
    case Action::StopAll:
        std::fill(voices_.begin(), voices_.end(), Voice{}); state_ = State::Stopped; heldMarker_ = -1;
        epoch_.fetch_add(1); clipped_.store(false); prime();
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
            while (nextStart_ < starts_.size() && show_->clips[starts_[nextStart_]].start <= position_) {
                const auto i = starts_[nextStart_++];
                if (show_->clips[i].contains(position_)) active_.push_back(i);
            }
            while (nextMarker_ < markers_.size() && show_->markers[markers_[nextMarker_]].time == position_) {
                const auto i = markers_[nextMarker_++]; const auto& marker = show_->markers[i];
                if (marker.timing == MidiTiming::Arrival) dispatch(static_cast<int>(i));
                if (marker.hold) { state_ = State::Held; heldMarker_ = static_cast<int>(i); }
                else if (marker.timing == MidiTiming::Resume) dispatch(static_cast<int>(i));
            }
            if (state_ == State::Playing && position_ >= show_->duration) {
                state_ = State::Ended; std::fill(voices_.begin(), voices_.end(), Voice{}); active_.clear();
            }
        }
        bool ready = true;
        // Resolve all sources before mixing: a cache miss freezes the whole
        // sample clock, so tracks remain aligned and no source frames are lost.
        for (std::size_t cursor = 0; cursor < active_.size();) {
            const auto i = active_[cursor]; const auto& clip = show_->clips[i]; const auto& track = show_->tracks[clip.track]; auto& voice = voices_[i];
            voice.audible = false;
            if (!clip.contains(position_) && state_ == State::Playing) {
                voice = {}; active_[cursor] = active_.back(); active_.pop_back(); continue;
            }
            ++cursor;
            Frame source;
            if (track.async) {
                if (voice.state == VoiceState::Dormant && clip.contains(position_) && advancingAtBoundary && state_ != State::Ended) {
                    voice.state = VoiceState::Running; voice.source = clip.offset + position_ - clip.start;
                }
                if (voice.state != VoiceState::Running) continue;
                source = voice.source;
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
            if (source < 0 || source >= length) continue;
            if (track.async) voice.source = source; // Keep loops' cursors bounded, including long holds.
            if (track.muted) continue;
            if (buffers_[i]) {
                if (!buffers_[i]->sample(source, voice.left, voice.right)) { ready = false; continue; }
            } else {
                voice.left = clip.audio->samples[source * 2]; voice.right = clip.audio->samples[source * 2 + 1];
            }
            voice.audible = true;
        }
        if (!ready) {
            bufferWaits_.fetch_add(1, std::memory_order_relaxed);
            break; // Remaining device samples stay silent; retry at the same show/source time.
        }
        for (const auto i : active_) {
            const auto& clip = show_->clips[i]; const auto& track = show_->tracks[clip.track]; auto& voice = voices_[i];
            if (track.async && voice.state == VoiceState::Running) ++voice.source;
            if (!voice.audible) continue;
            const Frame local = position_ - clip.start;
            const double gain = track.gain * level(clip.volume, local, clip.gain, voice.volumeKey);
            if (!output) continue;
            if ((track.left >= 0) != (track.right >= 0)) {
                // One connected leg carries the whole source at full level; pan has no meaning there.
                const int leg = std::max(track.left, track.right);
                const double mono = clip.audio->mono ? voice.left : (voice.left + voice.right) * .5;
                if (static_cast<unsigned>(leg) < channels) output[static_cast<std::size_t>(f) * channels + leg] += static_cast<float>(mono * gain);
                continue;
            }
            const double pan = std::clamp(track.pan + level(clip.balance, local, clip.pan, voice.balanceKey), -1., 1.);
            if (pan != voice.pan) {
                voice.pan = pan;
                voice.leftGain = clip.audio->mono ? std::cos((pan + 1.) * 0.7853981633974483) : (pan > 0 ? 1. - pan : 1.);
                voice.rightGain = clip.audio->mono ? std::sin((pan + 1.) * 0.7853981633974483) : (pan < 0 ? 1. + pan : 1.);
            }
            if (track.left >= 0 && static_cast<unsigned>(track.left) < channels)
                output[static_cast<std::size_t>(f) * channels + track.left] += static_cast<float>(voice.left * gain * voice.leftGain);
            if (track.right >= 0 && static_cast<unsigned>(track.right) < channels)
                output[static_cast<std::size_t>(f) * channels + track.right] += static_cast<float>(voice.right * gain * voice.rightGain);
        }
        if (output) for (unsigned c = 0; c < channels; ++c) {
            float& sample = output[static_cast<std::size_t>(f) * channels + c];
            if (!std::isfinite(sample)) { sample = 0; clipped_.store(true, std::memory_order_relaxed); } // Drivers may turn NaN into full-scale noise.
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
    for (const auto i : active_) active += show_->tracks[show_->clips[i].track].async && voices_[i].state == VoiceState::Running;
    publishedPosition_.store(position_, std::memory_order_release);
    publishedState_.store(state_, std::memory_order_release);
    publishedMarker_.store(heldMarker_); publishedVoices_.store(active); publishedPeak_.store(peak);
}
}
