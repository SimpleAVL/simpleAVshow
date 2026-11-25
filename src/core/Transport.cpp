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
    for (auto& v : savedSeekVoices_) v.assign(show_->clips.size(), {});
    for (auto& a : savedSeekActive_) { a.clear(); a.reserve(show_->clips.size()); }
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
bool Transport::rollbackRevokedSeeks(PendingPlayStart& pendingStart, bool onlyAfterPlay) noexcept {
    bool rolledBack = false;
    auto compactBuriedInvalid = [&]() noexcept {
        if (pendingStart.seekRollbackCount == 2 &&
            !isSourceValid(pendingStart.seekRollbacks[0].source, pendingStart.seekRollbacks[0].sourceEpoch)) {
            auto& older = pendingStart.seekRollbacks[0];
            auto& newer = pendingStart.seekRollbacks[1];
            newer.prevPos = older.prevPos;
            newer.prevState = older.prevState;
            newer.prevHeldMarker = older.prevHeldMarker;
            newer.prevNextStart = older.prevNextStart;
            newer.prevNextMarker = older.prevNextMarker;
            newer.prevWasHeld = older.prevWasHeld;
            newer.prevResumeBegin = older.prevResumeBegin;
            newer.prevResumeEnd = older.prevResumeEnd;
            newer.prevResumeSource = older.prevResumeSource;
            newer.prevResumeEpoch = older.prevResumeEpoch;
            newer.prevResumePrevState = older.prevResumePrevState;
            newer.prevResumePrevHeldMarker = older.prevResumePrevHeldMarker;
            newer.prevSeekCueActive = older.prevSeekCueActive;
            newer.prevSeekCueSource = older.prevSeekCueSource;
            newer.prevSeekCueEpoch = older.prevSeekCueEpoch;
            if (older.afterPlay) {
                newer.afterPlay = true;
                newer.savedPlayPrevState = older.savedPlayPrevState;
                newer.savedPlayPrevHeld = older.savedPlayPrevHeld;
            } else if (newer.afterPlay) {
                // older ran before Play while newer ran after Play; the compacted rollback now
                // spans back before Play, so roll it back after Play verification (afterPlay = false)
                // to preserve a still-valid Operator Play and reconstruct its hold-resume MIDI.
                newer.afterPlay = false;
                newer.savedPlayPrevState = older.prevState;
                newer.savedPlayPrevHeld = older.prevHeldMarker;
            }
            // savedSeekVoices_[0] and savedSeekActive_[0] already hold older's snapshot.
            pendingStart.seekRollbacks[0] = newer;
            pendingStart.seekRollbackCount = 1;
        }
    };
    compactBuriedInvalid();
    while (pendingStart.seekRollbackCount > 0) {
        auto& top = pendingStart.seekRollbacks[pendingStart.seekRollbackCount - 1];
        if (isSourceValid(top.source, top.sourceEpoch)) break;
        if (onlyAfterPlay && !top.afterPlay) break;
        const int slot = --pendingStart.seekRollbackCount;
        const SeekRollback entry = pendingStart.seekRollbacks[slot];
        position_ = entry.prevPos;
        nextStart_ = entry.prevNextStart;
        nextMarker_ = entry.prevNextMarker;
        active_ = savedSeekActive_[slot];
        for (std::size_t i = 0; i < voices_.size(); ++i) {
            const bool wasCut = !voices_[i].audible && savedSeekVoices_[slot][i].audible;
            voices_[i] = savedSeekVoices_[slot][i];
            if (wasCut) {
                voices_[i].audible = false;
                voices_[i].attack = fadeFrames;
            }
        }
        if (entry.afterPlay) {
            state_ = entry.prevState;
            heldMarker_ = entry.prevHeldMarker;
            if (pendingStart.active) {
                pendingStart.prevState = entry.savedPlayPrevState;
                pendingStart.prevHeldMarker = entry.savedPlayPrevHeld;
            }
        } else if (state_ != State::Playing) {
            state_ = entry.prevState;
            heldMarker_ = entry.prevHeldMarker;
        } else if (pendingStart.active) {
            pendingStart.prevState = entry.prevState;
            pendingStart.prevHeldMarker = entry.prevHeldMarker;
            if (!entry.prevWasHeld && entry.prevState == State::Held) {
                // A valid Play ran after this revoked Seek; without the Seek, that Play would
                // have continued the Hold at entry.prevPos, so synthesize its deferred resume MIDI.
                std::size_t begin = nextMarker_;
                while (begin > 0 && show_->markers[markers_[begin - 1]].time == position_) --begin;
                pendingStart.wasHeld = true;
                pendingStart.begin = begin;
                pendingStart.end = nextMarker_;
                pendingStart.resumeSource = pendingStart.source;
                pendingStart.resumeEpoch = pendingStart.sourceEpoch;
                pendingStart.resumePrevState = entry.prevState;
                pendingStart.resumePrevHeldMarker = entry.prevHeldMarker;
            }
        }
        if (entry.prevWasHeld) {
            pendingStart.wasHeld = true;
            pendingStart.begin = entry.prevResumeBegin;
            pendingStart.end = entry.prevResumeEnd;
            pendingStart.resumeSource = entry.prevResumeSource;
            pendingStart.resumeEpoch = entry.prevResumeEpoch;
            pendingStart.resumePrevState = entry.prevResumePrevState;
            pendingStart.resumePrevHeldMarker = entry.prevResumePrevHeldMarker;
        }
        pendingStart.seekCueActive = entry.prevSeekCueActive;
        pendingStart.seekCueSource = entry.prevSeekCueSource;
        pendingStart.seekCueEpoch = entry.prevSeekCueEpoch;
        prime();
        rolledBack = true;
        compactBuriedInvalid();
    }
    return rolledBack;
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
        // A valid Pause supersedes any pending seek rollback's pre-seek Playing state so revoking
        // an agent seek later can never resume playback past an explicit Pause.
        for (int s = 0; s < pendingStart.seekRollbackCount; ++s) {
            if (pendingStart.seekRollbacks[s].prevState == State::Playing)
                pendingStart.seekRollbacks[s].prevState = State::Paused;
        }
        break;
    case Action::Seek: {
        // Re-verify agent authorization immediately before cutting voices or moving the playhead
        // so callback preemption between batch validation and Seek cannot apply a revoked seek.
        if (command.source != CommandSource::Operator && !isSourceValid(command.source, command.sourceEpoch))
            break;
        // Seeking cancels any pending hold-resume MIDI, but MUST preserve pendingStart.active
        // and its initiating source/epoch when Play preceded Seek in the same batch so a queued
        // Seek cannot bypass the first-sample Play authorization check.
        if (command.source != CommandSource::Operator) {
            // Keep at most one rollback slot per revocable domain (ExternalAgent vs ChatAgent)
            // in LIFO order so revoking Chat preserves an earlier still-authorized External seek.
            if (pendingStart.seekRollbackCount == 2 &&
                pendingStart.seekRollbacks[0].source == command.source &&
                pendingStart.seekRollbacks[0].sourceEpoch == command.sourceEpoch) {
                auto& older = pendingStart.seekRollbacks[0];
                auto& newer = pendingStart.seekRollbacks[1];
                newer.prevPos = older.prevPos;
                newer.prevState = older.prevState;
                newer.prevHeldMarker = older.prevHeldMarker;
                newer.prevNextStart = older.prevNextStart;
                newer.prevNextMarker = older.prevNextMarker;
                newer.prevWasHeld = older.prevWasHeld;
                newer.prevResumeBegin = older.prevResumeBegin;
                newer.prevResumeEnd = older.prevResumeEnd;
                newer.prevResumeSource = older.prevResumeSource;
                newer.prevResumeEpoch = older.prevResumeEpoch;
                newer.prevResumePrevState = older.prevResumePrevState;
                newer.prevResumePrevHeldMarker = older.prevResumePrevHeldMarker;
                newer.prevSeekCueActive = older.prevSeekCueActive;
                newer.prevSeekCueSource = older.prevSeekCueSource;
                newer.prevSeekCueEpoch = older.prevSeekCueEpoch;
                if (older.afterPlay) {
                    newer.afterPlay = true;
                    newer.savedPlayPrevState = older.savedPlayPrevState;
                    newer.savedPlayPrevHeld = older.savedPlayPrevHeld;
                } else if (newer.afterPlay) {
                    newer.savedPlayPrevState = older.prevState;
                    newer.savedPlayPrevHeld = older.prevHeldMarker;
                }
                pendingStart.seekRollbacks[0] = newer;
                pendingStart.seekRollbackCount = 1;
            }
            const bool sameAsTop = pendingStart.seekRollbackCount > 0 &&
                pendingStart.seekRollbacks[pendingStart.seekRollbackCount - 1].source == command.source &&
                pendingStart.seekRollbacks[pendingStart.seekRollbackCount - 1].sourceEpoch == command.sourceEpoch;
            if (!sameAsTop && pendingStart.seekRollbackCount < 2) {
                const int slot = pendingStart.seekRollbackCount++;
                SeekRollback entry;
                entry.source = command.source;
                entry.sourceEpoch = command.sourceEpoch;
                entry.prevPos = position_;
                entry.prevState = state_;
                entry.prevHeldMarker = heldMarker_;
                entry.prevNextStart = nextStart_;
                entry.prevNextMarker = nextMarker_;
                entry.prevWasHeld = pendingStart.wasHeld;
                entry.prevResumeBegin = pendingStart.begin;
                entry.prevResumeEnd = pendingStart.end;
                entry.prevResumeSource = pendingStart.resumeSource;
                entry.prevResumeEpoch = pendingStart.resumeEpoch;
                entry.prevResumePrevState = pendingStart.resumePrevState;
                entry.prevResumePrevHeldMarker = pendingStart.resumePrevHeldMarker;
                entry.afterPlay = pendingStart.active;
                entry.savedPlayPrevState = pendingStart.prevState;
                entry.savedPlayPrevHeld = pendingStart.prevHeldMarker;
                entry.prevSeekCueActive = pendingStart.seekCueActive;
                entry.prevSeekCueSource = pendingStart.seekCueSource;
                entry.prevSeekCueEpoch = pendingStart.seekCueEpoch;
                pendingStart.seekRollbacks[slot] = entry;
                savedSeekVoices_[slot] = voices_;
                savedSeekActive_[slot] = active_;
            }
        } else {
            pendingStart.seekRollbackCount = 0;
        }
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
        // Operator seeks commit immediately; revocable agent seeks defer bumping the global MIDI
        // epoch until the seek actually commits so a rolled-back seek never drops earlier Operator MIDI.
        if (command.source == CommandSource::Operator) epoch_.fetch_add(1);
        if (pendingStart.seekRollbackCount > 0 && !isSourceValid(command.source, command.sourceEpoch))
            rollbackRevokedSeeks(pendingStart, false);
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
        // Roll back any already-revoked stalled agent Seek/Play from a previous callback BEFORE
        // applying this callback's commands (e.g. an Operator Pause or Seek queued after revocation).
        rollbackRevokedSeeks(pendingStart, true);
        if (pendingStart.active && pendingStart.source != CommandSource::Operator &&
            !isSourceValid(pendingStart.source, pendingStart.sourceEpoch)) {
            state_ = pendingStart.prevState;
            heldMarker_ = pendingStart.prevHeldMarker;
            pendingStart.active = false;
        }
        if (pendingStart.wasHeld && pendingStart.resumeSource != CommandSource::Operator &&
            !isSourceValid(pendingStart.resumeSource, pendingStart.resumeEpoch)) {
            state_ = pendingStart.resumePrevState;
            heldMarker_ = pendingStart.resumePrevHeldMarker;
            pendingStart.wasHeld = false;
            pendingStart.active = false;
        }
        rollbackRevokedSeeks(pendingStart, false);

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
    // Roll back any revoked agent Seek/Play in LIFO order: Seeks that followed Play roll back
    // before Play, and Seeks that preceded Play roll back after Play.
    if (pendingStart.seekRollbackCount > 0 || pendingStart.active || pendingStart.wasHeld) {
        if (urgentStop_.load(std::memory_order_acquire)) {
            applyUrgentStop(countQueued);
            pendingStart = {};
        } else {
            rollbackRevokedSeeks(pendingStart, true);
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
            rollbackRevokedSeeks(pendingStart, false);
        }
    }
    if (state_ != State::Playing) {
        if (pendingStart.seekRollbackCount > 0) epoch_.fetch_add(1);
        pendingStart.active = false;
        pendingStart.wasHeld = false;
        pendingStart.seekCueActive = false;
        pendingStart.seekRollbackCount = 0;
    } else if (count == 0) {
        if (pendingStart.seekRollbackCount > 0) epoch_.fetch_add(1);
        pendingStart.seekRollbackCount = 0;
    }
    float peak = 0; bool waiting = false;
    // Stay on the start-of-callback transaction boundary until a sample actually commits
    // (!waiting && !rolledBackOnReady), so a rollback on f == 0 still dispatches restored
    // Operator hold-resume MIDI before advancing to the next frame in the same callback.
    bool startBoundary = true;
    for (unsigned f = 0; f < count; ++f) {
        if (waiting && releasing_.empty()) break; // Remaining device samples stay silent; retry at the same show/source time.
        float* const frame = output ? output + static_cast<std::size_t>(f) * channels : nullptr;
        bool rolledBackOnReady = false;
        if (!waiting) {
            if (startBoundary && (pendingStart.seekRollbackCount > 0 || pendingStart.active || pendingStart.wasHeld)) {
                rollbackRevokedSeeks(pendingStart, true);
                if (state_ == State::Playing) {
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
                rollbackRevokedSeeks(pendingStart, false);
            }
            const bool advancingAtBoundary = state_ == State::Playing;
            // Defer consuming markers/MIDI, starting dormant async voices, and removing expired clips
            // until disk readiness and live authorization both succeed so a cache miss (including on
            // a seek into an async clip at a hold) never leaves holds consumed while voices stay Dormant.
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
            bool holdAtPos = false;
            bool willEnd = false;
            if (advancingAtBoundary) {
                if (startBoundary) {
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
                // Preview whether a hold marker exists at position_ WITHOUT mutating state_ or
                // heldMarker_ yet, so normal clips respect the hold during readiness checking while
                // any revoked-seek rollback still sees the true Playing state (e.g. Operator Play).
                for (std::size_t m = nextMarker_; m < markers_.size() && show_->markers[markers_[m]].time == position_; ++m) {
                    if (show_->markers[markers_[m]].hold) holdAtPos = true;
                }
                willEnd = !holdAtPos && position_ >= show_->duration;
            }
            bool ready = true;
            bool hasExpired = false;
            const bool playNormal = state_ == State::Playing && !holdAtPos;
            // Resolve all sources before mixing: a cache miss freezes the whole
            // sample clock, so tracks remain aligned and no source frames are lost.
            // Do NOT mutate Dormant async voices to Running or Finished until !waiting && !rolledBackOnReady.
            if (!willEnd) for (std::size_t cursor = 0; cursor < active_.size(); ++cursor) {
                const auto i = active_[cursor]; const auto& clip = show_->clips[i]; const auto& track = show_->tracks[clip.track]; auto& voice = voices_[i];
                voice.ready = false;
                if (!clip.contains(position_) && playNormal) {
                    hasExpired = true;
                    continue;
                }
                Frame source;
                if (track.async) {
                    const bool startingAsync = voice.state == VoiceState::Dormant && clip.contains(position_) && advancingAtBoundary;
                    if (!startingAsync && voice.state != VoiceState::Running) continue;
                    source = startingAsync ? (clip.offset + position_ - clip.start) : voice.source;
                } else {
                    if (!playNormal || !clip.contains(position_)) continue;
                    source = clip.offset + position_ - clip.start;
                }
                const Frame length = sourceLength(clip);
                if (length <= 0) continue;
                if (source >= length) {
                    if (clip.loop) source = clip.offset + (source - clip.offset) % std::max(Frame{1}, length - clip.offset);
                    else continue;
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
            bool seekRolledBackOnReady = false;
            if (!waiting && startBoundary && (pendingStart.seekRollbackCount > 0 || pendingStart.active || pendingStart.wasHeld)) {
                if (rollbackRevokedSeeks(pendingStart, true)) { rolledBackOnReady = true; seekRolledBackOnReady = true; }
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
                }
                if (rollbackRevokedSeeks(pendingStart, false)) { rolledBackOnReady = true; seekRolledBackOnReady = true; }
            }
            if (advancingAtBoundary) {
                if (waiting || rolledBackOnReady) {
                    if (!seekRolledBackOnReady) {
                        nextStart_ = savedNextStart;
                        nextMarker_ = savedNextMarker;
                        active_.resize(savedActiveSize);
                    }
                    if (waiting) {
                        state_ = savedBoundaryState;
                        heldMarker_ = savedBoundaryHeld;
                    }
                } else {
                    // Re-verify agent Seek, Play, and hold-resume authorization ONCE right before
                    // mutating boundary state, bumping epoch_, or dispatching any MIDI events.
                    // Because state_ is still Playing here (never tentatively Held), rollback of a
                    // revoked Seek before Operator Play sees State::Playing and preserves the Play.
                    bool revoked = false;
                    bool seekRevoked = false;
                    if (startBoundary && (pendingStart.seekRollbackCount > 0 || pendingStart.active || pendingStart.wasHeld)) {
                        if (rollbackRevokedSeeks(pendingStart, true)) { revoked = true; seekRevoked = true; }
                        if (pendingStart.active && pendingStart.source != CommandSource::Operator &&
                            !isSourceValid(pendingStart.source, pendingStart.sourceEpoch)) {
                            state_ = pendingStart.prevState;
                            heldMarker_ = pendingStart.prevHeldMarker;
                            pendingStart.active = false;
                            pendingStart.wasHeld = false;
                            pendingStart.seekCueActive = false;
                            revoked = true;
                        } else if (pendingStart.wasHeld && pendingStart.resumeSource != CommandSource::Operator &&
                                   !isSourceValid(pendingStart.resumeSource, pendingStart.resumeEpoch)) {
                            state_ = pendingStart.resumePrevState;
                            heldMarker_ = pendingStart.resumePrevHeldMarker;
                            pendingStart.active = false;
                            pendingStart.wasHeld = false;
                            pendingStart.seekCueActive = false;
                            revoked = true;
                        }
                        if (rollbackRevokedSeeks(pendingStart, false)) { revoked = true; seekRevoked = true; }
                        if (revoked && !seekRevoked) {
                            nextStart_ = savedNextStart;
                            nextMarker_ = savedNextMarker;
                            active_.resize(savedActiveSize);
                        }
                    }
                    if (revoked) {
                        rolledBackOnReady = true;
                    } else {
                        // Transaction is now committed (no further rollback on this sample):
                        // 1) Bump global MIDI epoch for any committed agent Seek.
                        // 2) Dispatch deferred hold-resume MIDI (cannot be rolled back to Held now).
                        // 3) Consume boundary markers, apply holds, and dispatch landing MIDI.
                        if (startBoundary && pendingStart.seekRollbackCount > 0) epoch_.fetch_add(1);
                        if (startBoundary && pendingStart.wasHeld) {
                            for (auto m = pendingStart.begin; m < pendingStart.end; ++m) {
                                const auto i = markers_[m]; const auto& marker = show_->markers[i];
                                if (marker.hold && marker.timing == MidiTiming::Resume)
                                    dispatch(static_cast<int>(i), pendingStart.resumeSource, pendingStart.resumeEpoch);
                            }
                            pendingStart.wasHeld = false;
                        }
                        while (nextMarker_ < markers_.size() && show_->markers[markers_[nextMarker_]].time == position_) {
                            const auto i = markers_[nextMarker_++]; const auto& marker = show_->markers[i];
                            if (marker.timing == MidiTiming::Arrival && !suppressSeekCue) dispatch(static_cast<int>(i), cueSource, cueEpoch, cueSource2, cueEpoch2);
                            if (marker.hold) { state_ = State::Held; heldMarker_ = static_cast<int>(i); }
                            else if (marker.timing == MidiTiming::Resume && !suppressSeekCue) dispatch(static_cast<int>(i), cueSource, cueEpoch, cueSource2, cueEpoch2);
                        }
                        if (willEnd && state_ == State::Playing) {
                            state_ = State::Ended; for (std::size_t i = 0; i < voices_.size(); ++i) cut(i);
                            std::fill(voices_.begin(), voices_.end(), Voice{}); active_.clear();
                        } else if (hasExpired && state_ == State::Playing) {
                            for (std::size_t cursor = 0; cursor < active_.size();) {
                                const auto i = active_[cursor];
                                if (!show_->clips[i].contains(position_)) {
                                    cut(i); voices_[i] = {}; active_[cursor] = active_.back(); active_.pop_back(); continue;
                                }
                                ++cursor;
                            }
                        }
                    }
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
                if (track.async && voice.state == VoiceState::Running) {
                    const Frame length = sourceLength(clip);
                    if (length > 0 && voice.source >= length && !clip.loop) {
                        voice.state = VoiceState::Finished;
                        continue;
                    }
                    ++voice.source;
                }
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
        if (!waiting && !rolledBackOnReady) {
            if (startBoundary) {
                pendingStart.active = false;
                pendingStart.wasHeld = false;
                pendingStart.seekCueActive = false;
                pendingStart.seekRollbackCount = 0;
                startBoundary = false;
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
