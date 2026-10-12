#include "app/Controller.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMetaObject>
#include <QPainter>
#include <chrono>
#include <set>

namespace sav {
Controller::Controller(QObject* parent) : QObject(parent), project(newProject()), device(transport) {
    midiName_ = QSettings().value("midi/port").toString();
    arm(0);
    timer_.setInterval(16);
    connect(&timer_, &QTimer::timeout, this, [this] {
        if (configured_ && !device.running()) {
            configured_ = false; device.close();
            // Once the device is closed, it is safe to render a zero-sized control block.
            transport.command({Action::StopAll}); transport.render(nullptr, 0, 0);
            report("Audio output stopped. Press Play to reconnect, or choose another device in Outputs.");
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
    }); timer_.start(); midiWatch_.start();
    autosave_.setInterval(60000);
    connect(&autosave_, &QTimer::timeout, this, [this] {
        if (!dirty || path.isEmpty() || busy) return;
        try { writeProject(project, path + ".autosave"); }
        catch (const std::exception& e) { report("Autosave failed: " + QString::fromUtf8(e.what())); }
    }); autosave_.start();
}
Controller::~Controller() {
    timer_.stop(); stopMidiThread(); device.close();
}
void Controller::report(QString text) { status = text; emit message(text); }
bool Controller::canEdit(bool reportFailure) {
    const bool allowed = !showMode && !busy && transport.state() != State::Playing && transport.state() != State::Held &&
        transport.asyncVoices() == 0;
    if (!allowed && reportFailure) report(showMode ? "Leave show mode to edit" : "Use Stop All before editing; async audio may still be playing");
    return allowed;
}
bool Controller::edit(const std::function<void(Project&)>& mutation) {
    if (!canEdit()) return false;
    try {
        Project next = project; mutation(next); bindAudio(next); validate(next);
        const Frame time = std::min(position(), next.show.duration);
        undo_.push_back(project); if (undo_.size() > 32) undo_.erase(undo_.begin()); redo_.clear();
        project = std::move(next); dirty = true; arm(time); emit changed(); return true;
    } catch (const std::exception& e) { report(QString::fromUtf8(e.what())); return false; }
}
void Controller::replace(Project next, QString projectPath) {
    validate(next);
    // Readers hold extracted package files open; Windows cannot remove them while open.
    videos_.clear(); lastPicture_ = {};
    project = std::move(next); path = std::move(projectPath); dirty = false; playbackProblem.clear();
    undo_.clear(); redo_.clear();
    arm(0); report("Project ready. Space plays or pauses; Stop All silences async audio."); emit changed();
}
void Controller::undo() {
    if (!canEdit() || undo_.empty()) return;
    redo_.push_back(project); project = std::move(undo_.back()); undo_.pop_back(); dirty = true;
    arm(std::min(position(), project.show.duration)); emit changed();
}
void Controller::redo() {
    if (!canEdit() || redo_.empty()) return;
    undo_.push_back(project); project = std::move(redo_.back()); redo_.pop_back(); dirty = true;
    arm(std::min(position(), project.show.duration)); emit changed();
}
void Controller::arm(Frame position) {
    stopMidiThread();
    device.close(); videos_.clear(); videoErrors_.clear(); lastPicture_ = {}; lastComposed_ = -1; lateFrames = 0;
    armed_ = std::make_shared<Show>(project.show); transport.configure(armed_, position);
    if (configured_) {
        QString error;
        if (!device.open(api_, deviceId_, channels_, buffer_, error)) { configured_ = false; report("Audio output unavailable: " + error); }
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
    if (!playing) {
        playbackProblem.clear();
        // Re-arming retries sources that were briefly unavailable; changed sources still need reopening.
        if (transport.audioReadFailed() && state != State::Held && !transport.asyncVoices()) arm(position());
        if (transport.audioReadFailed()) { report("Audio source is unreadable. Restore missing media and press Play to retry, or reopen the project to rebuild changed or damaged caches."); playbackProblem = status; return; }
        // Resuming a live show never blocks on picture or MIDI; it warns and continues.
        const bool resuming = state == State::Held || transport.asyncVoices() || (state == State::Paused && device.running());
        if (const QString missing = missingVideo(); !missing.isEmpty()) {
            report("Video source is missing: " + missing); playbackProblem = status;
            if (!resuming) return;
        }
        if (needsMidi() && !prepareMidi()) { playbackProblem = status; if (!resuming) return; }
        if (!prepareAudio()) { playbackProblem = status; return; }
        if (!routesValid()) { report("A track needs more output channels. Open Outputs to increase channels, or change the track's routing."); playbackProblem = status; return; }
    }
    if (!transport.command({playing ? Action::Pause : Action::Play})) report("Transport command queue is full");
}
void Controller::seek(Frame time) {
    if (busy || showMode) { report(showMode ? "Leave show mode to jump on the timeline" : "Media is loading"); return; }
    if (!transport.command({Action::Seek, time})) report("Transport command queue is full");
    if (!device.running()) transport.render(nullptr, 0, 0);
    lastComposed_ = -1;
}
void Controller::stopAll() {
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
    configured_ = device.open(api, id, channels, buffer, error);
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
    if (ok) { midiName_ = midi.name(); QSettings().setValue("midi/port", midiName_); playbackProblem.clear(); }
    report(ok ? (port < 0 ? "MIDI output disconnected" : "MIDI output: " + midi.name()) : error); return ok;
}
QString Controller::missingVideo() const {
    for (const auto& clip : project.show.clips) {
        const auto asset = project.assets.constFind(QString::fromStdString(clip.assetId));
        if (asset != project.assets.cend() && asset->video && !QFileInfo::exists(asset->path)) return asset->path;
    }
    return {};
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
    const Frame frame = static_cast<Frame>(std::floor(seconds(position) * project.fps));
    if (frame == lastComposed_ && !lastPicture_.isNull()) return lastPicture_;
    QImage picture(project.width, project.height, QImage::Format_RGB32); picture.fill(Qt::black);
    QPainter painter(&picture); painter.setRenderHint(QPainter::SmoothPixmapTransform); painter.setRenderHint(QPainter::TextAntialiasing);
    bool waiting = false;
    // Track 0 is the background; successive visual tracks are composited over it.
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
                const auto& a = project.assets[QString::fromStdString(c.assetId)];
                QImage source = a.still;
                if (a.video) {
                    double time = seconds(c.offset + local);
                    if (c.loop) time = seconds(c.offset) + std::fmod(seconds(local), std::max(.001, a.duration - seconds(c.offset)));
                    auto it = videos_.find(c.id);
                    if (it != videos_.end()) {
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
    lastComposed_ = frame; lastPicture_ = picture; return picture;
}
void Controller::save(QString target) {
    if (!canEdit()) return;
    try {
        Project candidate = project;
        const QString directory = QFileInfo(target).absolutePath() + "/" + QFileInfo(target).completeBaseName() + ".media";
        const auto sourceDirectory = project.packageDirectory;
        materializeProjectMedia(candidate, directory);
        writeProject(candidate, target); project.assets = std::move(candidate.assets); bindAudio(project); project.packageDirectory.reset(); path = target; dirty = false;
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
        report("Saved " + QFileInfo(target).fileName()); emit changed();
    } catch (const std::exception& e) { report("Save failed: " + QString::fromUtf8(e.what())); }
}
}
