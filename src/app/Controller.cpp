#include "app/Controller.h"
#include "media/AudioSources.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSet>
#include <QMetaObject>
#include <QPainter>
#include <QStandardPaths>
#include <chrono>
#include <thread>
#include <set>

namespace sav {
namespace {
// What a failed video layer shows: its last picture if it ever drew one, otherwise nothing.
QString videoFallback(bool hadPicture) {
    return hadPicture ? QStringLiteral(" The picture holds its last frame; audio continues.")
                      : QStringLiteral(" This clip shows nothing until its file can be read again; audio continues.");
}
}

Controller::Controller(QObject* parent) : QObject(parent), project(newProject()), device(transport) {
    midiName_ = QSettings().value("midi/port").toString();
    transport.setLateAudio(QSettings().value("audio/late").toString() == "gap" ? LateAudio::Gap : LateAudio::Freeze);
    sav::setAudioPreparation(QSettings().value("audio/sources").toString() == "prepared" ? AudioPreparation::Prepared : AudioPreparation::Stream);
    arm(0);
    timer_.setInterval(16);
    connect(&timer_, &QTimer::timeout, this, [this] {
        // A stream that stops calling back without reporting an error has stalled just the same.
        if (device.callbacks() != heartbeat_ || !device.running()) { heartbeat_ = device.callbacks(); stall_.restart(); }
        if (transport.state() != State::Held) wasHeld_ = false; // Each hold, however short the gap, is a new one.
        {
            // Leaving a live show (a seek, the end, Stop All) drops what its checks found: a stale list would
            // keep fresh readers of restored files from holding the picture while they open.
            const State now = transport.state();
            const bool live = now == State::Playing || now == State::Held || transport.asyncVoices() || (now == State::Paused && pausedLive_);
            if (wasLive_ && !live) resetMediaState();
            wasLive_ = live;
        }
        const bool stalled = configured_ && device.running() && stall_.hasExpired(750);
        if (configured_ && (!device.running() || stalled)) {
            const QString reason = stalled ? QString("stopped responding") : device.lastError().isEmpty() ? QString("stopped") : "failed: " + device.lastError();
            configured_ = false; device.close();
            // Once the device is closed, it is safe to render a zero-sized control block.
            transport.command({Action::StopAll}); transport.render(nullptr, 0, 0); pausedLive_ = false; resetMediaState();
            report("Audio output " + reason + ". Press Play to reconnect, or choose another device in Outputs.");
            playbackProblem = status;
        }
        if (transport.eventOverflow() && transport.state() != State::Stopped) {
            stopAll(); report("MIDI event queue overflow. Playback is stopped; inspect the project.");
        }
        if (transport.audioReadFailed() && transport.state() != State::Stopped) {
            stopAll(); report("Audio source changed or could not be read. Playback is stopped; restore missing media and press Play to retry, or reopen the project to rebuild changed caches."); playbackProblem = status;
        }
        if (midiWatch_.hasExpired(1000)) {
            midiWatch_.restart(); watchMidi(); closeIdleStreams(); retryPackageRemovals();
            // During a live show, media is checked in the background; a hold says when a later cue's file goes missing.
            const State state = transport.state();
            const bool live = state == State::Playing || state == State::Held || transport.asyncVoices() || (state == State::Paused && pausedLive_);
            if (!busy && live) {
                wasLive_ = true; // This check's results belong to a live show.
                QStringList offline, needed; mediaToCheck(offline, needed);
                // The finished check is read and the next one reserved under one lock, so no result goes unseen.
                const auto missing = lastMissingMediaAndCheck(offline, needed);
                // Each file found again reopens its pictures (a reader that failed while it was gone stays
                // failed), whatever else is still missing.
                const QSet<QString> still(missing.cbegin(), missing.cend());
                QSet<QString> back; for (const auto& path : missingNow_) if (!still.contains(path)) back.insert(path);
                if (!back.isEmpty()) {
                    for (const auto& clip : project.show.clips) {
                        const auto asset = project.assets.constFind(QString::fromStdString(clip.assetId));
                        if (asset != project.assets.cend() && back.contains(asset->path)) retireVideo(clip.id);
                    }
                    for (const auto& path : back) missingSeen_.remove(path); // Its readers are new now.
                    lastComposed_ = -1;
                }
                const bool changed = missing != missingNow_, held = state == State::Held;
                missingNow_ = missing; missingWarning_ = missing.isEmpty() ? QString() : mediaWarning(missing);
                for (const auto& path : missing) missingSeen_.insert(path);
                // The warning replaces only itself (or nothing), never another problem; the hold banner shows it
                // on its own line regardless.
                const bool ours = !reportedProblem_.isEmpty() && playbackProblem == reportedProblem_;
                if (missing.isEmpty()) {
                    if (ours) {
                        playbackProblem.clear(); report("Missing media is back online.");
                        promoteVideoWarning(); // A picture still frozen for another reason keeps its warning.
                        if (!playbackProblem.isEmpty()) report(playbackProblem);
                    }
                    reportedProblem_.clear();
                } else if ((ours && changed) || (held && (!wasHeld_ || changed) && (ours || playbackProblem.isEmpty()))) {
                    report(missingWarning_); playbackProblem = reportedProblem_ = status; // Each hold says it again.
                }
                wasHeld_ = held;
            }
        }
        emit tick();
    }); timer_.start(); midiWatch_.start(); stall_.start();
    autosave_.setInterval(60000);
    connect(&autosave_, &QTimer::timeout, this, [this] {
        // Extracted package media is temporary, and generated media has no file to reference yet.
        if (!dirty || busy || project.packageDirectory) return;
        const bool untitled = path.isEmpty();
        if (untitled && std::any_of(project.assets.cbegin(), project.assets.cend(), [](const MediaAsset& a) { return a.path.isEmpty(); })) return;
        try {
            if (untitled) QDir().mkpath(QFileInfo(autosavePath()).absolutePath());
            writeProject(project, autosavePath());
        } catch (const std::exception& e) { report("Autosave failed: " + QString::fromUtf8(e.what())); }
    }); autosave_.start();
}
Controller::~Controller() {
    timer_.stop(); stopMidiThread(); device.close();
}
void Controller::report(QString text) { status = text; emit message(text); }
QString Controller::untitledAutosavePath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/Untitled.savshow.autosave";
}
QString Controller::autosavePath() const { return path.isEmpty() ? untitledAutosavePath() : path + ".autosave"; }
void Controller::discardAutosave() { QFile::remove(autosavePath()); }
bool Controller::canEdit(bool reportFailure) {
    const bool allowed = !showMode && !busy && transport.state() != State::Playing && transport.state() != State::Held &&
        transport.asyncVoices() == 0;
    if (!allowed && reportFailure) report(showMode ? "Leave show mode to edit" : "Use Stop All before editing; async audio or video may still be playing");
    return allowed;
}
bool Controller::edit(const std::function<void(Project&)>& mutation) {
    if (!canEdit()) return false;
    try {
        Project next = project; mutation(next); bindAudio(next); validate(next);
        const Frame time = std::min(position(), next.show.duration);
        // The previous state moves into history rather than being copied again.
        undo_.push_back(std::move(project)); if (undo_.size() > 32) undo_.pop_front(); redo_.clear();
        project = std::move(next); dirty = true; arm(time); emit changed(); return true;
    } catch (const std::exception& e) { report(QString::fromUtf8(e.what())); return false; }
}
void Controller::replace(Project next, QString projectPath) {
    validate(next);
    // Readers hold extracted package files open; Windows cannot remove them while open. The previous
    // project and its history (which may own an extraction) outlive re-arming, which releases the
    // transport's streamed decoders, and are destroyed at the end of this function.
    videos_.clear(); missingSeen_.clear(); clipFrames_.clear(); recovering_.clear(); videoProblems_.clear(); lastPicture_ = {};
    auto retired = std::move(project); auto retiredUndo = std::move(undo_), retiredRedo = std::move(redo_);
    project = std::move(next); path = std::move(projectPath); dirty = false; playbackProblem.clear();
    undo_.clear(); redo_.clear();
    if (arm(0)) report("Project ready. Space plays or pauses; Stop All stops async audio and video.");
    emit changed();
}
void Controller::undo() {
    if (!canEdit() || undo_.empty()) return;
    redo_.push_back(std::move(project)); project = std::move(undo_.back()); undo_.pop_back(); dirty = true;
    arm(std::min(position(), project.show.duration)); emit changed();
}
void Controller::redo() {
    if (!canEdit() || redo_.empty()) return;
    undo_.push_back(std::move(project)); project = std::move(redo_.back()); redo_.pop_back(); dirty = true;
    arm(std::min(position(), project.show.duration)); emit changed();
}
bool Controller::arm(Frame position) {
    pausedLive_ = false; resetMediaState(); // Arming starts over from position, with the show as it is now.
    // Callers have already committed their change; a failure (out of memory, no worker thread) must not
    // escape into Qt or leave the previous snapshot playing. The stream closes (so the device watchdog
    // keeps quiet), the transport disarms, and Play or the next edit arms again at the kept position.
    try {
        armOrThrow(position); armFailed_ = false;
        if (!armProblem_.isEmpty() && playbackProblem == armProblem_) playbackProblem.clear(); // Recovered.
        armProblem_.clear(); return true;
    } catch (const std::exception& e) {
        // The previous snapshot goes too, so nothing (a MIDI consumer started from Outputs) acts on it.
        stopMidiThread(); device.close(); configured_ = false; transport.disarm(); armed_.reset();
        armFailed_ = true; pendingPosition_ = std::clamp(position, Frame{0}, project.show.duration);
        report("Playback could not be prepared: " + QString::fromUtf8(e.what()) + ". Press Play to try again.");
        playbackProblem = armProblem_ = status; return false;
    }
}
void Controller::armOrThrow(Frame position) {
    stopMidiThread();
    // The callback must be stopped before configure; a healthy stream only pauses.
    const bool keep = configured_ && device.running();
    if (keep) device.pause(); else device.close();
    // A video warning about readers being replaced goes with them.
    for (const auto& [clip, problem] : videoProblems_) {
        if (playbackProblem == problem) playbackProblem.clear();
        if (status == problem) report(QString());
    }
    // missingSeen_ stays: a confirmed absence still needs a confirmed return, even after re-arming.
    videos_.clear(); videoErrors_.clear(); clipFrames_.clear(); recovering_.clear(); videoProblems_.clear(); lastPicture_ = {}; lastComposed_ = -1; lateFrames = 0;
    auto armed = std::make_shared<Show>(project.show);
    for (auto& clip : armed->clips) {
        if (!armed->tracks[clip.track].visual) continue;
        const auto asset = project.assets.constFind(QString::fromStdString(clip.assetId));
        if (asset != project.assets.cend() && asset->video && !asset->offline) clip.videoLength = frames(asset->duration);
    }
    armed_ = std::move(armed); transport.configure(armed_, position);
    if (keep && device.resume()) stall_.restart();
    else if (configured_) {
        QString error;
        if (!device.open(api_, deviceId_, channels_, buffer_, error)) { configured_ = false; report("Audio output unavailable: " + error); }
        stall_.restart();
    }
    startMidiThread();
}
void Controller::startMidiThread() {
    if (!armed_) return; // Nothing armed: no cues to send.
    const auto snapshot = armed_;
    midiStop_.store(false);
    // std::jthread is unavailable in the libc++ shipped with older macOS SDKs.
    midiThread_ = std::thread([this, snapshot] {
        while (!midiStop_.load()) {
            MidiEvent event;
            while (transport.takeMidi(event)) {
                if (event.epoch != transport.epoch() || event.marker < 0 || static_cast<std::size_t>(event.marker) >= snapshot->markers.size()) continue;
                const auto& marker = snapshot->markers[event.marker]; QString error;
                if (!midi.send(marker.message, error)) {
                    QMetaObject::invokeMethod(this, [this, error] { report(error); }, Qt::QueuedConnection);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
}
void Controller::stopMidiThread() {
    midiStop_.store(true);
    if (midiThread_.joinable()) midiThread_.join();
}
bool Controller::routesValid() const {
    for (const auto& track : project.show.tracks) {
        if (track.visual || track.muted) continue;
        if (track.left >= static_cast<int>(device.channels()) || track.right >= static_cast<int>(device.channels())) return false;
    }
    return true;
}
void Controller::toggle() {
    if (busy) return;
    const State state = transport.state(); const bool playing = state == State::Playing;
    const bool recovering = !playing && !playbackProblem.isEmpty();
    QString warning; // A problem this resume continues past; reconnecting a device must not clear it.
    if (!playing) {
        // A failed arm left nothing armed; preparing again (at any position sought since) comes first.
        if ((armFailed_ || !transport.armed()) && !arm(armFailed_ ? pendingPosition_ : position())) return;
        playbackProblem.clear();
        // Re-arming retries sources that were briefly unavailable; changed sources still need reopening.
        if (transport.audioReadFailed() && state != State::Held && !transport.asyncVoices() && !arm(position())) return;
        if (transport.audioReadFailed()) { report("Audio source is unreadable. Restore missing media and press Play to retry, or reopen the project to rebuild changed or damaged caches."); playbackProblem = status; return; }
        // Resuming a live show never blocks on picture or MIDI; it warns and continues.
        const bool resuming = state == State::Held || transport.asyncVoices() || (state == State::Paused && device.running());
        // Continuing a live show does not look for missing files itself: a stalled share would hold up the
        // cue. It warns with what the background check last found. Paused after Stop All or a seek is a
        // fresh start and checks.
        const bool live = state == State::Held || transport.asyncVoices() || (state == State::Paused && pausedLive_);
        // A fresh start first takes in what the last background check confirmed missing.
        if (!live) { const std::lock_guard lock(mediaCheck_->mutex); for (const auto& path : mediaCheck_->missing) missingSeen_.insert(path); }
        auto missing = live ? lastMissingMedia() : missingMedia();
        if (!live && !missingSeen_.isEmpty()) {
            // A file confirmed missing earlier counts as back only when a lookup confirms it; an inconclusive
            // one (a share still dropping out) keeps it missing.
            QStringList offline, needed; mediaToCheck(offline, needed);
            for (const auto& path : needed)
                if (missingSeen_.contains(path) && !missing.contains(path) && detail::presentIfKnown(path) != std::optional<bool>(true)) missing << path;
        }
        // A fresh start's own check is the latest: background checks continue from it.
        if (!live) {
            resetMediaState(); { const std::lock_guard lock(mediaCheck_->mutex); mediaCheck_->missing = missing; }
            for (const auto& path : missing) missingSeen_.insert(path); // Kept even if this start stops.
            // Pictures that failed while a file was gone open again now that it is back.
            if (missing.isEmpty()) {
                // Also readers of files seen missing since they opened (Stop All forgets the check, not this):
                // one may still be blocked on the old file without reporting an error.
                std::vector<std::string> failed(videoErrors_.begin(), videoErrors_.end());
                for (const auto& [clip, reader] : videos_) {
                    if (videoErrors_.contains(clip)) continue;
                    const auto found = std::find_if(project.show.clips.begin(), project.show.clips.end(), [&](const Clip& c) { return c.id == clip; });
                    const auto asset = found == project.show.clips.end() ? project.assets.cend() : project.assets.constFind(QString::fromStdString(found->assetId));
                    if (!reader->error().isEmpty() || (asset != project.assets.cend() && missingSeen_.contains(asset->path))) failed.push_back(clip);
                }
                missingSeen_.clear();
                for (const auto& clip : failed) retireVideo(clip);
                lastComposed_ = -1;
            }
        }
        if (!missing.isEmpty()) {
            report(mediaWarning(missing));
            // Tracked as the missing-media warning, so an edit, relink or stop that resolves it clears it.
            playbackProblem = reportedProblem_ = warning = status;
            if (!live) return; // Only a live show continues past missing media (it warned above).
        }
        if (needsMidi() && !prepareMidi()) { playbackProblem = midiProblem_ = status; if (!resuming) return; if (warning.isEmpty()) warning = status; }
        if (!prepareAudio()) { playbackProblem = status; return; }
        if (!routesValid()) { report("A track needs more output channels. Open Outputs to increase channels, or change the track's routing."); playbackProblem = status; return; }
    }
    const bool sent = transport.command({playing ? Action::Pause : Action::Play});
    if (sent) pausedLive_ = playing;
    if (sent && warning.isEmpty() && !playing) {
        // A video that still cannot play keeps its warning: Play resolves what it checked, not that.
        promoteVideoWarning();
        if (!playbackProblem.isEmpty()) warning = playbackProblem;
    }
    if (!sent) report("Transport command queue is full");
    else if (!warning.isEmpty()) { report(warning); playbackProblem = warning; }
    else if (recovering) report("Playback resumed."); // Replaces the resolved problem in the status bar.
}
void Controller::seek(Frame time) {
    if (busy || showMode) { report(showMode ? "Leave show mode to jump on the timeline" : "Media is loading"); return; }
    // Disarmed after a failed arm: the transport cannot take the seek, so it is where Play arms again.
    if (armFailed_) { pendingPosition_ = std::clamp(time, Frame{0}, project.show.duration); lastComposed_ = -1; return; }
    if (!transport.command({Action::Seek, time})) report("Transport command queue is full");
    if (!device.running()) transport.render(nullptr, 0, 0);
    lastComposed_ = -1; pausedLive_ = false;
}
void Controller::stopAll() {
    if (armFailed_) return; // Nothing is armed, so nothing plays; the transport could not take the command.
    pausedLive_ = false; resetMediaState();
    if (!transport.command({Action::StopAll})) { report("Transport command queue is full"); return; }
    if (!device.running()) transport.render(nullptr, 0, 0);
}
bool Controller::configureAudio(RtAudio::Api api, unsigned id, QString name, unsigned channels, unsigned buffer) {
    if (!canEdit()) return false;
    return openAudio(api, id, std::move(name), channels, buffer);
}
bool Controller::openAudio(RtAudio::Api api, unsigned id, QString name, unsigned channels, unsigned buffer) {
    api_ = api; deviceId_ = id; channels_ = channels; buffer_ = buffer; audioName = name;
    QString error;
    configured_ = device.open(api, id, channels, buffer, error); stall_.restart();
    if (configured_) {
        playbackProblem.clear();
        QSettings settings; settings.setValue("audio/api", static_cast<int>(api)); settings.setValue("audio/device", name);
        settings.setValue("audio/channels", channels); settings.setValue("audio/buffer", buffer);
        report(QString("%1 · %2 channels · 48 kHz · %3 samples").arg(name).arg(channels).arg(device.bufferFrames()));
    }
    else report("Audio output unavailable: " + error);
    return configured_;
}
bool Controller::configureMidi(int port) {
    if (!canEdit()) return false;
    return openMidi(port);
}
bool Controller::openMidi(int port) {
    stopMidiThread();
    QString error; const bool ok = midi.open(port, error); startMidiThread();
    // A reconnect resolves the MIDI problem only; a missing-media warning outlives it.
    if (ok) { midiName_ = midi.name(); QSettings().setValue("midi/port", midiName_); if (!midiProblem_.isEmpty() && playbackProblem == midiProblem_) playbackProblem.clear(); midiProblem_.clear(); }
    report(ok ? (port < 0 ? "MIDI output disconnected" : "MIDI output: " + midi.name()) : error); return ok;
}
void Controller::setLateAudio(LateAudio policy) {
    transport.setLateAudio(policy); QSettings().setValue("audio/late", policy == LateAudio::Gap ? "gap" : "freeze");
}
void Controller::setAudioPreparation(AudioPreparation preparation) {
    sav::setAudioPreparation(preparation); QSettings().setValue("audio/sources", preparation == AudioPreparation::Prepared ? "prepared" : "stream");
}
QStringList Controller::audioSources() const {
    QStringList paths;
    const auto collect = [&](const Project& p) { for (const auto& a : p.assets) if (a.audio && a.audio->storage && !a.offline && !paths.contains(a.path)) paths << a.path; };
    collect(project);
    for (const auto* history : {&undo_, &redo_}) for (const auto& p : *history) collect(p);
    return paths;
}
bool Controller::audioDiffersFrom(AudioPreparation preparation) const {
    // Direct WAVs read their source in both modes; generated audio has no storage.
    const auto differs = [preparation](const Project& p) {
        return std::any_of(p.assets.cbegin(), p.assets.cend(), [preparation](const MediaAsset& a) {
            const auto* storage = a.audio ? a.audio->storage.get() : nullptr;
            return storage && (preparation == AudioPreparation::Prepared ? storage->decodes() : !storage->readsSource());
        });
    };
    return differs(project) || std::any_of(undo_.cbegin(), undo_.cend(), differs) || std::any_of(redo_.cbegin(), redo_.cend(), differs);
}
Controller::AudioSwap Controller::replaceAudio(const QMap<QString, std::shared_ptr<const AudioData>>& audio) {
    if (!canEdit()) return AudioSwap::Rejected;
    // Matching by path keeps a relinked asset (same id, other file) on its own source.
    const auto swap = [&](Project& p) {
        for (auto& a : p.assets) if (a.audio && a.audio->storage && audio.contains(a.path)) {
            a.audio = audio.value(a.path);
            if (!a.video && !a.image) a.duration = seconds(a.audio->length()); // The file may have changed since import; new clips use it.
        }
        bindAudio(p);
    };
    Project next = project;
    try { swap(next); validate(next); }
    catch (const std::exception& e) { report(QString::fromUtf8(e.what())); return AudioSwap::Rejected; }
    // A history snapshot the new audio would invalidate (an offset beyond a shorter replacement)
    // keeps its previous audio rather than becoming a state undo cannot arm.
    for (auto* history : {&undo_, &redo_}) for (auto& p : *history) {
        Project candidate = p;
        try { swap(candidate); validate(candidate); p = std::move(candidate); } catch (const std::exception&) {}
    }
    // Committed: a failure to re-arm leaves the transport disarmed (Play retries) and the new audio in place.
    project = std::move(next);
    const auto outcome = arm(std::min(position(), project.show.duration)) ? AudioSwap::Applied : AudioSwap::ArmFailed;
    emit changed(); return outcome;
}
QStringList Controller::preflight() {
    QStringList problems;
    if (dirty) problems << "The show has unsaved changes.";
    // Picture clips carry their video's audio but never play it; only audio tracks stream.
    QStringList streaming;
    for (const auto& clip : project.show.clips) {
        if (project.show.tracks[clip.track].visual || !clip.audio || !clip.audio->storage || !clip.audio->storage->decodes()) continue;
        const auto asset = project.assets.constFind(QString::fromStdString(clip.assetId));
        const auto name = asset == project.assets.cend() ? QString("audio") : asset->name;
        if (!streaming.contains(name)) streaming << name;
    }
    // With Prepared chosen, a streaming source is one that could not be prepared: say which, and how to retry.
    if (!streaming.isEmpty())
        problems << (audioPreparation() == AudioPreparation::Prepared
            ? "Audio streams from the original media files for " + streaming.join(", ") + ": preparing them failed. Fix the cause, then choose Prepared audio sources in Outputs again to retry; they keep streaming meanwhile."
            : QString("Audio streams from the original media files. For the steadiest playback, choose Prepared audio sources in Outputs."));
    for (const auto& missing : missingMedia()) problems << "Media is missing: " + missing;
    if (needsMidi() && !midi.connected()) {
        const QString remembered = QSettings().value("midi/port").toString();
        if (remembered.isEmpty()) problems << "MIDI cues need a destination. Choose one in Outputs.";
        else if (!midi.ports().contains(remembered)) problems << "MIDI destination “" + remembered + "” is missing.";
    }
    // Probe audio drivers only while nothing is playing.
    if (!device.running() && transport.state() != State::Playing && transport.state() != State::Held && !transport.asyncVoices()) {
        QSettings settings; const QString remembered = settings.value("audio/device").toString();
        const auto api = remembered.isEmpty() ? RtAudio::UNSPECIFIED : static_cast<RtAudio::Api>(settings.value("audio/api", 0).toInt());
        try {
            if (!playbackDevice(device.devices(api), remembered))
                problems << (remembered.isEmpty() ? "No audio output is available." : "Audio output “" + remembered + "” is missing.");
        } catch (const std::exception& e) { problems << "Cannot check audio outputs: " + QString::fromUtf8(e.what()); }
    } else if (device.running() && !routesValid()) problems << "A track is routed to more outputs than the audio interface provides.";
    return problems;
}
bool Controller::needsRelink(const MediaAsset& a) {
    const bool readsSource = a.audio && a.audio->storage && a.audio->storage->readsSource();
    return a.offline || ((a.video || readsSource) && !a.path.isEmpty() && !detail::present(a.path));
}
void Controller::mediaToCheck(QStringList& offline, QStringList& needed) const {
    QSet<QString> seen, wanted; // Large shows reuse files across many clips: listed once each, in show order.
    for (const auto& clip : project.show.clips) {
        const auto asset = project.assets.constFind(QString::fromStdString(clip.assetId));
        if (asset == project.assets.cend()) continue;
        if (asset->offline) { if (!seen.contains(asset->path)) { seen.insert(asset->path); offline << asset->path; } continue; }
        // Picture clips need their video; audio clips need the file only while they play from it rather than
        // from a prepared cache (a video's soundtrack on an audio track included).
        const bool visual = project.show.tracks[clip.track].visual;
        const bool needs = visual ? static_cast<bool>(asset->video) : asset->audio && asset->audio->storage && asset->audio->storage->readsSource();
        if (needs && !wanted.contains(asset->path)) { wanted.insert(asset->path); needed << asset->path; }
    }
    if (!seen.isEmpty()) needed.removeIf([&](const QString& path) { return seen.contains(path); });
}
QStringList Controller::missingMedia() const {
    QStringList missing, needed; mediaToCheck(missing, needed); // Each file is looked up once, however many clips use it.
    for (const auto& path : needed) if (!detail::present(path)) missing << path;
    return missing;
}
QString Controller::mediaWarning(const QStringList& missing) {
    return QString("%1 media file%2 missing: %3. Relink it from File, or remove its clips.").arg(missing.size()).arg(missing.size() == 1 ? " is" : "s are", missing.join(", "));
}
QString Controller::missingMediaWarning() const { return missingWarning_; }
void Controller::resetMediaState() {
    // A finished check's absences are kept for the open readers before its result is dropped.
    { const std::lock_guard lock(mediaCheck_->mutex); for (const auto& path : mediaCheck_->missing) missingSeen_.insert(path); }
    mediaCheck_ = std::make_shared<MediaCheck>(); // A check still running finishes into the old state, unread.
    if (!reportedProblem_.isEmpty() && playbackProblem == reportedProblem_) playbackProblem.clear();
    if (!reportedProblem_.isEmpty() && status == reportedProblem_) report(QString()); // Not left on screen either.
    if (!missingNow_.isEmpty()) lastComposed_ = -1; // A picture composed around missing files is not reused.
    missingNow_.clear(); missingWarning_.clear(); reportedProblem_.clear(); wasHeld_ = false; wasLive_ = false;
}
void Controller::promoteVideoWarning() {
    // Another picture still frozen keeps the banner's warning once the one shown is resolved.
    if (!playbackProblem.isEmpty()) return;
    for (const auto& [clip, problem] : videoProblems_) if (!recovering_.contains(clip)) { playbackProblem = problem; return; } // Not one reopening.
}
void Controller::closeVideo(std::unique_ptr<VideoReader> reader) {
    if (!reader) return;
    auto holder = std::make_shared<std::unique_ptr<VideoReader>>(std::move(reader));
    try { std::thread([holder] { holder->reset(); }).detach(); } catch (...) {} // Otherwise it closes here.
}
void Controller::retireVideo(const std::string& clip) {
    videoErrors_.erase(clip); recovering_.insert(clip); // Its last frame stands in until the new reader has one.
    const auto it = videos_.find(clip); if (it == videos_.end()) return;
    // Its worker may still be blocked reading a share; joining it here would freeze the controls.
    closeVideo(std::move(it->second)); videos_.erase(it);
}
QStringList Controller::lastMissingMedia() const { QStringList offline, needed; mediaToCheck(offline, needed); return lastMissingMedia(offline, needed); }
QStringList Controller::lastMissingMedia(const QStringList& offline, const QStringList& needed) const {
    QSet<QString> found; { const std::lock_guard lock(mediaCheck_->mutex); found = QSet<QString>(mediaCheck_->missing.cbegin(), mediaCheck_->missing.cend()); }
    // A check from before an edit or relink can name files the show no longer uses.
    QStringList missing = offline;
    for (const auto& path : needed) if (found.contains(path)) missing << path;
    return missing;
}
QStringList Controller::lastMissingMediaAndCheck(const QStringList& offline, const QStringList& needed) {
    const auto check = mediaCheck_; QSet<QString> found; bool start = false;
    {
        const std::lock_guard lock(check->mutex);
        found = QSet<QString>(check->missing.cbegin(), check->missing.cend());
        if (!check->running) start = check->running = true;
    }
    QStringList missing = offline;
    for (const auto& path : needed) if (found.contains(path)) missing << path;
    if (start) startMediaCheck(check, needed, false);
    return missing;
}
void Controller::checkMediaInBackground(bool wait) {
    QStringList offline, needed; mediaToCheck(offline, needed);
    const auto check = mediaCheck_;
    for (;;) {
        { const std::lock_guard lock(check->mutex); if (!check->running) { check->running = true; break; } }
        if (!wait) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    startMediaCheck(check, needed, wait);
}
void Controller::startMediaCheck(const std::shared_ptr<MediaCheck>& check, const QStringList& needed, bool wait) {
    // Lookups can stall on a share, so they run on a thread of their own, never the show's controls.
    auto run = [check, needed] {
        QStringList missing; QSet<QString> before;
        { const std::lock_guard lock(check->mutex); before = QSet<QString>(check->missing.cbegin(), check->missing.cend()); }
        // A lookup that cannot tell (a share dropping out) keeps what the last one found.
        try {
            for (const auto& path : needed) {
                const auto here = detail::presentIfKnown(path);
                if (here ? !*here : before.contains(path)) missing << path;
            }
        } catch (...) { missing = QStringList(before.cbegin(), before.cend()); }
        const std::lock_guard lock(check->mutex); check->missing = missing; check->running = false;
    };
    if (!wait) try { std::thread(run).detach(); return; } catch (...) { const std::lock_guard lock(check->mutex); check->running = false; return; }
    run();
}
void Controller::watchMidi() {
    if (!needsMidi() || midiName_.isEmpty() || busy) return;
    const bool present = midi.ports().contains(midiName_);
    if (midi.connected() && !present) {
        stopMidiThread(); midi.close(); startMidiThread();
        report("MIDI destination “" + midiName_ + "” disconnected. Cues are skipped until it reconnects."); playbackProblem = midiProblem_ = status;
    } else if (!midi.connected() && present && transport.state() != State::Stopped) {
        if (prepareMidi()) report("MIDI destination “" + midiName_ + "” reconnected.");
    }
}
bool Controller::needsMidi() const {
    return std::any_of(project.show.markers.begin(), project.show.markers.end(), [](const Marker& marker) { return !marker.message.empty(); });
}
bool Controller::prepareMidi() {
    if (midi.connected()) return true;
    // Automatic preparation is allowed in Show mode and while voices are live: queued cues wait in the transport.
    if (busy) { report("Wait for media preparation to finish."); return false; }
    const QString remembered = QSettings().value("midi/port").toString();
    midiName_ = remembered;
    if (remembered.isEmpty()) {
        report("Choose a MIDI destination in Outputs once. It will reconnect automatically when you press Play."); return false;
    }
    const auto ports = midi.ports(); const int port = ports.indexOf(remembered);
    if (port < 0) { report("MIDI destination “" + remembered + "” is missing. Plug it in, or choose another destination in Outputs."); return false; }
    return openMidi(port);
}
bool Controller::prepareAudio() {
    if (device.running()) return true;
    if (busy || transport.state() == State::Playing || transport.state() == State::Held || transport.asyncVoices()) {
        report("Use Stop All before reconnecting audio in Outputs."); return false;
    }
    QSettings settings;
    const QString remembered = settings.value("audio/device").toString();
    const auto api = remembered.isEmpty() ? RtAudio::UNSPECIFIED : static_cast<RtAudio::Api>(settings.value("audio/api", 0).toInt());
    try {
        const auto selected = playbackDevice(device.devices(api), remembered);
        if (!selected) {
            report(remembered.isEmpty() ? "No audio output found. Plug in speakers or an interface, then press Play again." :
                "Audio output “" + remembered + "” is missing. Plug it in, or choose another device in Outputs."); return false;
        }
        unsigned required = 1;
        for (const auto& track : project.show.tracks) if (!track.visual && !track.muted)
            required = std::max(required, static_cast<unsigned>(std::max(track.left, track.right) + 1));
        if (required > selected->channels) {
            report(QString("%1 has %2 outputs, but a track is routed to output %3. Choose a larger interface in Outputs or change track routing.")
                .arg(selected->name).arg(selected->channels).arg(required)); return false;
        }
        const unsigned channels = std::min(selected->channels, std::max(required, settings.value("audio/channels", 2).toUInt()));
        const unsigned buffer = settings.value("audio/buffer", 512).toUInt();
        return openAudio(api, selected->id, selected->name, channels, buffer);
    } catch (const std::exception& error) { report("Cannot prepare audio. Open Outputs to choose a device. " + QString::fromUtf8(error.what())); return false; }
}
bool Controller::testMidi(const std::vector<unsigned char>& bytes) {
    if (!canEdit() || bytes.empty()) return false;
    if (!validMidi(bytes)) { report("Complete the MIDI message before testing it."); return false; }
    if (!prepareMidi()) return false;
    QString error;
    stopMidiThread();
    const bool sent = midi.send(bytes, error);
    startMidiThread();
    if (!sent) { report(error); return false; }
    report("Test message sent to " + midi.name() + ". External devices may keep playing until they receive a stop message."); return true;
}
const Marker* Controller::nextCue() const {
    const Frame now = position(); const bool held = transport.state() == State::Held; const Marker* upcoming = nullptr;
    for (const auto& m : project.show.markers)
        if ((m.time > now || (m.time == now && !held)) && (!upcoming || m.time < upcoming->time)) upcoming = &m;
    return upcoming;
}
void Controller::setShowMode(bool enabled) { showMode = enabled; emit changed(); }
QImage Controller::compose() {
    const Frame position = this->position();
    // Pre-roll the next clip on each layer. Keep readers only for active/upcoming clips.
    std::set<std::string> needed;
    for (std::size_t track = 0; track < project.show.tracks.size(); ++track) {
        if (!project.show.tracks[track].visual || project.show.tracks[track].muted) continue;
        const Clip* upcoming = nullptr;
        for (const auto& clip : project.show.clips) {
            if (clip.track != static_cast<int>(track)) continue;
            const auto asset = project.assets.constFind(QString::fromStdString(clip.assetId));
            if (asset == project.assets.cend() || !asset->video) continue;
            if (clip.contains(position)) needed.insert(clip.id);
            else if (clip.start > position && clip.start <= position + frames(1) && (!upcoming || clip.start < upcoming->start)) upcoming = &clip;
        }
        if (upcoming) needed.insert(upcoming->id);
    }
    for (auto it = videos_.begin(); it != videos_.end();) {
        if (!needed.contains(it->first)) { videoErrors_.erase(it->first); closeVideo(std::move(it->second)); it = videos_.erase(it); }
        else ++it;
    }
    // A clip leaving the picture takes its video warning with it, unless a newer problem replaced it.
    bool cleared = false;
    for (auto it = videoProblems_.begin(); it != videoProblems_.end();) {
        if (needed.contains(it->first)) { ++it; continue; }
        if (playbackProblem == it->second) { playbackProblem.clear(); cleared = true; }
        if (status == it->second) report(QString()); // Not left in the status bar either.
        it = videoProblems_.erase(it);
    }
    if (cleared) promoteVideoWarning();
    std::erase_if(clipFrames_, [&](const auto& entry) { return !needed.contains(entry.first); });
    std::erase_if(recovering_, [&](const std::string& clip) { return !needed.contains(clip); });
    for (const auto& clip : project.show.clips) {
        if (!needed.contains(clip.id)) continue;
        const auto asset = project.assets.constFind(QString::fromStdString(clip.assetId));
        if (asset != project.assets.cend() && asset->video && !videos_.contains(clip.id)) {
            videos_[clip.id] = std::make_unique<VideoReader>(asset->path, QSize(project.width, project.height));
            videos_[clip.id]->imageAt(seconds(clip.offset));
        }
    }
    // Async videos keep their own clock through pauses and holds; their pictures change while show time stands still.
    std::vector<Frame> clocks;
    for (std::size_t i = 0; i < project.show.clips.size(); ++i) {
        const auto& clip = project.show.clips[i]; const auto& track = project.show.tracks[clip.track];
        if (!track.visual || !track.async || track.muted || !clip.contains(position)) continue;
        const Frame source = transport.asyncSource(i);
        clocks.push_back(source < 0 ? source : static_cast<Frame>(std::floor(seconds(source) * project.fps)));
    }
    const Frame frame = static_cast<Frame>(std::floor(seconds(position) * project.fps));
    // A reopened video that is drawn is polled until it has a picture, even while show time stands still (a hold).
    // recoveringDrawn_: the last pass polled a recovering clip's reader (it is visible, not skipped).
    // A reader that failed since the last pass (decoding ahead during a hold) is reported, not hidden by the cache.
    const bool newError = std::any_of(videos_.begin(), videos_.end(), [&](const auto& entry) { return !videoErrors_.contains(entry.first) && !entry.second->error().isEmpty(); });
    if (frame == lastComposed_ && clocks == lastClocks_ && !lastPicture_.isNull() && !recoveringDrawn_ && !newError) return lastPicture_;
    recoveringDrawn_ = false;
    QImage picture(project.width, project.height, QImage::Format_RGB32); picture.fill(Qt::black);
    QPainter painter(&picture); painter.setRenderHint(QPainter::SmoothPixmapTransform); painter.setRenderHint(QPainter::TextAntialiasing);
    bool waiting = false;
    // Track 0 is the background; successive visual tracks are composited over it.
    for (std::size_t track = 0; track < project.show.tracks.size(); ++track) {
        if (!project.show.tracks[track].visual || project.show.tracks[track].muted) continue;
        for (std::size_t index = 0; index < project.show.clips.size(); ++index) {
            const auto& c = project.show.clips[index];
            if (c.track != static_cast<int>(track) || !c.contains(position)) continue;
            const Frame local = position - c.start;
            const double opacity = c.alpha.at(local, c.opacity);
            if (opacity <= 0) continue;
            painter.save(); painter.setOpacity(opacity);
            painter.translate(project.width * (.5 + c.positionX.at(local, c.x)), project.height * (.5 + c.positionY.at(local, c.y)));
            const double scale = c.size.at(local, c.scale); painter.scale(scale, scale);
            const QRectF bounds(-project.width * .5, -project.height * .5, project.width, project.height);
            if (!c.text.empty()) {
                QFont font("Arial"); font.setPixelSize(project.height / 22); font.setWeight(QFont::Medium);
                painter.setFont(font); painter.setPen(Qt::white);
                painter.drawText(bounds.adjusted(project.width * .08, project.height * .08, -project.width * .08, -project.height * .08),
                    Qt::AlignCenter | Qt::TextWordWrap, QString::fromStdString(c.text));
            } else {
                const auto& a = project.assets[QString::fromStdString(c.assetId)];
                QImage source = a.still;
                if (a.video) {
                    double time = seconds(c.offset + local);
                    if (c.loop) time = seconds(c.offset) + std::fmod(seconds(local), std::max(.001, a.duration - seconds(c.offset)));
                    if (project.show.tracks[track].async) {
                        // A finished one-shot keeps its last frame until show time leaves the clip.
                        const Frame source = transport.asyncSource(index);
                        if (source == Transport::finishedSource) time = a.duration;
                        else if (source >= 0) time = seconds(source);
                    }
                    auto it = videos_.find(c.id);
                    QString error; // One reading: the reader's worker can fail at any moment.
                    if (it != videos_.end()) {
                        it->second->setLoop(c.loop ? seconds(c.offset) : -1, a.duration);
                        source = it->second->imageAt(std::min(time, std::max(0., a.duration - 1. / a.fps)));
                        // A failed reader freezes its picture; audio and cues continue.
                        error = it->second->error();
                        if (!error.isEmpty() && !videoErrors_.contains(c.id)) {
                            videoErrors_.insert(c.id);
                            report("Video error in " + a.name + ": " + error + videoFallback(!source.isNull() || clipFrames_.contains(c.id)));
                            playbackProblem = videoProblems_[c.id] = status;
                        }
                        if (!error.isEmpty()) recovering_.erase(c.id); // Failed again: no longer polled every frame.
                    }
                    if (!source.isNull()) {
                        clipFrames_[c.id] = source;
                        // A reopened reader that delivers a picture resolves its own warning, not a newer problem.
                        if (recovering_.erase(c.id) && error.isEmpty()) {
                            const auto problem = videoProblems_.find(c.id);
                            if (problem != videoProblems_.end()) {
                                const bool shown = playbackProblem == problem->second;
                                if (shown) playbackProblem.clear();
                                if (status == problem->second) report("Picture restored: " + a.name + ".");
                                videoProblems_.erase(problem);
                                if (shown) promoteVideoWarning();
                            }
                        }
                    } else if (recovering_.contains(c.id) || !error.isEmpty() || missingNow_.contains(a.path)) {
                        // A reopened reader starting, one that failed without a picture, or a file known to be
                        // missing (its read may be stalled): this layer keeps its last frame (or none) and the
                        // others go on.
                        recoveringDrawn_ |= recovering_.contains(c.id);
                        if (const auto held = clipFrames_.find(c.id); held != clipFrames_.end()) source = held->second;
                    } else waiting = true;
                }
                if (!source.isNull()) {
                    const QSizeF fitted = QSizeF(source.size()).scaled(bounds.size(), Qt::KeepAspectRatio);
                    painter.drawImage(QRectF(-fitted.width() / 2, -fitted.height() / 2, fitted.width(), fitted.height()), source);
                }
            }
            painter.restore();
        }
    }
    painter.end();
    // Readers not drawn this pass (upcoming, or a layer at zero opacity) report their failures too, once.
    for (const auto& [clip, reader] : videos_) {
        if (videoErrors_.contains(clip)) continue;
        const QString error = reader->error(); if (error.isEmpty()) continue;
        const auto found = std::find_if(project.show.clips.begin(), project.show.clips.end(), [&](const Clip& c) { return c.id == clip; });
        const auto asset = found == project.show.clips.end() ? project.assets.cend() : project.assets.constFind(QString::fromStdString(found->assetId));
        videoErrors_.insert(clip); recovering_.erase(clip);
        report("Video error in " + (asset == project.assets.cend() ? QString("a video") : asset->name) + ": " + error + videoFallback(clipFrames_.contains(clip)));
        playbackProblem = videoProblems_[clip] = status;
    }
    if (waiting) {
        ++lateFrames; lastComposed_ = -1; // An unfinished picture is never reused for this frame.
        if (!lastPicture_.isNull()) return lastPicture_;
        return picture;
    }
    lastComposed_ = frame; lastClocks_ = std::move(clocks); lastPicture_ = picture; return picture;
}
QString Controller::mediaDirectory(const QString& target) {
    return QFileInfo(target).absolutePath() + "/" + QFileInfo(target).completeBaseName() + ".media";
}
bool Controller::savingCopiesMedia() const {
    return project.packageDirectory || std::any_of(project.assets.cbegin(), project.assets.cend(), [](const MediaAsset& a) { return a.path.isEmpty(); });
}
void Controller::save(QString target) {
    if (!canEdit()) return;
    try { Project candidate = project; materializeProjectMedia(candidate, mediaDirectory(target)); commitSave(std::move(candidate), target); }
    catch (const std::exception& e) { report("Save failed: " + QString::fromUtf8(e.what())); }
}
void Controller::commitSave(Project candidate, QString target) {
    try {
        const auto sourceDirectory = project.packageDirectory;
        const QString previous = path;
        writeProject(candidate, target);
        // The media this Save moved (extracted package files, generated audio), by asset and previous path.
        QMap<QString, QString> moved;
        for (const auto& a : candidate.assets)
            if (const auto before = project.assets.constFind(a.id); before != project.assets.cend() && before->path != a.path) moved.insert(a.id, before->path);
        project.assets = std::move(candidate.assets); bindAudio(project); project.packageDirectory.reset(); path = target; dirty = false;
        // Undo must not restore temporary package paths or unsaved generated media. Only those follow the
        // saved copy: other snapshots keep their own media, such as an asset before a relink, or audio a
        // preparation reload left valid for that snapshot alone.
        for (auto* history : {&undo_, &redo_}) for (auto& snapshot : *history) {
            Project candidate = snapshot; bool extracted = false;
            for (auto it = candidate.assets.begin(); it != candidate.assets.end(); ++it) {
                if (const auto from = moved.constFind(it.key()); from != moved.cend() && *from == it->path) {
                    const auto& saved = project.assets[it.key()]; it->path = saved.path; it->audio = saved.audio;
                }
                // Each snapshot keeps its own extraction alive while any of its media still lives there.
                else extracted |= candidate.packageDirectory && it->path.startsWith(candidate.packageDirectory->path() + "/");
            }
            if (!extracted) candidate.packageDirectory.reset();
            bindAudio(candidate);
            // A snapshot the saved media would invalidate (audio reloaded shorter since) keeps its own media
            // and extraction instead of becoming a state undo cannot arm.
            try { validate(candidate); snapshot = std::move(candidate); } catch (const std::exception&) {}
        }
        if (sourceDirectory) arm(position());
        // The edits now live in target; a sidecar left next to the previous file would offer them again.
        for (const auto& saved : {target, previous}) if (!saved.isEmpty()) QFile::remove(saved + ".autosave");
        QFile::remove(untitledAutosavePath());
        report("Saved " + QFileInfo(target).fileName()); emit changed();
    } catch (const std::exception& e) { report("Save failed: " + QString::fromUtf8(e.what())); }
}

}
