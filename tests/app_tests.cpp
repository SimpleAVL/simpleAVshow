#include "app/MainWindow.h"
#include "app/MidiMessageEditor.h"
#include "app/Project.h"
#include "media/Media.h"
#include "media/MidiOutput.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QPushButton>
#include <QListWidget>
#include <QTableWidget>
#include <QDialog>
#include <QLabel>
#include <QAction>
#include <QTimer>
#include <QComboBox>
#include <QSpinBox>
#include <QLineEdit>
#include <QSettings>
#include <QCheckBox>
#include <limits>

using namespace sav;
namespace {
float sampleAt(const AudioData& audio, Frame frame) {
    if (!audio.storage) return audio.samples[frame * 2];
    float stereo[2]; audio.storage->read(frame, stereo, 1); return stereo[0];
}
class UnreadableStorage : public AudioStorage {
public:
    Frame length() const noexcept override { return frames(2); }
    void read(Frame, float*, std::size_t) const override { throw std::runtime_error("Cache is unavailable"); }
};
}
class AppTests : public QObject {
    Q_OBJECT
    QTemporaryDir settingsDir_;
private slots:
    void initTestCase() {
        QVERIFY(settingsDir_.isValid());
        QCoreApplication::setOrganizationName("SimpleAVShowTests");
        QCoreApplication::setApplicationName("PlaybackUX");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir_.path());
    }
    void init() { QSettings().clear(); }
    void audioSelectionRestoresByNameWithoutFallback() {
        const std::vector<DeviceDescription> devices = {{17, "Show interface", 8, false}, {42, "Speakers", 2, true}};
        auto selected = playbackDevice(devices, {}); QVERIFY(selected); QCOMPARE(selected->id, 42u);
        selected = playbackDevice(devices, "Show interface"); QVERIFY(selected); QCOMPARE(selected->id, 17u);
        QVERIFY(!playbackDevice(devices, "Unplugged interface")); QVERIFY(!playbackDevice({}, {}));
        selected = playbackDevice({{99, "Only output", 2, false}}, {}); QVERIFY(selected); QCOMPARE(selected->id, 99u);
    }
    void midiEditorPreservesExistingMessages() {
        MidiMessageEditor editor; std::vector<unsigned char> result; QString error;
        const std::vector<std::vector<unsigned char>> messages = {
            {}, {0x90, 60, 100}, {0x9f, 127, 0}, {0x80, 0, 64}, {0xbf, 64, 127},
            {0xc0, 0}, {0xcf, 127}, {0xa2, 60, 90}, {0xd4, 127},
            {0xe0, 0, 0}, {0xe1, 0, 64}, {0xef, 127, 127}, {0xf0, 0x7d, 1, 0xf7}, {0xfa}, {0xf2, 0, 2}
        };
        for (const auto& bytes : messages) {
            editor.setMessage(bytes); QVERIFY2(editor.message(result, error), qPrintable(error)); QVERIFY(result == bytes);
        }
        editor.setMessage({0xcf, 127});
        QCOMPARE(editor.findChild<QSpinBox*>("midiValue")->value(), 128);
        QVERIFY(editor.findChild<QLabel*>("midiMessagePreview")->text().contains("Program 128"));
        editor.setMessage({0xe1, 0, 64}); QCOMPARE(editor.findChild<QSpinBox*>("midiValue")->value(), 0);
    }
    void midiEditorUsesMusicalFieldsAndValidatesRawInput() {
        MidiMessageEditor editor; editor.show();
        auto* type = editor.findChild<QComboBox*>("midiMessageType");
        auto* note = editor.findChild<QComboBox*>("midiNote");
        auto* control = editor.findChild<QComboBox*>("midiController");
        auto* value = editor.findChild<QSpinBox*>("midiValue");
        auto* amount = editor.findChild<QSpinBox*>("midiAmount");
        auto* raw = editor.findChild<QLineEdit*>("midiRawBytes");
        std::vector<unsigned char> bytes; QString error;
        QVERIFY(type && note && control && value && amount && raw);
        type->setCurrentIndex(1); QVERIFY(note->isVisible()); QVERIFY(control->isHidden()); QVERIFY(raw->isHidden());
        QVERIFY(editor.message(bytes, error)); QVERIFY(bytes == std::vector<unsigned char>({0x90, 60, 100}));
        type->setCurrentIndex(3); QVERIFY(note->isHidden()); QVERIFY(control->isVisible());
        QVERIFY(editor.message(bytes, error)); QVERIFY(bytes == std::vector<unsigned char>({0xb0, 7, 127}));
        type->setCurrentIndex(4); value->setValue(128); QVERIFY(amount->isHidden());
        QVERIFY(editor.message(bytes, error)); QVERIFY(bytes == std::vector<unsigned char>({0xc0, 127}));
        type->setCurrentIndex(7); value->setValue(-8192); QVERIFY(editor.message(bytes, error));
        QVERIFY(bytes == std::vector<unsigned char>({0xe0, 0, 0}));
        value->setValue(8191); QVERIFY(editor.message(bytes, error)); QVERIFY(bytes == std::vector<unsigned char>({0xe0, 127, 127}));
        type->setCurrentIndex(8); QVERIFY(raw->isVisible()); QVERIFY(value->isHidden());
        raw->setText("F0 7D 01"); QVERIFY(!editor.message(bytes, error)); QVERIFY(error.contains("F7"));
        raw->setText("GG 01"); QVERIFY(!editor.message(bytes, error));
        raw->setText("f0 7d 01 f7"); QVERIFY(editor.message(bytes, error)); QVERIFY(bytes == std::vector<unsigned char>({0xf0, 0x7d, 1, 0xf7}));
        raw->clear(); QVERIFY(!editor.message(bytes, error));
        type->setCurrentIndex(0); QVERIFY(editor.message(bytes, error)); QVERIFY(bytes.empty());
    }
    void missingMidiExplainsSetupAndDoesNotStartPlayback() {
        Controller c; auto project = newProject();
        project.show.markers = {{"midi", "Lighting cue", frames(1), false, MidiTiming::Arrival, {0xc0, 0}}};
        c.replace(project); QVERIFY(c.needsMidi()); c.toggle();
        QCOMPARE(c.transport.state(), State::Stopped); QVERIFY(!c.device.running());
        QVERIFY(c.status.contains("Choose a MIDI destination")); QCOMPARE(c.playbackProblem, c.status);
        QSettings().setValue("midi/port", "SimpleAVShow absent MIDI test destination");
        c.setShowMode(true); c.toggle(); QCOMPARE(c.transport.state(), State::Stopped);
        QVERIFY(c.status.contains("missing")); QVERIFY(c.status.contains("SimpleAVShow absent MIDI test destination"));
        c.setShowMode(false); QVERIFY(c.configureMidi(-1)); QVERIFY(QSettings().value("midi/port").toString().isEmpty()); QVERIFY(c.playbackProblem.isEmpty());
    }
    void midiTestingRespectsPlaybackAndShowLocks() {
        Controller c; c.setShowMode(true); QVERIFY(!c.testMidi({0x90, 60, 100}));
        c.setShowMode(false); QVERIFY(!c.testMidi({0x90, 60})); QVERIFY(c.status.contains("Complete"));
        QVERIFY(c.transport.command({Action::Play})); c.transport.render(nullptr, 1, 0);
        QVERIFY(!c.testMidi({0x90, 60, 100})); QVERIFY(c.status.contains("Stop All"));
        c.stopAll(); QVERIFY(!c.testMidi({0x90, 60, 100})); QVERIFY(c.status.contains("Choose a MIDI destination"));
    }
    void cueEditorAppliesProgramAndRejectsIncompleteSysex() {
        MainWindow window; auto project = newProject();
        project.show.markers = {{"cue", "Select scene", frames(1), false, MidiTiming::Arrival, {0xc0, 0}}};
        window.controller().replace(project); window.show();
        window.findChild<Timeline*>()->selectMarker("cue");
        auto* type = window.findChild<QComboBox*>("midiMessageType");
        auto* value = window.findChild<QSpinBox*>("midiValue");
        auto* timing = window.findChild<QComboBox*>("midiCueTiming"); QVERIFY(type && value && timing); QVERIFY(!timing->isEnabled());
        QPushButton* apply = nullptr;
        for (auto* b : window.findChildren<QPushButton*>()) if (b->text() == "Apply cue") apply = b;
        QVERIFY(apply); value->setValue(128); QTest::mouseClick(apply, Qt::LeftButton);
        QVERIFY(window.controller().project.show.markers[0].message == std::vector<unsigned char>({0xc0, 127}));
        type->setCurrentIndex(8); window.findChild<QLineEdit*>("midiRawBytes")->setText("F0 7D 01");
        QTest::mouseClick(apply, Qt::LeftButton);
        QVERIFY(window.controller().project.show.markers[0].message == std::vector<unsigned char>({0xc0, 127}));
        QVERIFY(window.controller().status.contains("F7"));
        window.controller().undo(); QVERIFY(window.controller().project.show.markers[0].message == std::vector<unsigned char>({0xc0, 0}));
        window.controller().dirty = false;
    }
    void routingPresetsApplyMonoAtFullLevelAndUndo() {
        MainWindow window; window.show();
        auto* timeline = window.findChild<Timeline*>(); timeline->selectedTrack = 4;
        window.controller().setShowMode(false); // Refresh the inspector for the selected Guide track.
        auto* preset = window.findChild<QComboBox*>("trackRoutingPreset"); QVERIFY(preset);
        QPushButton* apply = nullptr;
        for (auto* b : window.findChildren<QPushButton*>()) if (b->text() == "Apply track") apply = b;
        QVERIFY(apply);
        preset->setCurrentIndex(preset->findData(3));
        QVERIFY(QMetaObject::invokeMethod(preset, "activated", Q_ARG(int, preset->currentIndex())));
        QTest::mouseClick(apply, Qt::LeftButton);
        const auto& guide = window.controller().project.show.tracks[4];
        QCOMPARE(guide.left, 2); QCOMPARE(guide.right, -1); QCOMPARE(guide.pan, -1.);
        window.controller().undo(); QCOMPARE(window.controller().project.show.tracks[4].left, -1);
        QCOMPARE(window.controller().project.show.tracks[4].right, -1);
        window.findChild<QSpinBox*>("trackLeftOutput")->setValue(128);
        window.findChild<QSpinBox*>("trackRightOutput")->setValue(129);
        QTest::mouseClick(apply, Qt::LeftButton);
        QCOMPARE(window.controller().project.show.tracks[4].left, 127);
        QCOMPARE(window.controller().project.show.tracks[4].right, 128);
        window.controller().dirty = false;
    }
    void unreadableCacheBlocksAutomaticAudioPreparation() {
        Controller c; auto project = newProject();
        MediaAsset asset; asset.id = "broken-cache"; asset.name = "Broken cache"; asset.duration = 2;
        auto audio = std::make_shared<AudioData>(); audio->storage = std::make_shared<UnreadableStorage>(); asset.audio = audio;
        project.assets.insert(asset.id, asset);
        Clip clip; clip.id = "clip"; clip.assetId = asset.id.toStdString(); clip.track = 2; clip.duration = frames(2);
        project.show.clips.push_back(clip); bindAudio(project); c.replace(project);
        QVERIFY(c.transport.audioReadFailed());
        c.toggle(); QCOMPARE(c.transport.state(), State::Stopped); QVERIFY(!c.device.running());
        QVERIFY(c.status.contains("unreadable audio cache")); QCOMPARE(c.playbackProblem, c.status);
        c.setShowMode(true); c.toggle(); QCOMPARE(c.transport.state(), State::Stopped); QVERIFY(!c.device.running());
    }
    void missingRememberedDevicesAreVisibleInOutputs() {
        QSettings().setValue("audio/device", "SimpleAVShow absent audio test destination");
        QSettings().setValue("audio/channels", 128);
        QSettings().setValue("midi/port", "SimpleAVShow absent MIDI test destination");
        MainWindow window; window.show(); bool audioMissing = false, midiMissing = false;
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = window.findChild<QDialog*>("outputSettingsDialog"); if (!dialog) return;
            auto* audio = dialog->findChild<QComboBox*>("audioOutputDevice");
            auto* midi = dialog->findChild<QComboBox*>("midiOutputDevice");
            auto* apply = dialog->findChild<QPushButton*>("connectAudioOutput");
            auto* channels = dialog->findChild<QSpinBox*>("audioOutputChannels");
            audioMissing = audio && audio->currentText().contains("absent audio test destination (missing)") && apply && !apply->isEnabled() && channels && channels->value() == 128;
            midiMissing = midi && midi->currentText().contains("absent MIDI test destination (missing)");
            dialog->reject();
        });
        window.findChild<QPushButton*>("outputSettingsButton")->click();
        QVERIFY(audioMissing); QVERIFY(midiMissing);
    }
    void demoSaveReloadAndDecodedAudio() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        Controller c; c.replace(demoProject()); c.save(dir.filePath("opening.savshow"));
        QVERIFY2(!c.path.isEmpty(), qPrintable(c.status));
        const Project loaded = readProject(c.path);
        QCOMPARE(loaded.title, c.project.title); QCOMPARE(loaded.show.clips.size(), c.project.show.clips.size());
        QCOMPARE(loaded.assets.size(), c.project.assets.size()); QCOMPARE(loaded.width, 1920); QCOMPARE(loaded.fps, 24.);
        for (const auto& a : loaded.assets) {
            QVERIFY(QFileInfo::exists(a.path)); QCOMPARE(a.name, c.project.assets[a.id].name);
            if (a.audio) {
                const auto expected = c.project.assets[a.id].audio;
                QCOMPARE(a.audio->mono, expected->mono);
                QCOMPARE(a.audio->length(), expected->length());
                for (Frame i : {Frame{10}, Frame{200}, Frame{1000}}) QVERIFY(std::abs(sampleAt(*a.audio, i) - sampleAt(*expected, i)) < .0001);
            }
        }
        // Relative paths remain usable after moving the entire show folder.
        const QString moved = dir.path() + "-moved";
        QVERIFY(QDir().rename(dir.path(), moved));
        auto restored = readProject(moved + "/opening.savshow"); QCOMPARE(restored.show.markers.size(), std::size_t{3});
        QVERIFY(QDir(moved).removeRecursively());
    }
    void malformedProjectsAreRejected() {
        QTemporaryDir dir; Controller c; c.replace(demoProject()); const QString path = dir.filePath("show.savshow"); c.save(path);
        QFile input(path); QVERIFY(input.open(QIODevice::ReadOnly)); auto root = QJsonDocument::fromJson(input.readAll()).object(); input.close();
        auto clips = root["clips"].toArray(); auto first = clips[0].toObject(); first["start"] = -20; clips[0] = first; root["clips"] = clips;
        QFile broken(dir.filePath("broken.savshow")); QVERIFY(broken.open(QIODevice::WriteOnly)); broken.write(QJsonDocument(root).toJson()); broken.close();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readProject(broken.fileName()));
        auto invalid = demoProject(); invalid.show.tracks[2].left = -2; QVERIFY_THROWS_EXCEPTION(std::runtime_error, validate(invalid));
        invalid = demoProject(); invalid.show.clips.push_back(invalid.show.clips[0]); invalid.show.clips.back().id = newId().toStdString();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validate(invalid));
    }
    void largeProjectsRoundTripWithoutPrototypeCaps() {
        QTemporaryDir dir; auto p = newProject(); p.show.duration = frames(48 * 3600);
        for (int i = 0; i < 70; ++i) p.show.tracks.push_back({newId().toStdString(), "Layer", true});
        p.show.tracks[2].left = 127;
        for (int i = 0; i < 300; ++i) {
            Clip c; c.id = newId().toStdString(); c.text = "Cue"; c.track = 0; c.start = frames(i * 2); c.duration = frames(1);
            for (int k = 0; k < 300; ++k) c.alpha.points.push_back({k, k / 300.});
            p.show.clips.push_back(c);
            MediaAsset a; a.id = newId(); a.name = "Still"; a.image = true; a.path = dir.filePath(a.id + ".png");
            a.still = QImage(1, 1, QImage::Format_RGB32); a.still.fill(Qt::black); QVERIFY(a.still.save(a.path));
            p.assets.insert(a.id, a);
        }
        for (int i = 0; i < 700; ++i) p.show.markers.push_back({newId().toStdString(), "Cue", frames(i), false});
        const QString path = dir.filePath("large.savshow"); writeProject(p, path); const auto loaded = readProject(path);
        QCOMPARE(loaded.show.duration, p.show.duration); QCOMPARE(loaded.show.tracks.size(), p.show.tracks.size());
        QCOMPARE(loaded.show.clips.size(), std::size_t{300}); QCOMPARE(loaded.assets.size(), 300);
        QCOMPARE(loaded.show.markers.size(), std::size_t{700}); QCOMPARE(loaded.show.clips[0].alpha.points.size(), std::size_t{300});
        QCOMPARE(loaded.show.tracks[2].left, 127);
        p.show.clips[0].start = p.show.duration; p.show.clips[0].duration = std::numeric_limits<Frame>::max();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validate(p));
    }
    void midiMessagesValidateLengthsAndSysex() {
        QVERIFY(validMidi({0x90, 60, 100})); QVERIFY(validMidi({0xc2, 10})); QVERIFY(validMidi({0xf0, 0x7d, 1, 0xf7}));
        QVERIFY(validMidi({0xfa})); QVERIFY(validMidi({0xf2, 0, 2}));
        QVERIFY(!validMidi({60, 100})); QVERIFY(!validMidi({0x90, 60})); QVERIFY(!validMidi({0xc0, 1, 2}));
        QVERIFY(!validMidi({0xf0, 1})); QVERIFY(!validMidi({0xf0, 0x90, 0xf7})); QVERIFY(!validMidi({0x90, 128, 1}));
    }
    void editingIsTransactionalAndUndoWorks() {
        Controller c; const QString original = c.project.title;
        QVERIFY(c.edit([](Project& p) { p.title = "Act two"; })); QCOMPARE(c.project.title, "Act two");
        QVERIFY(!c.edit([](Project& p) { p.width = -1; })); QCOMPARE(c.project.width, 1920); QCOMPARE(c.project.title, "Act two");
        c.undo(); QCOMPARE(c.project.title, original); c.redo(); QCOMPARE(c.project.title, "Act two");
        c.setShowMode(true); QVERIFY(!c.edit([](Project& p) { p.title = "Accidental edit"; })); QCOMPARE(c.project.title, "Act two");
        c.seek(frames(3)); QCOMPARE(c.position(), Frame{0}); c.setShowMode(false); c.seek(frames(3)); QCOMPARE(c.position(), frames(3));
    }
    void guiPlacesTextCueAndLocksEdits() {
        MainWindow window; window.show(); QTest::qWait(40);
        auto buttons = window.findChildren<QPushButton*>();
        auto find = [&](QString text) -> QPushButton* { for (auto* b : buttons) if (b->text() == text) return b; return nullptr; };
        QVERIFY(find("+ Text")); QTest::mouseClick(find("+ Text"), Qt::LeftButton); QCOMPARE(window.controller().project.show.clips.size(), std::size_t{1});
        QVERIFY(find("+ Hold / MIDI cue")); QTest::mouseClick(find("+ Hold / MIDI cue"), Qt::LeftButton); QCOMPARE(window.controller().project.show.markers.size(), std::size_t{1});
        // A show requiring an unchosen MIDI destination must stay stopped, including in Show mode.
        QVERIFY(window.controller().edit([](Project& p) { p.show.markers[0].message = {0xc0, 0}; }));
        QVERIFY(find("Show mode")); QTest::mouseClick(find("Show mode"), Qt::LeftButton); QVERIFY(window.controller().showMode);
        QTest::mouseClick(find("+ Text"), Qt::LeftButton); QCOMPARE(window.controller().project.show.clips.size(), std::size_t{1});
        // Play reports the missing MIDI destination before opening audio.
        QTest::keyClick(&window, Qt::Key_Space); QCOMPARE(window.controller().transport.state(), State::Stopped);
        window.controller().dirty = false;
    }
    void outputsOpenAtMarkerHold_data() {
        QTest::addColumn<int>("entry");
        QTest::addColumn<bool>("async");
        QTest::newRow("Outputs button with async") << 0 << true;
        QTest::newRow("Open output button with async") << 1 << true;
        QTest::newRow("Project menu with async") << 2 << true;
        QTest::newRow("Outputs button without async") << 0 << false;
    }
    void outputsOpenAtMarkerHold() {
        QFETCH(int, entry); QFETCH(bool, async);
        MainWindow window; auto& controller = window.controller();
        auto project = async ? demoProject() : newProject();
        project.show.markers = {{"hold", "Wait", 0, true, MidiTiming::Arrival, {}}};
        controller.replace(std::move(project)); window.show();
        QVERIFY(controller.transport.command({Action::Play})); controller.transport.render(nullptr, 1, 0);
        QCOMPARE(controller.transport.state(), State::Held);
        QCOMPARE(controller.transport.asyncVoices(), async ? 1 : 0);

        bool opened = false, locked = false, playbackPreserved = false, unlockedAfterStop = false;
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = window.findChild<QDialog*>("outputSettingsDialog");
            if (!dialog) return;
            opened = dialog->isVisible();
            auto* audio = dialog->findChild<QPushButton*>("connectAudioOutput");
            auto* midi = dialog->findChild<QPushButton*>("connectMidiOutput");
            auto* display = dialog->findChild<QPushButton*>("openProgramDisplay");
            auto* stop = dialog->findChild<QPushButton*>("stopForOutputSettings");
            auto* explanation = dialog->findChild<QLabel*>("outputConnectionStatus");
            locked = audio && midi && display && stop && explanation && !audio->isEnabled() && !midi->isEnabled() &&
                !midi->toolTip().isEmpty() && display->isEnabled() && stop->isVisible() && explanation->text().contains("stop marker");
            controller.transport.render(nullptr, 1, 0);
            playbackPreserved = controller.transport.state() == State::Held && controller.position() == 0 &&
                controller.transport.asyncVoices() == (async ? 1 : 0);
            if (stop) {
                QTest::mouseClick(stop, Qt::LeftButton);
                unlockedAfterStop = controller.transport.state() == State::Stopped && controller.transport.asyncVoices() == 0 &&
                    midi && midi->isEnabled() && stop->isHidden();
            }
            dialog->reject();
        });
        if (entry == 2) {
            auto* action = window.findChild<QAction*>("outputSettingsAction"); QVERIFY(action); QVERIFY(action->isEnabled()); action->trigger();
        } else {
            auto* button = window.findChild<QPushButton*>(entry == 0 ? "outputSettingsButton" : "openOutputSettingsButton");
            QVERIFY(button); QVERIFY(button->isEnabled()); QTest::mouseClick(button, Qt::LeftButton);
        }
        QVERIFY(opened); QVERIFY(locked); QVERIFY(playbackPreserved); QVERIFY(unlockedAfterStop);
    }
    void outputEntryPointsShowWhenUnavailable() {
        MainWindow window; window.show();
        auto* outputs = window.findChild<QPushButton*>("outputSettingsButton");
        auto* openOutput = window.findChild<QPushButton*>("openOutputSettingsButton");
        auto* action = window.findChild<QAction*>("outputSettingsAction");
        QVERIFY(outputs); QVERIFY(openOutput); QVERIFY(action);
        window.controller().setShowMode(true);
        QVERIFY(!outputs->isEnabled()); QVERIFY(!openOutput->isEnabled()); QVERIFY(!action->isEnabled());
        QVERIFY(outputs->toolTip().contains("show mode")); QVERIFY(openOutput->toolTip().contains("show mode"));
        window.controller().setShowMode(false);
        QVERIFY(outputs->isEnabled()); QVERIFY(openOutput->isEnabled()); QVERIFY(action->isEnabled());
        window.controller().busy = true;
        QTRY_VERIFY(!outputs->isEnabled() && !openOutput->isEnabled() && !action->isEnabled());
        QVERIFY(outputs->toolTip().contains("media preparation"));
        window.controller().busy = false;
        QTRY_VERIFY(outputs->isEnabled() && openOutput->isEnabled() && action->isEnabled());
    }
    void videoDecodeSeekAndAudioResample() {
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg"); if (ffmpeg.isEmpty()) QSKIP("ffmpeg CLI is unavailable for fixture generation");
        QTemporaryDir dir; const QString file = dir.filePath("two-colors.mkv");
        QProcess process; process.start(ffmpeg, {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "color=red:s=320x180:r=24:d=1", "-f", "lavfi", "-i", "color=blue:s=320x180:r=24:d=1", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=2", "-filter_complex", "[0:v][1:v]concat=n=2:v=1:a=0[v]", "-map", "[v]", "-map", "2:a", "-c:v", "ffv1", "-c:a", "pcm_s16le", file});
        QVERIFY(process.waitForFinished(15000)); QCOMPARE(process.exitCode(), 0);
        const auto asset = loadMedia(file); QVERIFY(asset.video); QVERIFY(asset.audio); QCOMPARE(asset.width, 320); QCOMPARE(asset.audio->length(), frames(2));
        QVERIFY(asset.audio->samples.empty()); QVERIFY(asset.audio->storage);
        VideoReader reader(file); QImage image;
        QTRY_VERIFY_WITH_TIMEOUT(!(image = reader.imageAt(.5)).isNull(), 3000);
        QVERIFY(image.pixelColor(100, 100).red() > 200); QVERIFY(image.pixelColor(100, 100).blue() < 30);
        QTRY_VERIFY_WITH_TIMEOUT(!(image = reader.imageAt(1.5)).isNull(), 3000);
        QVERIFY(image.pixelColor(100, 100).blue() > 200); QVERIFY(image.pixelColor(100, 100).red() < 30);
        QTRY_VERIFY_WITH_TIMEOUT(!(image = reader.imageAt(.2)).isNull(), 3000);
        QVERIFY(image.pixelColor(100, 100).red() > 200); QVERIFY(reader.error().isEmpty());
    }
    void diskAudioCacheHasBoundedMemoryAndSharedLifetime() {
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg"); if (ffmpeg.isEmpty()) QSKIP("ffmpeg fixture generation unavailable");
        QTemporaryDir dir; const QString file = dir.filePath("long-silence.flac"); QProcess process;
        process.start(ffmpeg, {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "anullsrc=r=8000:cl=mono", "-t", "2800", "-c:a", "flac", file});
        QVERIFY(process.waitForFinished(15000)); QCOMPARE(process.exitCode(), 0);
        QDir caches(QDir::tempPath()); const auto before = caches.entryList({"simpleavshow-pcm-*"}, QDir::Files);
        std::shared_ptr<const AudioData> retained;
        QString cache;
        {
            const auto asset = loadMedia(file); retained = asset.audio;
            QVERIFY(retained); QVERIFY(retained->storage); QVERIFY(retained->samples.empty());
            QCOMPARE(retained->length(), frames(2800));
            QVERIFY(retained->peaks.size() * sizeof(float) < 2 * 1024 * 1024);
            auto created = caches.entryList({"simpleavshow-pcm-*"}, QDir::Files);
            for (const auto& name : before) created.removeAll(name);
            QCOMPARE(created.size(), 1); cache = caches.filePath(created.front());
            QVERIFY(QFileInfo(cache).size() > 1024ll * 1024 * 1024);
            // The immutable prepared source survives moves/deletion of the original,
            // and snapshots keep it alive after releasing the importing asset.
            QVERIFY(QFile::remove(file)); QCOMPARE(sampleAt(*retained, retained->length() - 1), 0.f);
        }
        QVERIFY(QFileInfo::exists(cache)); retained.reset(); QVERIFY(!QFileInfo::exists(cache));
        // Failed imports must release partial caches as well.
        QFile broken(dir.filePath("broken.wav")); QVERIFY(broken.open(QIODevice::WriteOnly)); broken.write("broken"); broken.close();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, loadMedia(broken.fileName()));
        QCOMPARE(caches.entryList({"simpleavshow-pcm-*"}, QDir::Files), before);
    }
    void videoScalingKeepsTheEntireFrame_data() {
        QTest::addColumn<QSize>("sourceSize");
        QTest::addColumn<QSize>("maximum");
        QTest::addColumn<QSize>("expectedSize");
        QTest::newRow("4K to 1080p") << QSize(3840, 2160) << QSize(1920, 1080) << QSize(1920, 1080);
        QTest::newRow("width limited") << QSize(640, 360) << QSize(320, 360) << QSize(320, 180);
        QTest::newRow("height limited") << QSize(640, 360) << QSize(640, 180) << QSize(320, 180);
        QTest::newRow("native size") << QSize(320, 180) << QSize(1920, 1080) << QSize(320, 180);
    }
    void videoScalingKeepsTheEntireFrame() {
        QFETCH(QSize, sourceSize); QFETCH(QSize, maximum); QFETCH(QSize, expectedSize);
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg"); if (ffmpeg.isEmpty()) QSKIP("ffmpeg CLI is unavailable for fixture generation");
        QTemporaryDir dir; QVERIFY(dir.isValid()); const QString file = dir.filePath("top-red-bottom-blue.mkv");
        QProcess process;
        process.start(ffmpeg, {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
            QString("color=red:s=%1x%2:r=2:d=1,drawbox=x=0:y=ih/2:w=iw:h=ih/2:color=blue:t=fill").arg(sourceSize.width()).arg(sourceSize.height()),
            "-c:v", "ffv1", file});
        QVERIFY(process.waitForFinished(15000)); QCOMPARE(process.exitCode(), 0);
        VideoReader reader(file, maximum); QImage image;
        for (double time : {0., .5, 0.}) {
            QTRY_VERIFY_WITH_TIMEOUT(!(image = reader.imageAt(time)).isNull(), 3000);
            QCOMPARE(image.size(), expectedSize);
            for (int x : {image.width() / 4, 3 * image.width() / 4}) {
                const QColor top = image.pixelColor(x, image.height() / 4);
                const QColor bottom = image.pixelColor(x, 3 * image.height() / 4);
                QVERIFY(top.red() > 200 && top.blue() < 30); QCOMPARE(top.alpha(), 255);
                QVERIFY(bottom.blue() > 200 && bottom.red() < 30); QCOMPARE(bottom.alpha(), 255);
            }
        }
        QVERIFY(reader.error().isEmpty());
    }
    void embeddedAudioKeepsItsContainerOffset() {
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg"); if (ffmpeg.isEmpty()) QSKIP("ffmpeg fixture generation unavailable");
        QTemporaryDir dir; const QString file = dir.filePath("offset.mkv"); QProcess process;
        process.start(ffmpeg, {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "color=red:s=160x90:r=24:d=2", "-itsoffset", "0.5", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=1", "-map", "0:v", "-map", "1:a", "-c:v", "ffv1", "-c:a", "pcm_s16le", file});
        QVERIFY(process.waitForFinished(15000)); QCOMPARE(process.exitCode(), 0);
        const auto asset = loadMedia(file); QVERIFY(asset.audio); QCOMPARE(asset.audio->length(), frames(1.5));
        for (Frame i = 0; i < frames(.5); ++i) QCOMPARE(sampleAt(*asset.audio, i), 0.f);
        float peak = 0; for (Frame i = frames(.5); i < frames(.6); ++i) peak = std::max(peak, std::abs(sampleAt(*asset.audio, i))); QVERIFY(peak > .1);
        auto show = std::make_shared<Show>(); show->duration = asset.audio->length();
        show->tracks.push_back({"audio", "Audio", false, false, false, 1, -1, 0, -1});
        Clip clip; clip.audio = asset.audio; clip.duration = show->duration; show->clips.push_back(clip);
        Transport transport; transport.configure(show); QVERIFY(transport.command({Action::Play}));
        std::vector<float> out(static_cast<std::size_t>(frames(.6)) * 2); transport.render(out.data(), static_cast<unsigned>(frames(.6)), 2);
        QCOMPARE(transport.bufferWaits(), 0u);
        for (Frame i = 0; i < frames(.6); ++i) {
            QCOMPARE(out[i * 2], sampleAt(*asset.audio, i)); QCOMPARE(out[i * 2 + 1], 0.f);
        }
    }
};
QTEST_MAIN(AppTests)
#include "app_tests.moc"
