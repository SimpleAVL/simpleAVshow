// Opt-in live rehearsal: the production MainWindow, Controller, audio callback and MIDI worker.
// Default: real loopback device. --software records the production mixer without a device.
#include "app/MainWindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QScrollBar>
#include <QSplitter>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QSettings>
#include <QTimer>
#include <QtEndian>
#include <iostream>
#include <set>
#include <chrono>

using namespace sav;
namespace {
QString stateName(State s) {
    switch (s) {
    case State::Stopped: return "STOPPED";
    case State::Playing: return "PLAYING";
    case State::Paused: return "PAUSED";
    case State::Held: return "HOLDING";
    case State::Ended: return "ENDED";
    }
    return "UNKNOWN";
}
QByteArray pictureHash(const QImage& image) {
    return QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char*>(image.constBits()), image.sizeInBytes()), QCryptographicHash::Sha256).toHex();
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("SimpleAVShowLiveValidation");
    app.setApplicationName("LighthouseRehearsal");
    app.setStyle("Fusion"); app.setFont(QFont("Arial", 10));
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#192331")); palette.setColor(QPalette::WindowText, QColor("#dee7f3"));
    palette.setColor(QPalette::Base, QColor("#141d28")); palette.setColor(QPalette::Text, QColor("#dee7f3"));
    palette.setColor(QPalette::Button, QColor("#263448")); palette.setColor(QPalette::ButtonText, QColor("#dee7f3"));
    palette.setColor(QPalette::Highlight, QColor("#486ca5")); app.setPalette(palette);
    QCommandLineParser parser; parser.addHelpOption();
    parser.addOption({"project", "Real-file .savshow to rehearse", "path"});
    parser.addOption({"output", "Evidence directory", "path"});
    parser.addOption({"device", "Required loopback output name", "name", "BlackHole 16ch"});
    parser.addOption({"midi", "Virtual receiving port name", "name", "Lighthouse MIDI Logger"});
    parser.addOption({"scenario", "show, routing, async, long-hold, package audit, or silent midi-only rehearsal", "name", "show"});
    parser.addOption({"buffer", "Output buffer frames", "frames", "512"});
    parser.addOption({"hold-seconds", "Wall time at each stage hold", "seconds", "4"});
    parser.addOption({"list-devices", "List available native audio outputs and MIDI destinations, then exit"});
    parser.addOption({"software", "Record the real-file mixer to WAV without an audio device; NOT hardware-loopback proof"});
    parser.process(app);
    if (parser.isSet("list-devices")) {
        Transport transport; AudioDevice device(transport); MidiOutput midi; QJsonArray outputs, ports;
        for (const auto& d : device.devices(RtAudio::MACOSX_CORE)) outputs.append(QJsonObject{{"id", static_cast<int>(d.id)}, {"name", d.name}, {"channels", static_cast<int>(d.channels)}});
        for (const auto& p : midi.ports()) ports.append(p);
        std::cout << QJsonDocument(QJsonObject{{"outputs", outputs}, {"midi", ports}}).toJson().toStdString(); return 0;
    }
    const QString out = QDir(parser.value("output")).absolutePath(); QDir().mkpath(out);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, out + "/settings");
    const QString scenario = parser.value("scenario");
    const bool software = parser.isSet("software");
    qint64 renderedFrames = 0, renderCallbacks = 0;
    QFile softwareWav(out + "/software-mix-16ch.wav");
    auto wavHeader = [&] {
        const quint32 bytes = static_cast<quint32>(renderedFrames * 16 * 2);
        QByteArray h; h.append("RIFF", 4);
        auto u32 = [&](quint32 n) { n = qToLittleEndian(n); h.append(reinterpret_cast<const char*>(&n), 4); };
        auto u16 = [&](quint16 n) { n = qToLittleEndian(n); h.append(reinterpret_cast<const char*>(&n), 2); };
        u32(bytes + 36); h.append("WAVEfmt ", 8); u32(16); u16(1); u16(16); u32(48000); u32(48000*32); u16(32); u16(16); h.append("data", 4); u32(bytes);
        softwareWav.seek(0); softwareWav.write(h); softwareWav.seek(softwareWav.size());
    };
    if (software) {
        if (!softwareWav.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 2;
        wavHeader();
    }
    if (!QStringList{"show", "routing", "async", "long-hold", "midi", "package"}.contains(scenario)) return 2;
    QFile telemetry(out + "/telemetry.jsonl");
    if (!telemetry.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 2;
    QElapsedTimer wall; wall.start();
    auto log = [&](QJsonObject record) {
        record["wall_seconds"] = wall.nsecsElapsed()/1e9;
        if (software) record["rendered_frames"] = renderedFrames;
        telemetry.write(QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n'); telemetry.flush();
    };
    const double monotonic = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    log({{"kind", "origin"}, {"monotonic_seconds", monotonic-wall.nsecsElapsed()/1e9}});
    QJsonArray checks;
    bool passed = true, finishing = false;
    auto check = [&](QString name, bool ok, QString detail = {}) {
        checks.append(QJsonObject{{"name", name}, {"passed", ok}, {"detail", detail}});
        passed &= ok;
        log({{"kind", "check"}, {"name", name}, {"passed", ok}, {"detail", detail}});
        std::cout << (ok ? "PASS " : "FAIL ") << name.toStdString() << " " << detail.toStdString() << std::endl;
    };
    MainWindow window; auto& c = window.controller();
    QTimer softwareTimer;
    softwareTimer.setTimerType(Qt::PreciseTimer); softwareTimer.setInterval(11);
    QObject::connect(&softwareTimer, &QTimer::timeout, [&] {
        std::array<float, 512*16> samples{};
        std::array<qint16, 512*16> pcm{};
        c.transport.render(samples.data(), 512, 16);
        for (std::size_t i=0; i<samples.size(); ++i) pcm[i] = qToLittleEndian(static_cast<qint16>(std::clamp(samples[i], -1.f, 1.f)*32767));
        softwareWav.write(reinterpret_cast<const char*>(pcm.data()), sizeof(pcm)); renderedFrames += 512; ++renderCallbacks;
    });
    auto toggle = [&] {
        if (software) c.transport.command({c.transport.state() == State::Playing ? Action::Pause : Action::Play});
        else c.toggle();
    };
    window.resize(1560, 980); window.show();
    if (software) {
        auto* timeline = window.findChild<Timeline*>();
        timeline->verticalScrollBar()->setValue(4*56);
        for (auto* splitter : window.findChildren<QSplitter*>()) if (splitter->orientation() == Qt::Vertical) splitter->setSizes({350, 530});
    }
    QTimer liveImages; liveImages.setInterval(200);
    QObject::connect(&liveImages, &QTimer::timeout, [&] {
        QSaveFile image(out + "/live-editor.jpg");
        if (image.open(QIODevice::WriteOnly)) { window.grab().save(&image, "JPEG", 85); image.commit(); }
    });
    liveImages.start();
    auto capture = [&](QString name) {
        window.grab().save(out + "/" + name + "-editor.png");
        c.compose().save(out + "/" + name + "-program.png");
    };
    auto sample = [&] {
        const auto s = c.transport.state();
        log({{"kind", "sample"}, {"state", stateName(s)}, {"show_seconds", seconds(c.position())},
             {"async_voices", c.transport.asyncVoices()}, {"peak", c.transport.peak()},
             {"callbacks", static_cast<qint64>(c.device.callbacks())}, {"underruns", static_cast<int>(c.device.underruns())},
             {"audio_waits", static_cast<int>(c.transport.bufferWaits())}, {"read_retries", static_cast<int>(c.transport.readRetries())},
             {"video_waits", static_cast<int>(c.lateFrames)}, {"clipped", c.transport.clipped()},
             {"audio_read_failed", c.transport.audioReadFailed()}, {"problem", c.playbackProblem}, {"status", c.status}});
    };
    auto action = [&](QString name, const std::function<void()>& fn) {
        log({{"kind", "action"}, {"name", name}, {"show_seconds", seconds(c.position())}}); fn();
    };
    auto finish = [&] {
        if (finishing) return;
        finishing = true;
        sample(); capture("final");
        check("no clipping", !c.transport.clipped());
        check("no source read failure", !c.transport.audioReadFailed());
        check("no MIDI queue overflow", !c.transport.eventOverflow());
        check(software ? "software render blocks advanced" : "audio callbacks advanced", software ? renderCallbacks > 10 : c.device.callbacks() > 10);
        if (!software) check("no driver underruns", c.device.underruns() == 0, QString::number(c.device.underruns()));
        check("no unresolved playback problem", c.playbackProblem.isEmpty(), c.playbackProblem);
        action("Stop All", [&] { c.stopAll(); });
        QTimer::singleShot(500, &app, [&] {
            sample(); check("Stop All clears async voices", c.transport.asyncVoices() == 0);
            if (software) { softwareTimer.stop(); wavHeader(); softwareWav.close(); }
            QFile summary(out + "/runner-summary.json");
            if (!summary.open(QIODevice::WriteOnly)) { std::cerr << "Cannot write validation summary" << std::endl; app.exit(2); return; }
            summary.write(QJsonDocument(QJsonObject{{"passed", passed}, {"scenario", scenario},
                {"backend", software ? "software mixer; no device loopback" : "native CoreAudio output"},
                {"device", c.audioName}, {"channels", software ? 16 : static_cast<int>(c.device.channels())},
                {"rendered_frames", renderedFrames},
                {"buffer_frames", static_cast<int>(c.device.bufferFrames())}, {"checks", checks},
                {"wall_seconds", wall.nsecsElapsed()/1e9}}).toJson(QJsonDocument::Indented));
            c.dirty = false; app.exit(passed ? 0 : 1);
        });
    };
    try {
        const QString projectPath = QFileInfo(parser.value("project")).absoluteFilePath();
        auto p = readProject(projectPath);
        check("real media loaded", !p.assets.isEmpty() && std::all_of(p.assets.cbegin(), p.assets.cend(), [](const MediaAsset& a) { return !a.path.isEmpty() && !a.offline; }));
        if (scenario == "show" || scenario == "package") {
            writeProject(p, out + "/roundtrip.savshow");
            const auto roundtrip = readProject(out + "/roundtrip.savshow");
            check("save/reopen preserves show", roundtrip.show.clips.size() == p.show.clips.size() && roundtrip.show.markers.size() == p.show.markers.size());
            writeProjectPackage(p, out + "/last-lighthouse.savpack");
            const auto packaged = readProjectPackage(out + "/last-lighthouse.savpack");
            check("portable package reopens", packaged.show.clips.size() == p.show.clips.size() && packaged.assets.size() == p.assets.size());
            bool identical = true;
            for (const auto& asset : p.assets) {
                QFile original(asset.path), extracted(packaged.assets.value(asset.id).path);
                if (!original.open(QIODevice::ReadOnly) || !extracted.open(QIODevice::ReadOnly)) { identical = false; break; }
                QCryptographicHash before(QCryptographicHash::Sha256), after(QCryptographicHash::Sha256);
                if (!before.addData(&original) || !after.addData(&extracted) || before.result() != after.result()) { identical = false; break; }
            }
            check("all packaged media bytes match original sources", identical);
            // Actually rehearse extracted package sources, not only the original paths.
            p = packaged;
        }
        if (scenario == "package") {
            QFile summary(out + "/runner-summary.json");
            if (!summary.open(QIODevice::WriteOnly)) return 2;
            summary.write(QJsonDocument(QJsonObject{{"passed", passed}, {"scenario", scenario},
                {"backend", "native persistence; no playback"}, {"checks", checks}}).toJson(QJsonDocument::Indented));
            return passed ? 0 : 1;
        }
        if (scenario == "midi") for (auto& track : p.show.tracks) if (!track.visual) track.muted = true;
        c.replace(std::move(p), projectPath);
        if (!software) {
        const auto devices = c.device.devices(RtAudio::MACOSX_CORE);
        const auto device = std::find_if(devices.begin(), devices.end(), [&](const auto& d) { return d.name == parser.value("device") || d.name.endsWith(": " + parser.value("device")); });
        if (device == devices.end() || device->channels < (scenario == "midi" ? 2u : 8u)) throw std::runtime_error("Required audio output is unavailable");
        if (!c.configureAudio(RtAudio::MACOSX_CORE, device->id, device->name, scenario == "midi" ? 2 : 16 <= device->channels ? 16 : 8, parser.value("buffer").toUInt()))
            throw std::runtime_error(c.status.toStdString());
        }
        if (c.needsMidi()) {
            const auto ports = c.midi.ports();
            int port = -1;
            for (int i = 0; i < ports.size(); ++i) if (ports[i].contains(parser.value("midi"))) { port = i; break; }
            if (port < 0 || !c.configureMidi(port)) throw std::runtime_error("Required MIDI receiver is unavailable");
            check("native MIDI destination connected", c.midi.connected(), c.midi.name());
        }
        window.findChild<Timeline*>()->zoomToFit();
        capture("loaded");
        log({{"kind", "ready"}, {"device", c.audioName}, {"midi", c.midi.name()}, {"scenario", scenario}, {"software", software}});
        if (software) softwareTimer.start();
    } catch (const std::exception& e) {
        check("setup", false, QString::fromUtf8(e.what())); finish(); return app.exec();
    }
    QTimer timer; timer.setInterval(100);
    int heldCount = 0, phase = 0;
    bool manualPaused = false, pauseDone = false;
    qint64 holdStart = -1, pauseStart = -1, phaseStart = -1;
    Frame frozen = 0;
    QByteArray frozenPicture;
    std::set<Frame> visitedHolds;
    const double holdSeconds = parser.value("hold-seconds").toDouble();
    QTimer::singleShot(2000, &app, [&] { action("Play", toggle); });
    QObject::connect(&timer, &QTimer::timeout, [&] {
        if (finishing) return;
        sample(); const auto s = c.transport.state(); const double pos = seconds(c.position()); const qint64 now = wall.elapsed();
        if (now > (scenario == "long-hold" ? 390000 : 145000)) { check("rehearsal timeout", false); finish(); return; }
        if (c.transport.audioReadFailed() || !c.playbackProblem.isEmpty()) { check("playback remains healthy", false, c.playbackProblem); finish(); return; }
        if (scenario == "routing") { if (s == State::Ended) { check("routing show reached end", true); finish(); } return; }
        if (scenario == "async") {
            if (phase == 0 && s == State::Playing && pos >= 2) {
                frozen = c.position(); action("Pause with async loop and one-shot", toggle); phase = 1; phaseStart = now;
            } else if (phase == 1 && now-phaseStart >= 2200) {
                check("pause freezes timeline", s == State::Paused && std::abs(c.position()-frozen) < frames(.15));
                check("one-shot ends during pause", c.transport.asyncVoices() == 1);
                action("Seek inside async interval", [&] { c.seek(frames(8)); }); phase = 2; phaseStart = now;
            } else if (phase == 2 && now-phaseStart >= 1000) {
                check("inside seek preserves running loop", s == State::Paused && c.transport.asyncVoices() == 1 && c.position() == frames(8));
                action("Seek exactly to async clip end", [&] { c.seek(frames(16)); }); phase = 3; phaseStart = now;
            } else if (phase == 3 && now-phaseStart >= 800) {
                check("end seek stops async voices", c.transport.asyncVoices() == 0);
                action("Seek into inactive clip while paused", [&] { c.seek(frames(4)); }); phase = 4; phaseStart = now;
            } else if (phase == 4 && now-phaseStart >= 700) {
                check("paused seek does not start inactive voices", c.transport.asyncVoices() == 0);
                action("Play from inside async clip", toggle); phase = 5; phaseStart = now;
            } else if (phase == 5 && now-phaseStart >= 1600) {
                check("play starts loop at sought offset", s == State::Playing && c.transport.asyncVoices() == 1);
                action("Stop All while playing", [&] { c.stopAll(); }); phase = 6; phaseStart = now;
            } else if (phase == 6 && now-phaseStart >= 600) {
                check("Stop All stops transport", s == State::Stopped && c.transport.asyncVoices() == 0); finish();
            }
            return;
        }
        if (s == State::Held) {
            if (holdStart < 0) {
                holdStart = now; frozen = c.position(); frozenPicture = pictureHash(c.compose()); ++heldCount;
                check("each hold reached once", visitedHolds.insert(frozen).second);
                log({{"kind", "hold_arrival"}, {"number", heldCount}, {"show_seconds", pos}});
                capture(QString("hold-%1").arg(heldCount));
            }
            const double wait = scenario == "long-hold" ? 300 : holdSeconds;
            if (now-holdStart >= wait*1000) {
                check(QString("hold %1 freezes show time").arg(heldCount), c.position() == frozen);
                check(QString("hold %1 freezes picture").arg(heldCount), pictureHash(c.compose()) == frozenPicture);
                if (scenario != "midi") {
                    check(QString("hold %1 sustains async bed").arg(heldCount), c.transport.asyncVoices() == 1);
                    if (heldCount == 1) check("async one-shot did not restart", c.transport.asyncVoices() == 1);
                }
                action("Continue hold", toggle); holdStart = -1;
                if (scenario == "long-hold") { QTimer::singleShot(2000, &app, finish); }
            }
        } else if (scenario == "show" && s == State::Playing && pos >= 20.5 && !pauseDone && !manualPaused) {
            manualPaused = true; frozen = c.position(); pauseStart = now;
            action("Manual pause", toggle);
        } else if (manualPaused && now-pauseStart >= 2000) {
            check("manual pause freezes timeline", s == State::Paused && std::abs(c.position()-frozen) < frames(.15));
            check("manual pause sustains async sea", c.transport.asyncVoices() == 1);
            action("Resume manual pause", toggle); manualPaused = false; pauseDone = true;
        } else if (s == State::Ended) {
            check("full show reached end", true); check("three stage holds completed", heldCount == 3);
            check("project end clears async voices", c.transport.asyncVoices() == 0); finish();
        }
    });
    timer.start(); return app.exec();
}
