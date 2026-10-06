#include "app/MainWindow.h"
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

using namespace sav;
class AppTests : public QObject {
    Q_OBJECT
private slots:
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
                for (Frame i : {Frame{10}, Frame{200}, Frame{1000}}) QVERIFY(std::abs(a.audio->samples[i * 2] - expected->samples[i * 2]) < .0001);
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
        auto invalid = demoProject(); invalid.show.tracks[2].left = 64; QVERIFY_THROWS_EXCEPTION(std::runtime_error, validate(invalid));
        invalid = demoProject(); invalid.show.clips.push_back(invalid.show.clips[0]); invalid.show.clips.back().id = newId().toStdString();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validate(invalid));
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
        QVERIFY(find("Show mode")); QTest::mouseClick(find("Show mode"), Qt::LeftButton); QVERIFY(window.controller().showMode);
        QTest::mouseClick(find("+ Text"), Qt::LeftButton); QCOMPARE(window.controller().project.show.clips.size(), std::size_t{1});
        // Space does not silently run a show without a connected audio device.
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
        const auto asset = loadMedia(file, audioBudget); QVERIFY(asset.video); QVERIFY(asset.audio); QCOMPARE(asset.width, 320); QCOMPARE(asset.audio->length(), frames(2));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, loadMedia(file, 100));
        VideoReader reader(file); QImage image;
        QTRY_VERIFY_WITH_TIMEOUT(!(image = reader.imageAt(.5)).isNull(), 3000);
        QVERIFY(image.pixelColor(100, 100).red() > 200); QVERIFY(image.pixelColor(100, 100).blue() < 30);
        QTRY_VERIFY_WITH_TIMEOUT(!(image = reader.imageAt(1.5)).isNull(), 3000);
        QVERIFY(image.pixelColor(100, 100).blue() > 200); QVERIFY(image.pixelColor(100, 100).red() < 30);
        QTRY_VERIFY_WITH_TIMEOUT(!(image = reader.imageAt(.2)).isNull(), 3000);
        QVERIFY(image.pixelColor(100, 100).red() > 200); QVERIFY(reader.error().isEmpty());
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
        const auto asset = loadMedia(file, audioBudget); QVERIFY(asset.audio); QCOMPARE(asset.audio->length(), frames(1.5));
        for (Frame i = 0; i < frames(.5); ++i) QCOMPARE(asset.audio->samples[i * 2], 0.f);
        float peak = 0; for (Frame i = frames(.5); i < frames(.6); ++i) peak = std::max(peak, std::abs(asset.audio->samples[i * 2])); QVERIFY(peak > .1);
    }
};
QTEST_MAIN(AppTests)
#include "app_tests.moc"
