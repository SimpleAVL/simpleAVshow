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
    urgentStop_.store(false, std::memory_order_release);
    pendingStart_ = {};
    commands_.clear(); applied_ = 0; commandsApplied_.store(0, std::memory_order_release); midi_.reset(std::max(std::size_t{1024}, show_->markers.size()));
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
            if (const auto found = previous.find(c.id); found != previous.end() && reusable[found->second] &&
                reusable[found->second]->matchesContent(c.audio.get(), c.offset, c.loop)) {
                buffers_[i] = std::move(reusable[found->second]);
                buffers_[i]->rebindData(c.audio);
                resident = buffers_[i]->retarget(from);
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

void Transport::dispatch(int marker, CommandSource source, std::uint64_t sourceEpoch,
                         CommandSource secondarySource, std::uint64_t secondaryEpoch) noexcept {
    if (!show_->markers[marker].message.empty() &&
        !midi_.push({marker, position_, epoch_.load(), source, sourceEpoch, secondarySource, secondaryEpoch}))
        overflow_.store(true);
}
void Transport::apply(Command command, PendingPlayStart& pendingStart) noexcept {
    if (!isSourceValid(command.source, command.sourceEpoch)) {
        ++applied_;
        return;
    }
    auto higherPrivilege = [](CommandSource a, CommandSource b) noexcept {
        return (a == CommandSource::Operator && b != CommandSource::Operator) ||
               (a == CommandSource::ExternalAgent && b == CommandSource::ChatAgent);
    };
    switch (command.action) {
    case Action::Play: {
        if (state_ == State::Playing) {
            // If an operator (or higher-privilege domain) also pressed Play in the same batch after
            // an agent started playback, promote the pending start and hold-resume authorization so
            // late agent revocation cannot roll back the higher-privilege Play.
            if (pendingStart.active && higherPrivilege(command.source, pendingStart.source)) {
                pendingStart.source = command.source;
                pendingStart.sourceEpoch = command.sourceEpoch;
            }
            if (pendingStart.wasHeld && higherPrivilege(command.source, pendingStart.resumeSource)) {
                pendingStart.resumeSource = command.source;
                pendingStart.resumeEpoch = command.sourceEpoch;
            }
            break;
        }
        if (position_ >= show_->duration && state_ != State::Held) break;
        const State prevState = state_;
        const int prevHeldMarker = heldMarker_;
        const bool wasHeld = state_ == State::Held;
        state_ = State::Playing; heldMarker_ = -1;
        // Re-verify agent authorization after state transition so a Show-mode entry or Disconnect
        // cannot be bypassed by callback preemption. Carry initiating source/epoch to the first
        // rendered sample and defer hold-resume MIDI until the command batch settles.
        if (command.source != CommandSource::Operator && !isSourceValid(command.source, command.sourceEpoch)) {
            state_ = prevState; heldMarker_ = prevHeldMarker;
            break;
        }
        pendingStart.active = true;
        pendingStart.source = command.source;
        pendingStart.sourceEpoch = command.sourceEpoch;
        pendingStart.prevState = prevState;
        pendingStart.prevHeldMarker = prevHeldMarker;
        if (wasHeld) {
            std::size_t begin = nextMarker_;
            while (begin > 0 && show_->markers[markers_[begin - 1]].time == position_) --begin;
            pendingStart.wasHeld = true;
            pendingStart.begin = begin;
            pendingStart.end = nextMarker_;
            pendingStart.resumeSource = command.source;
            pendingStart.resumeEpoch = command.sourceEpoch;
            pendingStart.resumePrevState = prevState;
            pendingStart.resumePrevHeldMarker = prevHeldMarker;
        } else if (pendingStart.wasHeld && higherPrivilege(command.source, pendingStart.resumeSource)) {
            pendingStart.resumeSource = command.source;
            pendingStart.resumeEpoch = command.sourceEpoch;
        }
        break;
    }
    case Action::Pause:
        if (state_ == State::Playing) state_ = State::Paused;
        break;
    case Action::Seek: {
        // Seeking cancels any pending hold-resume MIDI, but MUST preserve pendingStart.active
        // and its initiating source/epoch when Play preceded Seek in the same batch so a queued
        // Seek cannot bypass the first-sample Play authorization check.
        pendingStart.wasHeld = false;
        if (pendingStart.active) {
            pendingStart.prevState = State::Paused;
            pendingStart.prevHeldMarker = -1;
        }
        // Only tag seekCue when seeking while already Playing: a Seek while Stopped/Paused never
        // fires MIDI on its own, and any subsequent Play establishes its own authorization epoch.
        if (state_ == State::Playing && command.source != CommandSource::Operator) {
            pendingStart.seekCueActive = true;
            pendingStart.seekCueSource = command.source;
            pendingStart.seekCueEpoch = command.sourceEpoch;
        } else {
            pendingStart.seekCueActive = false;
        }
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
        pendingStart = {};
        applyStopAll();
        break;
    }
    ++applied_;
}

void Transport::applyStopAll() noexcept {
    pendingStart_ = {};
    for (std::size_t i = 0; i < voices_.size(); ++i) cut(i);
    std::fill(voices_.begin(), voices_.end(), Voice{}); state_ = State::Stopped; heldMarker_ = -1;
    epoch_.fetch_add(1); clipped_.store(false); prime();
}

void Transport::forceStopAll() noexcept {
    if (!show_) return;
    urgentStop_.store(false, std::memory_order_release);
    commands_.clear();
    applyStopAll();
    // Stream is closed: discard release tails created by applyStopAll() so reopening audio while Stopped is silent.
    releasing_.clear();
    std::fill(releases_.begin(), releases_.end(), Release{});
    applied_ = 0;
    publish(0);
}

void Transport::render(float* output, unsigned count, unsigned channels) noexcept {
    if (output) std::fill(output, output + static_cast<std::size_t>(count) * channels, 0.f);
    if (!show_) return;
    // Dequeue pending commands first, THEN sample urgentStop_ and authorization epochs so any
    // Disconnect, New, Show-mode entry, or emergency Stop All issued before dequeue takes effect
    // on this batch rather than executing a revoked command first.
    Command batch[128];
    std::size_t countQueued = 0;
    while (countQueued < 128 && commands_.pop(batch[countQueued])) ++countQueued;

    const bool urgent = urgentStop_.load(std::memory_order_acquire);
    const std::uint64_t validExt = externalAgentEpoch_.load(std::memory_order_acquire);
    const std::uint64_t validChat = chatAgentEpoch_.load(std::memory_order_acquire);

    auto applyUrgentStop = [&](std::size_t fromIndex) noexcept {
        // Coalesce any backlogged valid Seeks to the final seek target so emergency Stop All
        // runs in O(1) audio work rather than cutting/repriming every intermediate seek.
        bool hasFinalSeek = false;
        Frame finalSeekPos = 0;
        for (std::size_t i = fromIndex; i < countQueued; ++i) {
            const auto& c = batch[i];
            if (c.action == Action::Seek && isSourceValid(c.source, c.sourceEpoch)) {
                hasFinalSeek = true;
                finalSeekPos = c.position;
            }
            ++applied_;
        }
        Command residual;
        while (commands_.pop(residual)) {
            if (residual.action == Action::Seek && isSourceValid(residual.source, residual.sourceEpoch)) {
                hasFinalSeek = true;
                finalSeekPos = residual.position;
            }
            ++applied_;
        }
        if (hasFinalSeek) {
            position_ = std::clamp(finalSeekPos, Frame{0}, show_->duration);
            locate(position_);
        }
        applyStopAll();
        urgentStop_.store(false, std::memory_order_release);
    };

    // Keep pendingStart_ across callbacks if the previous callback stalled on a disk read before
    // mixing its first sample so revoking an agent during disk wait still blocks playback start.
    PendingPlayStart& pendingStart = pendingStart_;
    if (urgent) {
        applyUrgentStop(0);
    } else {
        int lastOpStop = -1;
        int lastExtStop = -1;
        int lastChatStop = -1;
        for (std::size_t i = 0; i < countQueued; ++i) {
            const auto& c = batch[i];
            if (c.action == Action::StopAll) {
                if (c.source == CommandSource::Operator) lastOpStop = static_cast<int>(i);
                else if (c.source == CommandSource::ExternalAgent && isCommandValid(c, validExt, validChat)) lastExtStop = static_cast<int>(i);
                else if (c.source == CommandSource::ChatAgent && isCommandValid(c, validExt, validChat)) lastChatStop = static_cast<int>(i);
            }
        }
        std::size_t startIndex = 0;
        if (lastOpStop >= 0) {
            // Give queued Operator StopAll O(1) priority even when the queue is not full:
            // coalesce any preceding valid seeks to the final seek position and stop once.
            bool hasFinalSeek = false;
            Frame finalSeekPos = 0;
            for (std::size_t i = 0; i <= static_cast<std::size_t>(lastOpStop); ++i) {
                const auto& c = batch[i];
                if (c.action == Action::Seek && isSourceValid(c.source, c.sourceEpoch)) {
                    hasFinalSeek = true;
                    finalSeekPos = c.position;
                }
                ++applied_;
            }
            if (hasFinalSeek) {
                position_ = std::clamp(finalSeekPos, Frame{0}, show_->duration);
                locate(position_);
            }
            applyStopAll();
            startIndex = static_cast<std::size_t>(lastOpStop + 1);
        }
        auto cutoffValid = [&](int idx) noexcept {
            return idx >= static_cast<int>(startIndex) && isSourceValid(batch[idx].source, batch[idx].sourceEpoch);
        };
        for (std::size_t i = startIndex; i < countQueued; ++i) {
            if (urgentStop_.load(std::memory_order_acquire)) {
                pendingStart = {};
                applyUrgentStop(i);
                break;
            }
            const auto& c = batch[i];
            if (lastOpStop >= 0 && c.source != CommandSource::Operator && c.action == Action::Play) {
                ++applied_;
                continue;
            }
            // Never allow a lower-privilege or revocable StopAll domain to suppress a command from
            // a higher-privilege or independent domain, and recheck live epochs so a StopAll
            // revoked mid-batch never suppresses an earlier valid command. Seeks are always preserved.
            const int validExtStop = cutoffValid(lastExtStop) ? lastExtStop : -1;
            const int validChatStop = cutoffValid(lastChatStop) ? lastChatStop : -1;
            const int stopCutoff = c.source == CommandSource::Operator
                ? -1
                : c.source == CommandSource::ExternalAgent
                    ? validExtStop
                    : std::max(validExtStop, validChatStop);
            if (static_cast<int>(i) < stopCutoff && (c.action == Action::Play || c.action == Action::Pause)) {
                ++applied_;
                continue;
            }
            apply(c, pendingStart);
        }
    }
    // Verify the initiating Play/resume source right at the first-sample rendering boundary (and on
    // zero-frame renders) so callback preemption before sample 0 cannot start audio or fire
    // arrival/resume cues after Disconnect, New, or Show-mode entry.
    if (pendingStart.active || pendingStart.wasHeld) {
        if (urgentStop_.load(std::memory_order_acquire)) {
            applyUrgentStop(countQueued);
            pendingStart = {};
        } else {
            // Do not bump the global MIDI epoch_ on revoked agent rollback: any MIDI emitted by the
            // revoked agent already carries its own source/sourceEpoch and is rejected by the MIDI worker,
            // while unrelated earlier Operator cues in midi_ must remain valid.
            if (pendingStart.active && pendingStart.source != CommandSource::Operator &&
                !isSourceValid(pendingStart.source, pendingStart.sourceEpoch)) {
                state_ = pendingStart.prevState;
                heldMarker_ = pendingStart.prevHeldMarker;
                pendingStart.active = false;
            }
            if (pendingStart.wasHeld && (state_ == State::Playing || state_ == State::Paused)) {
                if (pendingStart.resumeSource != CommandSource::Operator &&
                    !isSourceValid(pendingStart.resumeSource, pendingStart.resumeEpoch)) {
                    state_ = pendingStart.resumePrevState;
                    heldMarker_ = pendingStart.resumePrevHeldMarker;
                    pendingStart.wasHeld = false;
                    pendingStart.active = false;
                } else if (count == 0 || state_ == State::Paused) {
                    // When no audio frame will be rendered (zero-frame render or same-batch Play+Pause),
                    // dispatch hold-resume MIDI here. Otherwise defer until sample 0 passes disk readiness
                    // so a disconnect during a disk stall cannot emit resume MIDI twice.
                    for (auto m = pendingStart.begin; m < pendingStart.end; ++m) {
                        const auto i = markers_[m]; const auto& marker = show_->markers[i];
                        if (marker.hold && marker.timing == MidiTiming::Resume)
                            dispatch(static_cast<int>(i), pendingStart.resumeSource, pendingStart.resumeEpoch);
                    }
                    if (pendingStart.resumeSource != CommandSource::Operator &&
                        !isSourceValid(pendingStart.resumeSource, pendingStart.resumeEpoch)) {
                        state_ = pendingStart.resumePrevState;
                        heldMarker_ = pendingStart.resumePrevHeldMarker;
                        pendingStart.active = false;
                    }
                    pendingStart.wasHeld = false;
                }
            } else if (pendingStart.wasHeld) {
                pendingStart.wasHeld = false;
            }
        }
    }
    if (state_ != State::Playing) {
        pendingStart.active = false;
        pendingStart.wasHeld = false;
        pendingStart.seekCueActive = false;
    }
    float peak = 0; bool waiting = false;
    for (unsigned f = 0; f < count; ++f) {
        if (waiting && releasing_.empty()) break; // Remaining device samples stay silent; retry at the same show/source time.
        float* const frame = output ? output + static_cast<std::size_t>(f) * channels : nullptr;
        if (!waiting) {
            if (f == 0 && state_ == State::Playing) {
                if (pendingStart.active && pendingStart.source != CommandSource::Operator &&
                    !isSourceValid(pendingStart.source, pendingStart.sourceEpoch)) {
                    state_ = pendingStart.prevState;
                    heldMarker_ = pendingStart.prevHeldMarker;
                    pendingStart.active = false;
                    pendingStart.wasHeld = false;
                    pendingStart.seekCueActive = false;
                } else if (pendingStart.wasHeld && pendingStart.resumeSource != CommandSource::Operator &&
                           !isSourceValid(pendingStart.resumeSource, pendingStart.resumeEpoch)) {
                    state_ = pendingStart.resumePrevState;
                    heldMarker_ = pendingStart.resumePrevHeldMarker;
                    pendingStart.active = false;
                    pendingStart.wasHeld = false;
                    pendingStart.seekCueActive = false;
                }
            }
            bool advancingAtBoundary = state_ == State::Playing;
            // On sample 0 of a newly initiated Play, defer consuming markers/MIDI and starting dormant
            // async voices until disk readiness and live authorization both succeed so a disk stall
            // followed by Disconnect leaves markers unconsumed and async voices Dormant.
            const bool deferStartCommit = f == 0 && pendingStart.active && state_ == State::Playing;
            const std::size_t savedNextStart = nextStart_;
            const std::size_t savedNextMarker = nextMarker_;
            const std::size_t savedActiveSize = active_.size();
            const State savedBoundaryState = state_;
            const int savedBoundaryHeld = heldMarker_;
            CommandSource cueSource = CommandSource::Operator;
            std::uint64_t cueEpoch = 0;
            CommandSource cueSource2 = CommandSource::Operator;
            std::uint64_t cueEpoch2 = 0;
            bool suppressSeekCue = false;
            if (state_ == State::Playing) {
                if (f == 0) {
                    if (pendingStart.active && pendingStart.source != CommandSource::Operator) {
                        cueSource = pendingStart.source;
                        cueEpoch = pendingStart.sourceEpoch;
                    }
                    if (pendingStart.seekCueActive) {
                        if (cueSource == CommandSource::Operator) {
                            cueSource = pendingStart.seekCueSource;
                            cueEpoch = pendingStart.seekCueEpoch;
                        } else if (pendingStart.seekCueSource != cueSource || pendingStart.seekCueEpoch != cueEpoch) {
                            cueSource2 = pendingStart.seekCueSource;
                            cueEpoch2 = pendingStart.seekCueEpoch;
                        }
                        if (!isSourceValid(pendingStart.seekCueSource, pendingStart.seekCueEpoch))
                            suppressSeekCue = true;
                    }
                }
                while (nextStart_ < starts_.size() && show_->clips[starts_[nextStart_]].start <= position_) {
                    const auto i = starts_[nextStart_++];
                    if (show_->clips[i].contains(position_)) active_.push_back(i);
                }
                if (deferStartCommit) {
                    // Only preview whether a hold marker exists at position_ so normal clips respect the hold
                    // during readiness checking without consuming nextMarker_ or emitting MIDI yet.
                    for (std::size_t m = nextMarker_; m < markers_.size() && show_->markers[markers_[m]].time == position_; ++m) {
                        const auto i = markers_[m];
                        if (show_->markers[i].hold) { state_ = State::Held; heldMarker_ = static_cast<int>(i); }
                    }
                } else {
                    while (nextMarker_ < markers_.size() && show_->markers[markers_[nextMarker_]].time == position_) {
                        const auto i = markers_[nextMarker_++]; const auto& marker = show_->markers[i];
                        if (marker.timing == MidiTiming::Arrival && !suppressSeekCue) dispatch(static_cast<int>(i), cueSource, cueEpoch, cueSource2, cueEpoch2);
                        if (marker.hold) { state_ = State::Held; heldMarker_ = static_cast<int>(i); }
                        else if (marker.timing == MidiTiming::Resume && !suppressSeekCue) dispatch(static_cast<int>(i), cueSource, cueEpoch, cueSource2, cueEpoch2);
                    }
                }
                if (state_ == State::Playing && position_ >= show_->duration) {
                    state_ = State::Ended; for (std::size_t i = 0; i < voices_.size(); ++i) cut(i);
                    std::fill(voices_.begin(), voices_.end(), Voice{}); active_.clear();
                }
            }
            bool ready = true;
            // Resolve all sources before mixing: a cache miss freezes the whole
            // sample clock, so tracks remain aligned and no source frames are lost.
            // Do NOT mutate Dormant async voices to Running until !waiting && !rolledBackOnReady.
            for (std::size_t cursor = 0; cursor < active_.size();) {
                const auto i = active_[cursor]; const auto& clip = show_->clips[i]; const auto& track = show_->tracks[clip.track]; auto& voice = voices_[i];
                voice.ready = false;
                if (!clip.contains(position_) && state_ == State::Playing) {
                    // When deferStartCommit is true, do not swap-remove expired clips from active_
                    // before readiness succeeds, or rollback via active_.resize(savedActiveSize) could
                    // leave a newly appended clip in an earlier slot and duplicate it on retry.
                    if (deferStartCommit) { ++cursor; continue; }
                    cut(i); voice = {}; active_[cursor] = active_.back(); active_.pop_back(); continue;
                }
                ++cursor;
                Frame source;
                if (track.async) {
                    const bool startingAsync = voice.state == VoiceState::Dormant && clip.contains(position_) && advancingAtBoundary && state_ != State::Ended;
                    if (!startingAsync && voice.state != VoiceState::Running) continue;
                    source = startingAsync ? (clip.offset + position_ - clip.start) : voice.source;
                } else {
                    if (state_ != State::Playing || !clip.contains(position_)) continue;
                    source = clip.offset + position_ - clip.start;
                }
                const Frame length = sourceLength(clip);
                if (length <= 0) continue;
                if (source >= length) {
                    if (clip.loop) source = clip.offset + (source - clip.offset) % std::max(Frame{1}, length - clip.offset);
                    else { if (track.async && voice.state == VoiceState::Running) voice.state = VoiceState::Finished; continue; }
                }
                if (source < 0 || source >= length) continue;
                if (track.async && voice.state == VoiceState::Running) voice.source = source; // Keep loops' cursors bounded, including long holds.
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
            bool rolledBackOnReady = false;
            if (!waiting && f == 0 && (pendingStart.active || pendingStart.wasHeld)) {
                if (pendingStart.active && pendingStart.source != CommandSource::Operator &&
                    !isSourceValid(pendingStart.source, pendingStart.sourceEpoch)) {
                    state_ = pendingStart.prevState;
                    heldMarker_ = pendingStart.prevHeldMarker;
                    pendingStart.active = false;
                    pendingStart.wasHeld = false;
                    pendingStart.seekCueActive = false;
                    rolledBackOnReady = true;
                } else if (pendingStart.wasHeld && pendingStart.resumeSource != CommandSource::Operator &&
                           !isSourceValid(pendingStart.resumeSource, pendingStart.resumeEpoch)) {
                    state_ = pendingStart.resumePrevState;
                    heldMarker_ = pendingStart.resumePrevHeldMarker;
                    pendingStart.active = false;
                    pendingStart.wasHeld = false;
                    pendingStart.seekCueActive = false;
                    rolledBackOnReady = true;
                } else if (pendingStart.wasHeld) {
                    // Disk is ready and the initiating resume is still valid: emit hold-resume MIDI once.
                    for (auto m = pendingStart.begin; m < pendingStart.end; ++m) {
                        const auto i = markers_[m]; const auto& marker = show_->markers[i];
                        if (marker.hold && marker.timing == MidiTiming::Resume)
                            dispatch(static_cast<int>(i), pendingStart.resumeSource, pendingStart.resumeEpoch);
                    }
                    pendingStart.wasHeld = false;
                }
            }
            if (deferStartCommit) {
                if (waiting || rolledBackOnReady) {
                    nextStart_ = savedNextStart;
                    nextMarker_ = savedNextMarker;
                    active_.resize(savedActiveSize);
                    if (waiting) {
                        state_ = savedBoundaryState;
                        heldMarker_ = savedBoundaryHeld;
                    }
                } else {
                    // Disk is ready and start authorization is valid: now consume boundary markers and emit MIDI.
                    while (nextMarker_ < markers_.size() && show_->markers[markers_[nextMarker_]].time == position_) {
                        const auto i = markers_[nextMarker_++]; const auto& marker = show_->markers[i];
                        if (marker.timing == MidiTiming::Arrival && !suppressSeekCue) dispatch(static_cast<int>(i), cueSource, cueEpoch, cueSource2, cueEpoch2);
                        if (marker.hold) { state_ = State::Held; heldMarker_ = static_cast<int>(i); }
                        else if (marker.timing == MidiTiming::Resume && !suppressSeekCue) dispatch(static_cast<int>(i), cueSource, cueEpoch, cueSource2, cueEpoch2);
                    }
                    if (pendingStart.active && pendingStart.source != CommandSource::Operator &&
                        !isSourceValid(pendingStart.source, pendingStart.sourceEpoch)) {
                        nextStart_ = savedNextStart;
                        nextMarker_ = savedNextMarker;
                        active_.resize(savedActiveSize);
                        state_ = pendingStart.prevState;
                        heldMarker_ = pendingStart.prevHeldMarker;
                        pendingStart.active = false;
                        pendingStart.wasHeld = false;
                        pendingStart.seekCueActive = false;
                        rolledBackOnReady = true;
                    }
                }
            }
            if (deferStartCommit && !waiting && !rolledBackOnReady && state_ == State::Playing) {
                // Now that readiness and authorization succeeded, remove clips that ended at position_.
                for (std::size_t cursor = 0; cursor < active_.size();) {
                    const auto i = active_[cursor];
                    if (!show_->clips[i].contains(position_)) {
                        cut(i); voices_[i] = {}; active_[cursor] = active_.back(); active_.pop_back(); continue;
                    }
                    ++cursor;
                }
            }
            // Voices that stop sounding fade out. A miss fades every voice; normal voices fade back in once ready.
            for (const auto i : active_) if (waiting || rolledBackOnReady || !voices_[i].ready) cut(i);
            if (!waiting && !rolledBackOnReady) for (const auto i : active_) {
                const auto& clip = show_->clips[i]; const auto& track = show_->tracks[clip.track]; auto& voice = voices_[i];
                if (track.async && voice.state == VoiceState::Dormant && clip.contains(position_) && advancingAtBoundary && state_ != State::Ended) {
                    Frame startSource = clip.offset + position_ - clip.start;
                    const Frame length = sourceLength(clip);
                    if (length > 0 && startSource >= length) {
                        if (clip.loop) startSource = clip.offset + (startSource - clip.offset) % std::max(Frame{1}, length - clip.offset);
                        else { voice.state = VoiceState::Finished; continue; }
                    }
                    if (length > 0 && startSource >= 0 && startSource < length) {
                        voice.state = VoiceState::Running;
                        voice.source = startSource;
                    }
                }
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
        if (!waiting) {
            if (f == 0) {
                pendingStart.active = false;
                pendingStart.wasHeld = false;
                pendingStart.seekCueActive = false;
            }
            if (state_ == State::Playing) ++position_;
        }
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
    publishedMarker_.store(heldMarker_, std::memory_order_release);
    publishedVoices_.store(active, std::memory_order_release);
    publishedPeak_.store(peak, std::memory_order_relaxed);
    // Publish applied command count last (release) so readers acquiring it see the updated state and voices.
    commandsApplied_.store(applied_, std::memory_order_release);
}
}
