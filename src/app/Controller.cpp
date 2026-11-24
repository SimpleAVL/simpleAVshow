#include "app/Controller.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMetaObject>
#include <QPainter>
#include <QStandardPaths>
#include <chrono>
#include <set>

namespace sav {
Controller::Controller(QObject* parent) : QObject(parent), project(newProject()), device(transport) {
    midiName_ = QSettings().value("midi/port").toString();
    arm(0);
    timer_.setInterval(16);
    connect(&timer_, &QTimer::timeout, this, [this] {
        // A stream that stops calling back without reporting an error has stalled just the same.
        if (device.callbacks() != heartbeat_ || !device.running()) { heartbeat_ = device.callbacks(); stall_.restart(); }
        const bool stalled = configured_ && device.running() && stall_.hasExpired(750);
        if (configured_ && (!device.running() || stalled)) {
            const QString reason = stalled ? QString("stopped responding") : device.lastError().isEmpty() ? QString("stopped") : "failed: " + device.lastError();
            configured_ = false; device.close();
            // Once the device is closed, force-stop directly so even a full command queue cannot leave Playing behind.
            transport.forceStopAll();
            commandsPosted_ = 0; lastPlaySeq_ = 0; predictedState_ = State::Stopped; predictedPosition_ = position();
            report("Audio output " + reason + ". Press Play to reconnect, or choose another device in Outputs.");
            playbackProblem = status;
        }
        if (transport.eventOverflow() && transport.state() != State::Stopped) {
            stopAll(); report("MIDI event queue overflow. Playback is stopped; inspect the project.");
        }
        if (transport.audioReadFailed() && transport.state() != State::Stopped) {
            stopAll(); report("Audio source changed or could not be read. Playback is stopped; restore missing media and press Play to retry, or reopen the project to rebuild changed caches."); playbackProblem = status;
        }
        if (midiWatch_.hasExpired(1000)) { midiWatch_.restart(); watchMidi(); }
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
quint64 Controller::acquireBusy() {
    if (busy) return 0;
    busy = true;
    busyLease_ = nextBusyLease_++;
    return busyLease_;
}
void Controller::releaseBusy(quint64 lease) {
    if (lease != 0 && lease == busyLease_) {
        busyLease_ = 0;
        busy = false;
    }
}
bool Controller::postTransportCommand(Command cmd) {
    const State base = effectiveState();
    const Frame basePos = effectivePosition();
    if (!transport.command(cmd)) { report("Transport command queue is full"); return false; }
    ++commandsPosted_;
    predictedPosition_ = basePos;
    switch (cmd.action) {
    case Action::Play:
        predictedState_ = (basePos >= project.show.duration && base != State::Held) ? base : State::Playing;
        lastPlaySeq_ = commandsPosted_;
        break;
    case Action::Pause:
        predictedState_ = (base == State::Playing) ? State::Paused : base;
        break;
    case Action::Seek:
        predictedPosition_ = std::clamp(cmd.position, Frame{0}, project.show.duration);
        predictedState_ = (base == State::Playing) ? State::Playing : State::Paused;
        break;
    case Action::StopAll:
        predictedState_ = State::Stopped;
        break;
    }
    return true;
}
State Controller::effectiveState() const {
    const std::uint64_t applied = transport.commandsApplied();
    if (commandsPosted_ <= applied) return transport.state();
    return predictedState_;
}
Frame Controller::effectivePosition() const {
    const std::uint64_t applied = transport.commandsApplied();
    if (commandsPosted_ <= applied) return position();
    return predictedPosition_;
}
bool Controller::canEdit(bool reportFailure) {
    // Acquire commandsApplied() first so publishedState_ and publishedVoices_ reflect every applied command.
    const bool playPending = playInFlight();
    const bool allowed = !showMode && !busy && !playPending && !transport.stopAllPending() &&
        transport.state() != State::Playing && transport.state() != State::Held &&
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
        project = std::move(next); dirty = true; ++projectRevision_; arm(time); emit changed(); return true;
    } catch (const std::exception& e) { report(QString::fromUtf8(e.what())); return false; }
}
void Controller::replace(Project next, QString projectPath, bool markDirty) {
    validate(next);
    // Readers hold extracted package files open; Windows cannot remove them while open.
    videos_.clear(); lastPicture_ = {};
    project = std::move(next); path = std::move(projectPath); dirty = markDirty; playbackProblem.clear();
    undo_.clear(); redo_.clear(); ++projectRevision_; ++showEpoch_;
    arm(0); report("Project ready. Space plays or pauses; Stop All stops async audio and video."); emit changed();
}
void Controller::undo() {
    if (!canEdit() || undo_.empty()) return;
    redo_.push_back(std::move(project)); project = std::move(undo_.back()); undo_.pop_back(); dirty = true; ++projectRevision_;
    arm(std::min(position(), project.show.duration)); emit changed();
}
void Controller::redo() {
    if (!canEdit() || redo_.empty()) return;
    undo_.push_back(std::move(project)); project = std::move(redo_.back()); redo_.pop_back(); dirty = true; ++projectRevision_;
    arm(std::min(position(), project.show.duration)); emit changed();
}
void Controller::arm(Frame position) {
    stopMidiThread();
    // The callback must be stopped before configure; a healthy stream only pauses.
    const bool keep = configured_ && device.running();
    if (keep) device.pause(); else device.close();
    videos_.clear(); videoErrors_.clear(); lastPicture_ = {}; lastComposed_ = -1; lateFrames = 0;
    auto armed = std::make_shared<Show>(project.show);
    for (auto& clip : armed->clips) {
        if (!armed->tracks[clip.track].visual) continue;
        const auto asset = project.assets.constFind(QString::fromStdString(clip.assetId));
        if (asset != project.assets.cend() && asset->video && !asset->offline) clip.videoLength = frames(asset->duration);
    }
    armed_ = std::move(armed); transport.configure(armed_, position);
    commandsPosted_ = 0; lastPlaySeq_ = 0; predictedState_ = State::Stopped; predictedPosition_ = position;
    if (keep && device.resume()) stall_.restart();
    else if (configured_) {
        QString error;
        bool reopened = false;
        if (!audioName.isEmpty()) {
            QString resolvedName;
            reopened = device.openByName(api_, audioName, audioName, channels_, buffer_, resolvedName, error, &deviceId_);
            if (reopened) audioName = resolvedName;
        } else {
            reopened = device.open(api_, deviceId_, channels_, buffer_, error);
        }
        if (!reopened) { configured_ = false; report("Audio output unavailable: " + error); }
        stall_.restart();
    }
    startMidiThread();
}
void Controller::startMidiThread() {
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
            for (;;) {
                if (midiStop_.load()) break;
                PendingTestMidi item;
                {
                    std::lock_guard lock(testMidiMutex_);
                    if (testMidiQueue_.empty()) break;
                    item = std::move(testMidiQueue_.front());
                    testMidiQueue_.pop_front();
                }
                // Drop stale test messages if Show mode, Stop All, Play, shutdown, or bearer revocation intervened.
                if (midiStop_.load() ||
                    showModeAtomic_.load(std::memory_order_acquire) ||
                    (item.ownerCancelled && item.ownerCancelled->load(std::memory_order_acquire)) ||
                    item.epoch != transport.epoch() ||
                    item.gen != testMidiGen_.load(std::memory_order_acquire) ||
                    transport.state() == State::Playing || transport.state() == State::Held)
                    continue;
                QString error;
                if (!midi.send(item.bytes, error)) {
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
bool Controller::play(bool fromAgent) {
    if (busy) return false;
    cancelPendingTestMidi();
    const State state = transport.state();
    if (state != State::Playing) {
        playbackProblem.clear();
        // Re-arming retries sources that were briefly unavailable; changed sources still need reopening.
        if (transport.audioReadFailed() && state != State::Held && !transport.asyncVoices()) arm(position());
        if (transport.audioReadFailed()) { report("Audio source is unreadable. Restore missing media and press Play to retry, or reopen the project to rebuild changed or damaged caches."); playbackProblem = status; return false; }
        // Resuming a live show never blocks on picture or MIDI; it warns and continues.
        const bool resuming = state == State::Held || transport.asyncVoices() || (state == State::Paused && device.running());
        if (const auto missing = missingMedia(); !missing.isEmpty()) {
            report(QString("%1 media file%2 missing: %3. Relink it from File, or remove its clips.").arg(missing.size()).arg(missing.size() == 1 ? " is" : "s are", missing.join(", ")));
            playbackProblem = status;
            if (!resuming) return false;
        }
        if (needsMidi() && !prepareMidi()) { playbackProblem = status; if (!resuming) return false; }
        if (!prepareAudio()) { playbackProblem = status; return false; }
        if (!routesValid()) { report("A track needs more output channels. Open Outputs to increase channels, or change the track's routing."); playbackProblem = status; return false; }
    }
    // Always enqueue Action::Play so a Play queued immediately after a Pause is honored in arrival order.
    return postTransportCommand({Action::Play, 0, fromAgent ? transport.agentEpoch() : 0});
}
bool Controller::pause(bool fromAgent) {
    if (busy) return false;
    // Always enqueue Action::Pause (never Play) so a Pause queued behind Play runs in order and
    // a Playing → Held transition can never accidentally resume a hold.
    if (!postTransportCommand({Action::Pause, 0, fromAgent ? transport.agentEpoch() : 0})) return false;
    if (!device.streamActive()) transport.render(nullptr, 0, 0);
    return true;
}
bool Controller::toggle() {
    if (busy) return false;
    return effectiveState() == State::Playing ? pause() : play();
}
bool Controller::seek(Frame time, bool fromAgent) {
    if (busy || showMode) { report(showMode ? "Leave show mode to jump on the timeline" : "Media is loading"); return false; }
    if (!postTransportCommand({Action::Seek, time, fromAgent ? transport.agentEpoch() : 0})) return false;
    if (!device.streamActive()) transport.render(nullptr, 0, 0);
    lastComposed_ = -1;
    return true;
}
bool Controller::stopAll(bool fromAgent) {
    cancelPendingTestMidi();
    const Command cmd{Action::StopAll, 0, fromAgent ? transport.agentEpoch() : 0};
    if (!postTransportCommand(cmd)) {
        if (fromAgent) return false;
        // Emergency operator Stop All must never be rejected by a full bounded command queue.
        transport.requestStopAll();
        predictedState_ = State::Stopped;
        lastPlaySeq_ = 0;
    }
    if (!device.streamActive()) transport.render(nullptr, 0, 0);
    return true;
}
bool Controller::configureAudio(RtAudio::Api api, unsigned id, QString name, unsigned channels, unsigned buffer) {
    if (!canEdit()) return false;
    return openAudio(api, id, std::move(name), channels, buffer);
}
bool Controller::configureAudioByName(RtAudio::Api api, const QString& wantedName, unsigned channels, unsigned buffer) {
    if (!canEdit()) return false;
    const bool hadStream = configured_ && device.running();
    const RtAudio::Api prevApi = api_;
    const QString prevName = audioName;
    const unsigned prevChannels = channels_, prevBuffer = buffer_;
    QSettings settings;
    QString resolvedName, error;
    unsigned resolvedId = deviceId_;
    if (device.openByName(api, wantedName, settings.value("audio/device").toString(), channels, buffer, resolvedName, error, &resolvedId)) {
        api_ = api; deviceId_ = resolvedId; channels_ = channels; buffer_ = buffer; audioName = resolvedName; configured_ = true;
        stall_.restart(); playbackProblem.clear();
        settings.setValue("audio/api", static_cast<int>(api)); settings.setValue("audio/device", resolvedName);
        settings.setValue("audio/channels", channels); settings.setValue("audio/buffer", buffer);
        report(QString("%1 · %2 channels · 48 kHz · %3 samples").arg(resolvedName).arg(channels).arg(device.bufferFrames()));
        return true;
    }
    configured_ = false;
    if (hadStream && !prevName.isEmpty()) {
        QString restoredName, restoreErr;
        unsigned restoredId = deviceId_;
        if (device.openByName(prevApi, prevName, prevName, prevChannels, prevBuffer, restoredName, restoreErr, &restoredId)) {
            deviceId_ = restoredId;
            configured_ = true; stall_.restart();
            report("Audio output unavailable: " + error + " (kept previous connection: " + restoredName + ")");
        } else {
            report("Audio output unavailable: " + error + " (previous audio connection “" + prevName + "” was closed and could not be restored: " + restoreErr + ")");
        }
    } else {
        report("Audio output unavailable: " + error);
    }
    return false;
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
bool Controller::configureMidiByName(const QString& portName) {
    if (!canEdit()) return false;
    const QString trimmed = portName.trimmed();
    if (trimmed.isEmpty() || trimmed.compare("none", Qt::CaseInsensitive) == 0 || trimmed.compare("off", Qt::CaseInsensitive) == 0)
        return openMidi(-1);
    stopMidiThread();
    cancelPendingTestMidi();
    QString error;
    const bool ok = midi.openByName(trimmed, error);
    startMidiThread();
    if (ok) { midiName_ = midi.name(); QSettings().setValue("midi/port", midiName_); playbackProblem.clear(); }
    report(ok ? ("MIDI output: " + midi.name()) : error);
    return ok;
}
bool Controller::openMidi(int port) {
    stopMidiThread();
    cancelPendingTestMidi();
    QString error; const bool ok = midi.open(port, error); startMidiThread();
    if (ok) { midiName_ = midi.name(); QSettings().setValue("midi/port", midiName_); playbackProblem.clear(); }
    report(ok ? (port < 0 ? "MIDI output disconnected" : "MIDI output: " + midi.name()) : error); return ok;
}
QStringList Controller::preflight(bool probeHardware) {
    QStringList problems;
    if (dirty) problems << "The show has unsaved changes.";
    for (const auto& missing : missingMedia(probeHardware)) problems << "Media is missing: " + missing;
    if (needsMidi() && !midi.connected()) {
        const QString remembered = QSettings().value("midi/port").toString();
        if (remembered.isEmpty()) problems << "MIDI cues need a destination. Choose one in Outputs.";
        else if (!probeHardware) problems << "MIDI destination “" + remembered + "” is not connected.";
        else if (!midi.ports().contains(remembered)) problems << "MIDI destination “" + remembered + "” is missing.";
    }
    // Probe audio drivers only while nothing is playing and hardware probing is permitted.
    if (probeHardware && !device.running() && transport.state() != State::Playing && transport.state() != State::Held && !transport.asyncVoices()) {
        QSettings settings; const QString remembered = settings.value("audio/device").toString();
        const auto api = remembered.isEmpty() ? RtAudio::UNSPECIFIED : static_cast<RtAudio::Api>(settings.value("audio/api", 0).toInt());
        try {
            if (!playbackDevice(device.devices(api), remembered))
                problems << (remembered.isEmpty() ? "No audio output is available." : "Audio output “" + remembered + "” is missing.");
        } catch (const std::exception& e) { problems << "Cannot check audio outputs: " + QString::fromUtf8(e.what()); }
    } else if (device.running() && !routesValid()) problems << "A track is routed to more outputs than the audio interface provides.";
    return problems;
}
QStringList Controller::missingMedia(bool probeFilesystem) const {
    QStringList missing;
    for (const auto& clip : project.show.clips) {
        const auto asset = project.assets.constFind(QString::fromStdString(clip.assetId));
        if (asset == project.assets.cend() || missing.contains(asset->path)) continue;
        if (asset->offline || (probeFilesystem && asset->video && !QFileInfo::exists(asset->path))) missing << asset->path;
    }
    return missing;
}
void Controller::watchMidi() {
    if (!needsMidi() || midiName_.isEmpty() || busy) return;
    const bool present = midi.ports().contains(midiName_);
    if (midi.connected() && !present) {
        stopMidiThread(); midi.close(); startMidiThread();
        report("MIDI destination “" + midiName_ + "” disconnected. Cues are skipped until it reconnects."); playbackProblem = status;
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
    stopMidiThread();
    QString error;
    const bool ok = midi.openByName(remembered, error);
    startMidiThread();
    if (ok) {
        midiName_ = midi.name();
        playbackProblem.clear();
        report("MIDI output: " + midi.name());
        return true;
    }
    report(error.contains("not found", Qt::CaseInsensitive)
        ? ("MIDI destination “" + remembered + "” is missing. Plug it in, or choose another destination in Outputs.")
        : error);
    return false;
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
void Controller::cancelPendingTestMidi(const QString& owner) {
    std::lock_guard lock(testMidiMutex_);
    if (owner.isEmpty()) {
        testMidiGen_.fetch_add(1, std::memory_order_release);
        testMidiQueue_.clear();
    } else {
        std::erase_if(testMidiQueue_, [&](const PendingTestMidi& item) { return item.owner == owner; });
    }
}
bool Controller::testMidi(const std::vector<unsigned char>& bytes, const QString& owner,
                          std::shared_ptr<std::atomic<bool>> ownerCancelled) {
    if (!canEdit() || bytes.empty()) return false;
    if (!validMidi(bytes)) { report("Complete the MIDI message before testing it."); return false; }
    if (!prepareMidi()) return false;
    // Dispatch off the GUI thread so Windows WinMM SysEx completion waits never freeze operator controls.
    {
        std::lock_guard lock(testMidiMutex_);
        if (testMidiQueue_.size() >= 4) {
            report("Too many MIDI test messages are already queued; wait for them to finish.");
            return false;
        }
        testMidiQueue_.push_back({bytes, transport.epoch(), testMidiGen_.load(std::memory_order_acquire), owner, std::move(ownerCancelled)});
    }
    report("Test message queued for " + midi.name() + ". External devices may keep playing until they receive a stop message.");
    return true;
}
const Marker* Controller::nextCue() const {
    const Frame now = position(); const bool held = transport.state() == State::Held; const Marker* upcoming = nullptr;
    for (const auto& m : project.show.markers)
        if ((m.time > now || (m.time == now && !held)) && (!upcoming || m.time < upcoming->time)) upcoming = &m;
    return upcoming;
}
void Controller::setShowMode(bool enabled) {
    showMode = enabled;
    showModeAtomic_.store(enabled, std::memory_order_release);
    if (enabled) {
        transport.invalidateAgentCommands();
        cancelPendingTestMidi();
    }
    emit changed();
}
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
        if (!needed.contains(it->first)) { videoErrors_.erase(it->first); it = videos_.erase(it); }
        else ++it;
    }
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
    if (frame == lastComposed_ && clocks == lastClocks_ && !lastPicture_.isNull()) return lastPicture_;
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
                    if (it != videos_.end()) {
                        it->second->setLoop(c.loop ? seconds(c.offset) : -1, a.duration);
                        source = it->second->imageAt(std::min(time, std::max(0., a.duration - 1. / a.fps)));
                        // A failed reader freezes its picture; audio and cues continue.
                        const QString error = it->second->error();
                        if (!error.isEmpty() && !videoErrors_.contains(c.id)) {
                            videoErrors_.insert(c.id);
                            report("Video error in " + a.name + ": " + error + " The picture holds its last frame; audio continues.");
                            playbackProblem = status;
                        }
                    }
                    if (source.isNull()) waiting = true;
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
    if (waiting) {
        ++lateFrames;
        if (!lastPicture_.isNull()) return lastPicture_;
        return picture;
    }
    lastComposed_ = frame; lastClocks_ = std::move(clocks); lastPicture_ = picture; return picture;
}
QImage Controller::composeProjectAt(const Project& project, Frame position, QSize maximum, const QMap<std::string, QImage>* videoFrames) {
    QSize outSize(project.width, project.height);
    if (maximum.isValid() && (outSize.width() > maximum.width() || outSize.height() > maximum.height()))
        outSize = outSize.scaled(maximum, Qt::KeepAspectRatio);
    QImage picture(outSize, QImage::Format_RGB32); picture.fill(Qt::black);
    QPainter painter(&picture); painter.setRenderHint(QPainter::SmoothPixmapTransform); painter.setRenderHint(QPainter::TextAntialiasing);
    painter.scale(static_cast<double>(outSize.width()) / std::max(1, project.width),
                  static_cast<double>(outSize.height()) / std::max(1, project.height));
    for (std::size_t track = 0; track < project.show.tracks.size(); ++track) {
        if (!project.show.tracks[track].visual || project.show.tracks[track].muted) continue;
        for (const auto& c : project.show.clips) {
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
                const auto asset = project.assets.constFind(QString::fromStdString(c.assetId));
                if (asset != project.assets.cend() && !asset->offline) {
                    QImage source = asset->still;
                    if (asset->video) {
                        if (videoFrames) {
                            source = videoFrames->value(c.id);
                        } else {
                            double time = seconds(c.offset + local);
                            if (c.loop) time = seconds(c.offset) + std::fmod(seconds(local), std::max(.001, asset->duration - seconds(c.offset)));
                            source = videoFrameAt(asset->path, std::min(time, std::max(0., asset->duration - 1. / std::max(1., asset->fps))), outSize);
                        }
                    }
                    if (!source.isNull()) {
                        const QSizeF fitted = QSizeF(source.size()).scaled(bounds.size(), Qt::KeepAspectRatio);
                        painter.drawImage(QRectF(-fitted.width() / 2, -fitted.height() / 2, fitted.width(), fitted.height()), source);
                    }
                }
            }
            painter.restore();
        }
    }
    painter.end();
    return picture;
}
QString Controller::mediaDirectory(const QString& target) {
    return QFileInfo(target).absolutePath() + "/" + QFileInfo(target).completeBaseName() + ".media";
}
bool Controller::savingCopiesMedia() const {
    return project.packageDirectory || std::any_of(project.assets.cbegin(), project.assets.cend(), [](const MediaAsset& a) { return a.path.isEmpty(); });
}
bool Controller::save(QString target) {
    if (!canEdit()) return false;
    try { Project candidate = project; materializeProjectMedia(candidate, mediaDirectory(target)); return commitSave(std::move(candidate), target); }
    catch (const std::exception& e) { report("Save failed: " + QString::fromUtf8(e.what())); return false; }
}
bool Controller::commitSave(Project candidate, QString target) {
    try {
        const auto sourceDirectory = project.packageDirectory;
        const QString previous = path;
        writeProject(candidate, target); project.assets = std::move(candidate.assets); bindAudio(project); project.packageDirectory.reset(); path = target; dirty = false; ++projectRevision_;
        // Undo must not restore temporary package paths or unsaved generated media.
        for (auto* history : {&undo_, &redo_}) for (auto& snapshot : *history) {
            bool extracted = false;
            for (auto it = snapshot.assets.begin(); it != snapshot.assets.end(); ++it) {
                if (const auto saved = project.assets.constFind(it.key()); saved != project.assets.cend()) { it->path = saved->path; it->audio = saved->audio; }
                else extracted |= sourceDirectory && it->path.startsWith(sourceDirectory->path() + "/");
            }
            if (!extracted) snapshot.packageDirectory.reset();
            bindAudio(snapshot);
        }
        if (sourceDirectory) arm(position());
        // The edits now live in target; a sidecar left next to the previous file would offer them again.
        for (const auto& saved : {target, previous}) if (!saved.isEmpty()) QFile::remove(saved + ".autosave");
        QFile::remove(untitledAutosavePath());
        report("Saved " + QFileInfo(target).fileName()); emit changed();
        return true;
    } catch (const std::exception& e) { report("Save failed: " + QString::fromUtf8(e.what())); return false; }
}

}
