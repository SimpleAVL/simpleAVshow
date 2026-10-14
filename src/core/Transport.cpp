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
    return segment(p[key - 1], p[key], time);
}
// An async picture clip is clocked by its video, even when the video carries sound.
Frame sourceLength(const Clip& clip) noexcept { return clip.videoLength > 0 ? clip.videoLength : clip.audio->length(); }
// The source frame to play, wrapping loops to the source-in point; -1 once a one-shot source ends.
Frame wrap(const Clip& clip, Frame source) noexcept {
    const Frame length = sourceLength(clip);
    if (source < length) return source;
    return clip.loop && length > clip.offset ? clip.offset + (source - clip.offset) % (length - clip.offset) : -1;
}
// One connected leg carries the whole source at full level; pan has no meaning there.
void route(float* frame, unsigned channels, const Track& track, bool mono, float left, float right, double gain, double leftGain, double rightGain) noexcept {
    if ((track.left >= 0) != (track.right >= 0)) {
        const int leg = std::max(track.left, track.right);
        if (static_cast<unsigned>(leg) < channels) frame[leg] += static_cast<float>((mono ? left : (left + right) * .5) * gain);
        return;
    }
    if (track.left >= 0 && static_cast<unsigned>(track.left) < channels) frame[track.left] += static_cast<float>(left * gain * leftGain);
    if (track.right >= 0 && static_cast<unsigned>(track.right) < channels) frame[track.right] += static_cast<float>(right * gain * rightGain);
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
    if (source >= sourceLength(c)) source = c.loop ? c.offset + (source - c.offset) % std::max(Frame{1}, sourceLength(c) - c.offset) : c.offset;
    return source;
}
// Point every disk source at the frame it will play next, so clips that already
// played read ahead again after a seek or Stop All instead of missing on entry.
void Transport::prime() noexcept {
    for (const auto i : starts_) if (buffers_[i])
        buffers_[i]->request(voices_[i].state == VoiceState::Running ? voices_[i].source : entry(i, position_));
}
// An audible voice stopping mid-waveform fades out from the frame it would have played next.
void Transport::cut(std::size_t i) noexcept {
    auto& voice = voices_[i];
    if (!voice.audible) return;
    voice.audible = false; voice.attack = 0;
    const Frame source = wrap(show_->clips[i], voice.next);
    if (source < 0) return; // The source itself ended.
    auto& release = releases_[i];
    if (release.frames == 0) releasing_.push_back(i);
    release = {source, voice.gain, voice.leftGain, voice.rightGain, fadeFrames};
    float left, right;
    if (buffers_[i]) buffers_[i]->peek(source, left, right); // Hold the tail's block before the voice retargets.
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
    voices_.assign(show_->clips.size(), {}); releases_.assign(show_->clips.size(), {});
    releasing_.clear(); releasing_.reserve(show_->clips.size());
    starts_.clear(); pictures_.clear(); active_.clear(); active_.reserve(show_->clips.size());
    publishedSources_ = std::make_unique<std::atomic<Frame>[]>(show_->clips.size()); publishedSourceCount_ = show_->clips.size();
    for (std::size_t i = 0; i < publishedSourceCount_; ++i) publishedSources_[i].store(dormantSource);
    buffers_.clear(); buffers_.resize(show_->clips.size());
    position_ = std::clamp(position, Frame{0}, show_->duration);
    for (std::size_t i = 0; i < show_->clips.size(); ++i) {
        const auto& c = show_->clips[i];
        if (c.track < 0 || static_cast<std::size_t>(c.track) >= show_->tracks.size()) continue;
        if (show_->tracks[c.track].visual) {
            // An async video gets a silent voice: the same clock as async audio, without mixing.
            if (show_->tracks[c.track].async && c.videoLength > c.offset) { starts_.push_back(i); pictures_.push_back(i); }
            continue;
        }
        if (!c.audio) continue;
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
        for (std::size_t i = 0; i < show_->clips.size(); ++i) {
            if (!show_->clips[i].contains(next)) { cut(i); voices_[i] = {}; }
            else if (voices_[i].audible && next != position_ && !show_->tracks[show_->clips[i].track].async) cut(i); // A normal source jumps.
        }
        position_ = next; heldMarker_ = -1; locate(next);
        if (state_ != State::Playing) state_ = State::Paused;
        epoch_.fetch_add(1);
        break;
    }
    case Action::StopAll:
        for (std::size_t i = 0; i < voices_.size(); ++i) cut(i);
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
    float peak = 0; bool waiting = false;
    for (unsigned f = 0; f < count; ++f) {
        if (waiting && releasing_.empty()) break; // Remaining device samples stay silent; retry at the same show/source time.
        float* const frame = output ? output + static_cast<std::size_t>(f) * channels : nullptr;
        if (!waiting) {
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
                    state_ = State::Ended; for (std::size_t i = 0; i < voices_.size(); ++i) cut(i);
                    std::fill(voices_.begin(), voices_.end(), Voice{}); active_.clear();
                }
            }
            bool ready = true;
            // Resolve all sources before mixing: a cache miss freezes the whole
            // sample clock, so tracks remain aligned and no source frames are lost.
            for (std::size_t cursor = 0; cursor < active_.size();) {
                const auto i = active_[cursor]; const auto& clip = show_->clips[i]; const auto& track = show_->tracks[clip.track]; auto& voice = voices_[i];
                voice.ready = false;
                if (!clip.contains(position_) && state_ == State::Playing) {
                    cut(i); voice = {}; active_[cursor] = active_.back(); active_.pop_back(); continue;
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
                const Frame length = sourceLength(clip);
                if (length <= 0) continue;
                if (source >= length) {
                    if (clip.loop) source = clip.offset + (source - clip.offset) % std::max(Frame{1}, length - clip.offset);
                    else { if (track.async) voice.state = VoiceState::Finished; continue; }
                }
                if (source < 0 || source >= length) continue;
                if (track.async) voice.source = source; // Keep loops' cursors bounded, including long holds.
                if (track.muted || track.visual) continue;
                if (buffers_[i]) {
                    if (!buffers_[i]->sample(source, voice.left, voice.right)) { ready = false; continue; }
                } else {
                    voice.left = clip.audio->samples[source * 2]; voice.right = clip.audio->samples[source * 2 + 1];
                }
                if (!voice.audible && !track.async && source != clip.offset) voice.attack = fadeFrames; // Entering mid-source fades in.
                voice.at = source; voice.ready = true;
            }
            if (!ready) { bufferWaits_.fetch_add(1, std::memory_order_relaxed); waiting = true; }
            // Voices that stop sounding fade out. A miss fades every voice; normal voices fade back in once ready.
            for (const auto i : active_) if (waiting || !voices_[i].ready) cut(i);
            if (!waiting) for (const auto i : active_) {
                const auto& clip = show_->clips[i]; const auto& track = show_->tracks[clip.track]; auto& voice = voices_[i];
                if (track.async && voice.state == VoiceState::Running) ++voice.source;
                if (!voice.ready) continue;
                voice.audible = true; voice.next = voice.at + 1;
                const Frame local = position_ - clip.start;
                voice.gain = track.gain * level(clip.volume, local, clip.gain, voice.volumeKey);
                if (voice.attack > 0) voice.gain *= 1. - static_cast<double>(--voice.attack) / fadeFrames;
                if ((track.left >= 0) == (track.right >= 0)) {
                    const double pan = std::clamp(track.pan + level(clip.balance, local, clip.pan, voice.balanceKey), -1., 1.);
                    if (pan != voice.pan) {
                        voice.pan = pan;
                        voice.leftGain = clip.audio->mono ? std::cos((pan + 1.) * 0.7853981633974483) : (pan > 0 ? 1. - pan : 1.);
                        voice.rightGain = clip.audio->mono ? std::sin((pan + 1.) * 0.7853981633974483) : (pan < 0 ? 1. + pan : 1.);
                    }
                }
                if (frame) route(frame, channels, track, clip.audio->mono, voice.left, voice.right, voice.gain, voice.leftGain, voice.rightGain);
            }
        }
        // Release tails never wait: a disk miss ends the fade. They leave show time and async cursors alone.
        for (std::size_t r = 0; r < releasing_.size();) {
            const auto i = releasing_[r]; const auto& clip = show_->clips[i]; auto& release = releases_[i];
            const Frame source = wrap(clip, release.source);
            float left = 0, right = 0; bool read = source >= 0;
            if (read && buffers_[i]) read = buffers_[i]->peek(source, left, right);
            else if (read) { left = clip.audio->samples[source * 2]; right = clip.audio->samples[source * 2 + 1]; }
            if (read) {
                release.source = source + 1; --release.frames;
                if (frame) route(frame, channels, show_->tracks[clip.track], clip.audio->mono, left, right,
                    release.gain * release.frames / fadeFrames, release.leftGain, release.rightGain);
                if (release.frames > 0) { ++r; continue; }
            }
            release.frames = 0; if (buffers_[i]) buffers_[i]->settle();
            releasing_[r] = releasing_.back(); releasing_.pop_back();
        }
        if (frame) for (unsigned c = 0; c < channels; ++c) {
            float& sample = frame[c];
            if (!std::isfinite(sample)) { sample = 0; clipped_.store(true, std::memory_order_relaxed); } // Drivers may turn NaN into full-scale noise.
            peak = std::max(peak, std::abs(sample));
            if (std::abs(sample) > 1.f) clipped_.store(true, std::memory_order_relaxed);
            sample = std::clamp(sample, -1.f, 1.f);
        }
        if (!waiting && state_ == State::Playing) ++position_;
    }
    publish(peak);
}
void Transport::publish(float peak) noexcept {
    int active = 0;
    for (const auto i : active_) active += show_->tracks[show_->clips[i].track].async && voices_[i].state == VoiceState::Running;
    for (const auto i : pictures_) {
        const auto& voice = voices_[i];
        publishedSources_[i].store(voice.state == VoiceState::Running ? voice.source : voice.state == VoiceState::Finished ? finishedSource : dormantSource,
            std::memory_order_release);
    }
    publishedPosition_.store(position_, std::memory_order_release);
    publishedState_.store(state_, std::memory_order_release);
    publishedMarker_.store(heldMarker_); publishedVoices_.store(active); publishedPeak_.store(peak);
}
}
