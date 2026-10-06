#include "app/Controller.h"
#include <QDataStream>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QPainter>
#include <QSaveFile>
#include <chrono>
#include <set>

namespace sav {
Controller::Controller(QObject* parent) : QObject(parent), project(newProject()), device(transport) {
    arm(0);
    timer_.setInterval(16);
    connect(&timer_, &QTimer::timeout, this, [this] {
        if (configured_ && !device.running()) {
            configured_ = false; device.close();
            // Once the device is closed, it is safe to render a zero-sized control block.
            transport.command({Action::StopAll}); transport.render(nullptr, 0, 0);
            report("Audio device stopped. Playback is halted; reconnect it in Outputs.");
        }
        if (transport.eventOverflow() && transport.state() != State::Stopped) {
            stopAll(); report("MIDI event queue overflow. Playback is stopped; inspect the project.");
        }
        emit tick();
    }); timer_.start();
    autosave_.setInterval(60000);
    connect(&autosave_, &QTimer::timeout, this, [this] {
        if (!dirty || path.isEmpty() || busy) return;
        try { writeProject(project, path + ".autosave"); }
        catch (const std::exception& e) { report("Autosave failed: " + QString::fromUtf8(e.what())); }
    }); autosave_.start();
}
Controller::~Controller() {
    timer_.stop(); midiThread_.request_stop(); if (midiThread_.joinable()) midiThread_.join(); device.close();
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
    validate(next); project = std::move(next); path = std::move(projectPath); dirty = false;
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
    midiThread_.request_stop(); if (midiThread_.joinable()) midiThread_.join();
    device.close(); videos_.clear(); lastPicture_ = {}; lastComposed_ = -1; lateFrames = 0;
    armed_ = std::make_shared<Show>(project.show); transport.configure(armed_, position);
    if (configured_) {
        QString error;
        if (!device.open(api_, deviceId_, channels_, buffer_, error)) { configured_ = false; report("Audio output unavailable: " + error); }
    }
    startMidiThread();
}
void Controller::startMidiThread() {
    const auto snapshot = armed_;
    midiThread_ = std::jthread([this, snapshot](std::stop_token stop) {
        while (!stop.stop_requested()) {
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
bool Controller::routesValid() const {
    for (const auto& track : project.show.tracks) {
        if (track.visual || track.muted) continue;
        if (track.left >= static_cast<int>(device.channels()) || track.right >= static_cast<int>(device.channels())) return false;
    }
    return true;
}
void Controller::toggle() {
    if (busy) return;
    if (!device.running()) { report("Configure an audio device in Outputs before playback"); return; }
    if (!routesValid()) { report("A track is routed beyond the opened device channels. Fix its routing or mute it."); return; }
    const bool playing = transport.state() == State::Playing;
    if (!playing) {
        for (const auto& asset : project.assets) if (asset.video && !QFileInfo::exists(asset.path)) {
            report("Video source is missing: " + asset.path); return;
        }
        for (const auto& marker : project.show.markers) if (!marker.message.empty() && !midi.connected()) {
            report("This show has MIDI cues. Connect a MIDI output in Outputs before playback."); return;
        }
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
    api_ = api; deviceId_ = id; channels_ = channels; buffer_ = buffer; audioName = name;
    QString error;
    configured_ = device.open(api, id, channels, buffer, error);
    if (configured_) {
        QSettings settings; settings.setValue("audio/api", static_cast<int>(api)); settings.setValue("audio/device", name);
        settings.setValue("audio/channels", channels); settings.setValue("audio/buffer", buffer);
        report(QString("%1 · %2 channels · 48 kHz · %3 samples").arg(name).arg(channels).arg(device.bufferFrames()));
    }
    else report("Audio output unavailable: " + error);
    return configured_;
}
bool Controller::configureMidi(int port) {
    if (!canEdit()) return false;
    midiThread_.request_stop(); if (midiThread_.joinable()) midiThread_.join();
    QString error; const bool ok = midi.open(port, error); startMidiThread();
    if (ok) QSettings().setValue("midi/port", midi.name());
    report(ok ? (port < 0 ? "MIDI output disconnected" : "MIDI output: " + midi.name()) : error); return ok;
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
        if (!needed.contains(it->first)) it = videos_.erase(it);
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
                        const QString error = it->second->error();
                        if (!error.isEmpty()) {
                            if (transport.state() == State::Playing || transport.state() == State::Held) stopAll();
                            const QString message = "Video error: " + error;
                            if (status != message) report(message);
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
        QDir mediaDir(QFileInfo(target).absolutePath() + "/" + QFileInfo(target).completeBaseName() + ".media");
        for (auto& a : candidate.assets) if (a.path.isEmpty()) {
            if (!QDir().mkpath(mediaDir.absolutePath())) throw std::runtime_error("Cannot create generated media directory");
            a.path = mediaDir.filePath(newId() + (a.image ? ".png" : ".wav"));
            if (a.image) {
                QSaveFile image(a.path);
                if (!image.open(QIODevice::WriteOnly) || !a.still.save(&image, "PNG") || !image.commit()) throw std::runtime_error("Cannot save generated image");
            } else if (a.audio) {
                QSaveFile wav(a.path);
                if (!wav.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot save generated audio");
                QDataStream out(&wav); out.setByteOrder(QDataStream::LittleEndian);
                const quint16 channels = a.audio->mono ? 1 : 2;
                const quint32 bytes = static_cast<quint32>(a.audio->length() * channels * 2);
                out.writeRawData("RIFF", 4); out << quint32(bytes + 36); out.writeRawData("WAVEfmt ", 8);
                out << quint32(16) << quint16(1) << channels << quint32(sampleRate) << quint32(sampleRate * channels * 2) << quint16(channels * 2) << quint16(16);
                out.writeRawData("data", 4); out << bytes;
                for (std::size_t i = 0; i < a.audio->samples.size(); i += a.audio->mono ? 2 : 1)
                    out << static_cast<qint16>(std::clamp(a.audio->samples[i], -1.f, 1.f) * 32767);
                if (out.status() != QDataStream::Ok || !wav.commit()) throw std::runtime_error("Cannot finish generated audio file");
            } else throw std::runtime_error("Asset has no source file");
        }
        writeProject(candidate, target); project.assets = std::move(candidate.assets); path = target; dirty = false;
        report("Saved " + QFileInfo(target).fileName()); emit changed();
    } catch (const std::exception& e) { report("Save failed: " + QString::fromUtf8(e.what())); }
}
}
