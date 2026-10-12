#include "app/MainWindow.h"
#include "app/MidiMessageEditor.h"
#include "app/Project.h"
#include "media/Media.h"
#include "media/AudioSources.h"
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
#include <QMessageBox>
#include <QLabel>
#include <QAction>
#include <QTimer>
#include <QComboBox>
#include <QSpinBox>
#include <QLineEdit>
#include <QSettings>
#include <QCheckBox>
#include <QFileDialog>
#include <archive.h>
#include <archive_entry.h>
#include <limits>
#include <QStandardPaths>
#include <QUuid>
#include <QtEndian>
#include <filesystem>
#include <future>

using namespace sav;
namespace {
// Exact binary fractions make PCM conversion, stereo ordering and mono gain
// independently checkable without relying on FFmpeg to generate expectations.
float waveValue(Frame frame, int channel) {
    static constexpr float values[] = {-1.f, -.5f, -.25f, 0.f, .25f, .5f, .75f};
    return values[(frame + channel * 3) % 7];
}
QByteArray waveBytes(int bits, int type, int channels, int rate, bool extensible = false, Frame count = 17003) {
    QByteArray bytes; QDataStream out(&bytes, QIODevice::WriteOnly); out.setByteOrder(QDataStream::LittleEndian);
    out.setFloatingPointPrecision(QDataStream::SinglePrecision);
    const quint32 payload = static_cast<quint32>(count * channels * (bits / 8));
    out.writeRawData("RIFF", 4); out << quint32(0); out.writeRawData("WAVEJUNK", 8);
    out << quint32(1); out.writeRawData("x\0", 2); // Exercise odd-sized chunk padding.
    out.writeRawData("fmt ", 4); out << quint32(extensible ? 40 : 16) << quint16(extensible ? 0xfffe : type) << quint16(channels);
    out << quint32(rate) << quint32(rate * channels * (bits / 8)) << quint16(channels * (bits / 8)) << quint16(bits);
    if (extensible) {
        out << quint16(22) << quint16(bits) << quint32(channels == 1 ? 4 : 3) << quint32(type) << quint16(0) << quint16(0x10);
        const char tail[] = {char(0x80), 0, 0, char(0xaa), 0, 0x38, char(0x9b), 0x71}; out.writeRawData(tail, 8);
    }
    out.writeRawData("data", 4); out << payload;
    for (Frame i = 0; i < count; ++i) for (int channel = 0; channel < channels; ++channel) {
        const auto value = waveValue(i, channel);
        if (type == 3) out << value;
        else if (bits == 8) out << quint8(value * 128 + 128);
        else if (bits == 16) out << qint16(value * 32768);
        else if (bits == 24) {
            const auto sample = static_cast<quint32>(static_cast<qint32>(value * 8388608));
            out << quint8(sample) << quint8(sample >> 8) << quint8(sample >> 16);
        } else out << static_cast<qint32>(static_cast<double>(value) * 2147483648.);
    }
    if (payload & 1) out << quint8(0);
    qToLittleEndian<quint32>(static_cast<quint32>(bytes.size() - 8), bytes.data() + 4);
    return bytes;
}
void writeBytes(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) throw std::runtime_error("Cannot write audio fixture");
}
struct ZipEntry { QString name; QByteArray data; bool symlink = false; };
QByteArray zipBytes(const std::vector<ZipEntry>& entries, bool zip64 = false, bool compressed = false) {
    qsizetype capacity = 4096;
    for (const auto& e : entries) capacity += e.data.size() + e.name.size() * 4 + 1024;
    QByteArray bytes(capacity, '\0'); size_t used = 0;
    std::unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(), archive_write_free);
    auto check = [&](int code) { if (code != ARCHIVE_OK) throw std::runtime_error(archive_error_string(writer.get())); };
    check(archive_write_set_format_zip(writer.get()));
    if (!compressed) check(archive_write_zip_set_compression_store(writer.get()));
    if (zip64) check(archive_write_set_options(writer.get(), "zip:zip64"));
    check(archive_write_open_memory(writer.get(), bytes.data(), static_cast<size_t>(bytes.size()), &used));
    for (const auto& value : entries) {
        std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
        archive_entry_set_pathname(entry.get(), value.name.toUtf8().constData());
        archive_entry_set_filetype(entry.get(), value.symlink ? AE_IFLNK : AE_IFREG);
        archive_entry_set_perm(entry.get(), 0644);
        if (value.symlink) archive_entry_set_symlink(entry.get(), "../../outside");
        archive_entry_set_size(entry.get(), value.symlink ? 0 : value.data.size());
        check(archive_write_header(writer.get(), entry.get()));
        if (!value.symlink && archive_write_data(writer.get(), value.data.constData(), value.data.size()) != value.data.size())
            throw std::runtime_error("Could not write ZIP fixture");
        check(archive_write_finish_entry(writer.get()));
    }
    check(archive_write_close(writer.get())); bytes.resize(static_cast<qsizetype>(used)); return bytes;
}
QMap<QString, QByteArray> zipEntries(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Could not open ZIP fixture");
    const auto bytes = file.readAll();
    std::unique_ptr<archive, decltype(&archive_read_free)> reader(archive_read_new(), archive_read_free);
    auto check = [&](int code) { if (code != ARCHIVE_OK) throw std::runtime_error(archive_error_string(reader.get())); };
    check(archive_read_support_format_zip(reader.get()));
    check(archive_read_open_memory(reader.get(), bytes.constData(), static_cast<size_t>(bytes.size())));
    QMap<QString, QByteArray> entries; archive_entry* entry = nullptr;
    for (;;) {
        const auto result = archive_read_next_header(reader.get(), &entry);
        if (result == ARCHIVE_EOF) break;
        check(result);
        QByteArray data(static_cast<qsizetype>(archive_entry_size(entry)), '\0');
        const auto count = archive_read_data(reader.get(), data.data(), static_cast<size_t>(data.size()));
        if (count != data.size()) throw std::runtime_error("Incomplete ZIP fixture");
        entries.insert(QString::fromUtf8(archive_entry_pathname(entry)), data);
    }
    return entries;
}
float sampleAt(const AudioData& audio, Frame frame) {
    if (!audio.storage) return audio.samples[frame * 2];
    float stereo[2]; audio.storage->read(frame, stereo, 1); return stereo[0];
}
class UnreadableStorage : public AudioStorage {
public:
    Frame length() const noexcept override { return frames(2); }
    void read(Frame, float*, std::size_t) const override { throw std::runtime_error("Cache is unavailable"); }
};
// A valid manifest referencing one image under the exporter's UUID naming.
struct PackageFixture { QByteArray manifest, image; QString name; };
PackageFixture packageFixture(const QString& directory) {
    PackageFixture f; f.name = "media/" + newId() + ".png";
    QImage image(4, 4, QImage::Format_RGB32); image.fill(Qt::red);
    if (!QDir().mkpath(directory + "/media") || !image.save(directory + "/" + f.name)) throw std::runtime_error("Cannot write package fixture");
    Project p = newProject(); auto asset = loadMedia(directory + "/" + f.name); p.assets.insert(asset.id, asset);
    writeProject(p, directory + "/project.savshow");
    QFile manifest(directory + "/project.savshow"), source(directory + "/" + f.name);
    if (!manifest.open(QIODevice::ReadOnly) || !source.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read package fixture");
    f.manifest = manifest.readAll(); f.image = source.readAll(); return f;
}
QString openFailure(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return "Cannot write fixture";
    file.close();
    try { readProject(path); } catch (const std::runtime_error& e) { return QString::fromUtf8(e.what()); }
    return {};
}
}
class AppTests : public QObject {
    Q_OBJECT
    QTemporaryDir settingsDir_;
private slots:
    void initTestCase() {
        QVERIFY(settingsDir_.isValid());
        QCoreApplication::setOrganizationName("SimpleAVShowTests");
        QCoreApplication::setApplicationName("PlaybackUX-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
        QStandardPaths::setTestModeEnabled(true);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir_.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDir_.path());
    }
    void cleanupTestCase() {
        QVERIFY(QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).removeRecursively());
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
    void quittingDuringShowModeRequiresConfirmation() {
        MainWindow window; window.show(); window.controller().setShowMode(true);
        bool asked = false;
        QTimer::singleShot(0, &window, [&] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()); if (!box) return;
            asked = true; QCOMPARE(box->defaultButton(), box->button(QMessageBox::Cancel)); box->button(QMessageBox::Cancel)->click();
        });
        QVERIFY(!window.close()); QVERIFY(asked); QVERIFY(window.isVisible());
        QTimer::singleShot(0, &window, [&] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()); if (!box) return;
            for (auto* button : box->buttons()) if (button->text() == "Stop and quit") button->click();
        });
        window.controller().dirty = false; QVERIFY(window.close());
    }
    void onlyTimelineVideosCanBlockPlayback() {
        Controller c; auto project = newProject();
        MediaAsset unused; unused.id = "unused"; unused.name = "Unused"; unused.video = true; unused.duration = 1; unused.path = "/nonexistent/unused.mov";
        project.assets.insert(unused.id, unused); c.replace(project);
        QVERIFY(c.missingVideo().isEmpty());
        QVERIFY(c.edit([](Project& p) { Clip clip; clip.id = "v"; clip.assetId = "unused"; clip.track = 0; clip.duration = frames(1); p.show.clips.push_back(clip); }));
        QCOMPARE(c.missingVideo(), QString("/nonexistent/unused.mov"));
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
        QCOMPARE(guide.left, 2); QCOMPARE(guide.right, -1); QCOMPARE(guide.pan, 0.);
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
        QVERIFY(c.status.contains("Audio source is unreadable")); QCOMPARE(c.playbackProblem, c.status);
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
    void packageCollectsMediaAndSurvivesMissingOriginals() {
        QTemporaryDir originals, destination;
        Controller c; c.replace(demoProject()); c.save(originals.filePath("opening.savshow"));
        QVERIFY2(!c.path.isEmpty(), qPrintable(c.status));
        // Unused library items, same-name files, Unicode paths, and duplicate source references.
        for (const auto& folder : {QString::fromUtf8("Bühne 一"), QString("second")}) {
            QVERIFY(QDir().mkpath(originals.filePath(folder)));
            const auto source = originals.filePath(folder + "/picture.png");
            QImage image(4, 4, QImage::Format_RGB32); image.fill(folder == "second" ? Qt::blue : Qt::red);
            QVERIFY(image.save(source)); auto asset = loadMedia(source); c.project.assets.insert(asset.id, asset);
            if (folder == "second") { asset.id = newId(); c.project.assets.insert(asset.id, asset); }
        }
        c.dirty = true;
        const Project expected = c.project;
        const auto originalPath = c.path;
        const auto package = destination.filePath(QString::fromUtf8("Première 一.savpack"));
        writeProjectPackage(c.project, package);
        QCOMPARE(c.path, originalPath); QVERIFY(c.dirty);
        for (const auto& asset : c.project.assets) QCOMPARE(asset.path, expected.assets[asset.id].path);
        const auto entries = zipEntries(package);
        QCOMPARE(entries.size(), c.project.assets.size()); // Manifest + unique media (one alias).
        const auto root = QJsonDocument::fromJson(entries["project.savshow"]).object();
        QCOMPARE(root["version"].toInt(), 1);
        for (const auto& value : root["assets"].toArray()) {
            const auto asset = value.toObject(); const auto source = asset["path"].toString();
            QVERIFY(source.startsWith("media/")); QVERIFY(!source.contains("..")); QVERIFY(entries.contains(source));
            QFile original(expected.assets[asset["id"].toString()].path); QVERIFY(original.open(QIODevice::ReadOnly));
            QCOMPARE(entries[source], original.readAll());
        }
        QVERIFY(QDir(originals.path()).removeRecursively());
        const auto moved = destination.filePath("moved.SAVPACK"); QVERIFY(QFile::rename(package, moved));
        auto loaded = readProject(moved);
        QCOMPARE(loaded.assets.size(), expected.assets.size()); QCOMPARE(loaded.show.clips.size(), expected.show.clips.size());
        QCOMPARE(loaded.show.markers.size(), expected.show.markers.size()); QCOMPARE(loaded.title, expected.title);
        QVERIFY(loaded.packageDirectory);
        for (const auto& asset : loaded.assets) {
            QVERIFY(QFileInfo::exists(asset.path)); QCOMPARE(asset.name, expected.assets[asset.id].name);
            if (asset.audio) QCOMPARE(asset.audio->length(), expected.assets[asset.id].audio->length());
        }
        const auto temporary = loaded.packageDirectory->path();
        { Project snapshot = loaded; loaded = newProject(); QVERIFY(QFileInfo::exists(temporary)); }
        QVERIFY(!QFileInfo::exists(temporary));
    }
    void generatedPackageCanBeOpenedEditedAndSavedAsProject() {
        QTemporaryDir dir;
        Project generated = demoProject(); const auto package = dir.filePath("demo.savpack");
        writeProjectPackage(generated, package);
        for (const auto& asset : generated.assets) QVERIFY(asset.path.isEmpty());
        MainWindow window; window.openPath(package); QTRY_VERIFY_WITH_TIMEOUT(!window.controller().busy, 15000);
        auto& c = window.controller();
        QCOMPARE(c.project.title, generated.title); QVERIFY(c.path.isEmpty()); QVERIFY(c.dirty);
        QVERIFY(c.project.packageDirectory);
        const auto temporary = c.project.packageDirectory->path();
        QVERIFY(c.edit([](Project& p) { p.title = "An edited copy"; }));
        c.save(dir.filePath("edited.savshow")); QVERIFY2(!c.dirty, qPrintable(c.status));
        QVERIFY(!c.project.packageDirectory);
        for (const auto& asset : c.project.assets) {
            QVERIFY(asset.path.startsWith(dir.filePath("edited.media") + "/")); QVERIFY(QFileInfo::exists(asset.path));
        }
        // Undo history follows the saved media, so the extraction is released and undo never restores its paths.
        QVERIFY(!QFileInfo::exists(temporary));
        c.undo(); QVERIFY(!c.project.packageDirectory); QCOMPARE(c.project.title, generated.title);
        for (const auto& asset : c.project.assets) QVERIFY(asset.path.startsWith(dir.filePath("edited.media") + "/"));
        const auto autosave = dir.filePath("undone.savshow"); writeProject(c.project, autosave);
        QCOMPARE(readProject(autosave).title, generated.title);
        c.redo(); QCOMPARE(c.project.title, QString("An edited copy"));
        c.replace(newProject());
        const auto restored = readProject(dir.filePath("edited.savshow")); QCOMPARE(restored.title, QString("An edited copy"));
        const auto original = readProject(package); QCOMPARE(original.title, generated.title);
    }
    void packagedPcmSurvivesSaveAsAfterExtractionIsReleased() {
        QTemporaryDir dir; const auto package = dir.filePath("demo.savpack");
        writeProjectPackage(demoProject(), package);
        Controller controller; controller.replace(readProject(package));
        const auto extracted = controller.project.packageDirectory->path();
        controller.save(dir.filePath("saved.savshow")); QVERIFY2(!controller.dirty, qPrintable(controller.status));
        QCOMPARE(controller.path, dir.filePath("saved.savshow"));
        QVERIFY(!QFileInfo::exists(extracted)); QVERIFY(!controller.transport.audioReadFailed());
        for (const auto& clip : controller.project.show.clips) if (clip.audio) {
            QCOMPARE(clip.audio, controller.project.assets[QString::fromStdString(clip.assetId)].audio);
            QVERIFY(std::isfinite(sampleAt(*clip.audio, clip.audio->length() - 1)));
        }
        Transport transport; transport.configure(std::make_shared<Show>(controller.project.show));
        QVERIFY(!transport.audioReadFailed());
    }
    void packageExportFailurePreservesDestination() {
        QTemporaryDir dir; Controller c; c.replace(demoProject()); c.save(dir.filePath("source.savshow"));
        const auto target = dir.filePath("existing.savpack");
        QFile file(target); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("previous package"); file.close();
        const auto media = c.project.assets.first().path; QVERIFY(QFile::remove(media));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, writeProjectPackage(c.project, target));
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("previous package")); file.close();
        QVERIFY(c.project.assets.first().path == media);
        auto valid = demoProject();
        const auto missingDirectory = dir.filePath("absent/show.savpack");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, writeProjectPackage(valid, missingDirectory));
        QVERIFY(!QFileInfo::exists(missingDirectory));
    }
    void unsafeAndCorruptPackagesAreRejected() {
        QTemporaryDir dir; const auto path = dir.filePath("bad.savpack");
        const QByteArray manifest = R"({"assets":[]})";
        const std::vector<std::vector<ZipEntry>> fixtures = {
            {{"../outside", "escape"}, {"project.savshow", manifest}},
            {{"/absolute", "escape"}, {"project.savshow", manifest}},
            {{"media/../outside", "escape"}, {"project.savshow", manifest}},
            {{"media\\outside.png", "escape"}, {"project.savshow", manifest}},
            {{"media/link.png", {}, true}, {"project.savshow", manifest}},
            {{"project.savshow", manifest}, {"project.savshow", manifest}},
            {{"media/a.png", "one"}, {"media/A.png", "two"}, {"project.savshow", manifest}},
            {{"media/a.png", "no manifest"}},
            {{"project.savshow", R"({"assets":[{"path":"../outside.png"}]})"}},
            {{"project.savshow", R"({"assets":[{"path":"media/missing.png"}]})"}},
            {{"project.savshow", "not JSON"}}
        };
        auto reject = [&](const QByteArray& bytes) { QVERIFY(!openFailure(path, bytes).isEmpty()); };
        for (const auto& fixture : fixtures) reject(zipBytes(fixture));
        const auto valid = packageFixture(dir.filePath("fixture"));
        auto bytes = zipBytes({{valid.name, valid.image}, {"project.savshow", valid.manifest}});
        QCOMPARE(openFailure(path, bytes), QString());
        const auto offset = bytes.indexOf(valid.image); QVERIFY(offset >= 0); bytes[offset + valid.image.size() / 2] ^= 1;
        QVERIFY(openFailure(path, bytes).contains("CRC"));
        reject(bytes.left(bytes.size() / 2));
        reject("not a ZIP");
        QVERIFY(!QFileInfo::exists(dir.filePath("outside")));
    }
    void packageMediaMustBeStoredReferencedAndExporterNamed() {
        QTemporaryDir dir; const auto path = dir.filePath("test.savpack"); const auto fixture = packageFixture(dir.filePath("fixture"));
        const ZipEntry manifest{"project.savshow", fixture.manifest}, image{fixture.name, fixture.image};
        QCOMPARE(openFailure(path, zipBytes({manifest, image})), QString());
        // Deflated media could expand far beyond its declared budget; only the manifest may be compressed.
        QVERIFY(openFailure(path, zipBytes({manifest, image}, false, true)).contains(fixture.name));
        const auto unused = "media/" + newId() + ".png";
        QVERIFY(openFailure(path, zipBytes({manifest, image, {unused, fixture.image}})).contains("unreferenced"));
        for (const QString name : {QString("media/CON"), QString("media/NUL.wav"), QString("media/picture.png"),
                 "media/" + newId().toUpper() + ".png", fixture.name.chopped(4)}) {
            auto renamed = fixture.manifest; renamed.replace(fixture.name.toUtf8(), name.toUtf8());
            QVERIFY(openFailure(path, zipBytes({{"project.savshow", renamed}, {name, fixture.image}})).startsWith("Unsafe"));
        }
    }
    void projectJsonTypesAreStrictBeforeMediaLoads() {
        QTemporaryDir dir; Project p = demoProject(); materializeProjectMedia(p, dir.filePath("show.media"));
        const auto path = dir.filePath("show.savshow"); writeProject(p, path);
        QFile input(path); QVERIFY(input.open(QIODevice::ReadOnly)); const auto saved = QJsonDocument::fromJson(input.readAll()).object(); input.close();
        auto edited = [](QJsonObject root, const char* array, int index, const char* key, const QJsonValue& value) {
            auto items = root[array].toArray(); auto item = items[index].toObject();
            if (value.isUndefined()) item.remove(key); else item[key] = value;
            items[index] = item; root[array] = items; return root;
        };
        auto failure = [&](const QJsonObject& root) { return openFailure(dir.filePath("typed.savshow"), QJsonDocument(root).toJson()); };
        // Optional fields omitted by hand-written version-1 files keep their defaults.
        auto sparse = saved;
        for (const char* key : {"name", "muted", "gain", "pan"}) sparse = edited(sparse, "tracks", 2, key, QJsonValue::Undefined);
        for (const char* key : {"text", "loop", "gain", "scale", "opacity"}) sparse = edited(sparse, "clips", 0, key, QJsonValue::Undefined);
        QCOMPARE(failure(sparse), QString());
        const auto loaded = readProject(dir.filePath("typed.savshow"));
        QCOMPARE(loaded.show.tracks[2].gain, 1.); QVERIFY(!loaded.show.tracks[2].muted); QCOMPARE(loaded.show.clips[0].scale, 1.);
        // Every asset is unreachable, so structural errors must be reported before any media is opened.
        auto missing = saved; auto assets = missing["assets"].toArray();
        for (auto&& value : assets) { auto asset = value.toObject(); asset["path"] = "absent/" + asset["path"].toString(); value = asset; }
        missing["assets"] = assets;
        QVERIFY(failure(missing).startsWith("Media file is missing"));
        const struct { const char* array; int index; const char* key; QJsonValue value; } cases[] = {
            {"tracks", 2, "gain", "x"}, {"tracks", 2, "left", 1e10}, {"tracks", 2, "right", 1.5}, {"tracks", 2, "muted", 1},
            {"tracks", 0, "name", QJsonValue::Null}, {"clips", 0, "loop", "yes"}, {"clips", 0, "track", "0"}, {"clips", 0, "opacity", true},
            {"markers", 0, "hold", "true"}, {"markers", 0, "timing", 1}, {"assets", 0, "name", 5}, {"assets", 0, "path", QJsonValue::Null}
        };
        for (const auto& c : cases)
            QCOMPARE(failure(edited(missing, c.array, c.index, c.key, c.value)), QString("Invalid project field: ") + c.key);
        auto tracks = missing["tracks"].toArray(); tracks[0] = 5; auto entry = missing; entry["tracks"] = tracks;
        QCOMPARE(failure(entry), QString("Invalid project entry"));
        missing["width"] = 1920.5; QCOMPARE(failure(missing), QString("Invalid project field: width"));
    }
    void failedMaterializationRemovesCreatedFiles() {
        QTemporaryDir dir; const auto target = dir.filePath("show.media");
        Project p = newProject(); MediaAsset image; image.id = "a-image"; image.image = true;
        image.still = QImage(4, 4, QImage::Format_RGB32); image.still.fill(Qt::red); p.assets.insert(image.id, image);
        MediaAsset broken; broken.id = "z-broken"; p.assets.insert(broken.id, broken);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, materializeProjectMedia(p, target)); QVERIFY(!QFileInfo::exists(target));
        QVERIFY(QDir().mkpath(target));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, materializeProjectMedia(p, target));
        QVERIFY(QFileInfo::exists(target)); QVERIFY(QDir(target).isEmpty());
        // Copies of extracted package media are removed as well.
        const auto package = dir.filePath("demo.savpack"); writeProjectPackage(demoProject(), package);
        auto packaged = readProject(package); packaged.assets.insert(broken.id, broken);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, materializeProjectMedia(packaged, target));
        QVERIFY(QDir(target).isEmpty()); QVERIFY(packaged.packageDirectory);
    }
    void stalePackageDirectoriesAreRemoved() {
        auto make = [](const QString& prefix) {
            const auto path = QDir::temp().filePath(prefix + QUuid::createUuid().toString(QUuid::Id128));
            QDir().mkpath(path + "/media"); QFile file(path + "/media/a.wav"); if (file.open(QIODevice::WriteOnly)) file.write("x");
            return path;
        };
        auto age = [](const QString& path) {
            std::filesystem::last_write_time(std::filesystem::path(path.toStdU16String()),
                std::filesystem::file_time_type::clock::now() - std::chrono::hours(48));
        };
        const auto stale = make("simpleavshow-package-"), staging = make("simpleavshow-export-"),
            fresh = make("simpleavshow-package-"), unrelated = make("simpleavshow-other-");
        for (const auto& path : {stale, staging, unrelated}) age(path);
        // A package open in a running instance keeps its lock, however old its folder is.
        QTemporaryDir dir; const auto package = dir.filePath("demo.savpack"); writeProjectPackage(demoProject(), package);
        auto open = readProject(package); const auto live = open.packageDirectory->path(); age(live);
        removeStalePackageDirectories();
        QVERIFY(!QFileInfo::exists(stale)); QVERIFY(!QFileInfo::exists(staging));
        QVERIFY(QFileInfo::exists(fresh)); QVERIFY(QFileInfo::exists(unrelated)); QVERIFY(QFileInfo::exists(live + "/media"));
        open = newProject(); QVERIFY(!QFileInfo::exists(live));
        for (const auto& path : {fresh, unrelated}) QVERIFY(QDir(path).removeRecursively());
    }
    void packageExportActionRespectsEditingLocks() {
        MainWindow window;
        auto* action = window.findChild<QAction*>("exportPackageAction"); QVERIFY(action); QVERIFY(action->isEnabled());
        window.controller().setShowMode(true); QTRY_VERIFY(!action->isEnabled());
        window.controller().setShowMode(false); QTRY_VERIFY(action->isEnabled());
        window.controller().busy = true; QTRY_VERIFY(!action->isEnabled());
        window.controller().busy = false; QTRY_VERIFY(action->isEnabled());
    }
    void exportPackageThroughFileMenu() {
        QTemporaryDir dir; const auto package = dir.filePath("menu-export.savpack");
        const bool nativeDisabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
        MainWindow window; auto& c = window.controller(); c.replace(demoProject()); c.dirty = true;
        bool selected = false;
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
            if (!dialog) return;
            selected = true; dialog->selectFile(dir.filePath("menu-export")); QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        });
        window.findChild<QAction*>("exportPackageAction")->trigger();
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, nativeDisabled);
        QVERIFY(selected); QVERIFY(QFileInfo::exists(package)); QVERIFY(!c.busy); QVERIFY(c.dirty); QVERIFY(c.path.isEmpty());
        QCOMPARE(c.status, QString("Exported menu-export.savpack"));
        for (const auto& asset : c.project.assets) QVERIFY(asset.path.isEmpty());
        const auto loaded = readProject(package); QCOMPARE(loaded.assets.size(), c.project.assets.size());
    }
    void zip64AndCompressedPackagesCanBeOpened() {
        QTemporaryDir dir;
        writeProject(newProject(), dir.filePath("empty.savshow"));
        QFile manifest(dir.filePath("empty.savshow")); QVERIFY(manifest.open(QIODevice::ReadOnly));
        const auto json = manifest.readAll();
        for (const bool compressed : {false, true}) {
            const auto bytes = zipBytes({{"project.savshow", json}}, true, compressed);
            const auto path = dir.filePath("zip64.savpack"); QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(bytes), bytes.size()); file.close();
            const auto loaded = readProject(path); QCOMPARE(loaded.title, QString("Untitled show")); QVERIFY(loaded.assets.isEmpty());
        }
    }
    void packagedVideoAndEmbeddedAudioSurviveSaveAs() {
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg"); if (ffmpeg.isEmpty()) QSKIP("ffmpeg fixture generation unavailable");
        QTemporaryDir originals, destination; const auto source = originals.filePath("video.mkv");
        QProcess process; process.start(ffmpeg, {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "color=red:s=320x180:r=24:d=1", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=1", "-c:v", "ffv1", "-c:a", "pcm_s16le", source});
        QVERIFY(process.waitForFinished(15000)); QCOMPARE(process.exitCode(), 0);
        Project p = newProject(); p.width = 320; p.height = 240;
        auto asset = loadMedia(source); QVERIFY(asset.video && asset.audio); p.assets.insert(asset.id, asset);
        Clip picture; picture.id = newId().toStdString(); picture.assetId = asset.id.toStdString(); picture.track = 0; picture.duration = frames(1);
        p.show.clips.push_back(picture); Clip audio = picture; audio.id = newId().toStdString(); audio.track = 2; p.show.clips.push_back(audio); bindAudio(p);
        const auto package = destination.filePath("video.savpack"); writeProjectPackage(p, package);
        QVERIFY(QDir(originals.path()).removeRecursively());
        Controller c; c.replace(readProject(package));
        const auto temporary = c.project.packageDirectory->path();
        QCOMPARE(c.project.assets.first().audio->length(), frames(1)); QVERIFY(c.project.assets.first().audio->storage);
        c.seek(frames(.2));
        QTRY_VERIFY_WITH_TIMEOUT(c.compose().pixelColor(160, 120).red() > 200, 3000);
        c.save(destination.filePath("video.savshow")); QVERIFY2(!c.path.isEmpty(), qPrintable(c.status));
        QVERIFY(!QFileInfo::exists(temporary));
        c.seek(frames(.4));
        QTRY_VERIFY_WITH_TIMEOUT(c.compose().pixelColor(160, 120).red() > 200, 3000);
        const auto reloaded = readProject(c.path); QCOMPARE(reloaded.assets.first().audio->length(), frames(1));
        float peak = 0;
        for (Frame i = 0; i < frames(.05); ++i) peak = std::max(peak, std::abs(sampleAt(*reloaded.assets.first().audio, i)));
        QVERIFY(peak > .1);
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
    void videoSkipsCorruptPackets() {
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg"); if (ffmpeg.isEmpty()) QSKIP("ffmpeg CLI is unavailable for fixture generation");
        QTemporaryDir dir; const QString file = dir.filePath("corrupt.mkv");
        QProcess process; process.start(ffmpeg, {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "color=red:s=160x90:r=24:d=1", "-f", "lavfi", "-i", "color=blue:s=160x90:r=24:d=3", "-filter_complex", "[0:v][1:v]concat=n=2:v=1:a=0[v]", "-map", "[v]", "-c:v", "png", file});
        QVERIFY(process.waitForFinished(15000)); QCOMPARE(process.exitCode(), 0);
        QFile source(file); QVERIFY(source.open(QIODevice::ReadOnly)); const auto original = source.readAll(); source.close();
        // Damage the zlib streams of chosen frames; the container stays intact.
        auto damage = [&](const QString& path, int first, int end) {
            auto bytes = original; qsizetype at = 0; int damaged = 0;
            for (int frame = 0; (at = bytes.indexOf("IDAT", at)) >= 0; ++frame, at += 4)
                if (frame >= first && frame < end) { for (int i = 8; i < 16; ++i) bytes[at + i] = static_cast<char>(bytes[at + i] ^ 0x5a); ++damaged; }
            writeBytes(path, bytes); return damaged;
        };
        QCOMPARE(damage(dir.filePath("middle.mkv"), 20, 30), 10); // Spans the colour change.
        VideoReader reader(dir.filePath("middle.mkv")); QImage image;
        for (int frame = 0; frame < 46; ++frame)
            QTRY_VERIFY_WITH_TIMEOUT(!(image = reader.imageAt(frame / 24.)).isNull(), 3000);
        QVERIFY(image.pixelColor(80, 45).blue() > 200); QVERIFY(image.pixelColor(80, 45).red() < 30);
        QVERIFY(reader.error().isEmpty()); QVERIFY(reader.skipped() >= 10);
        // More than 50 consecutive failures are fatal, but the picture freezes on the last good frame.
        QCOMPARE(damage(dir.filePath("tail.mkv"), 30, 96), 66);
        VideoReader broken(dir.filePath("tail.mkv"));
        for (int frame = 0; frame < 29; ++frame)
            QTRY_VERIFY_WITH_TIMEOUT(!(image = broken.imageAt(frame / 24.)).isNull(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!broken.error().isEmpty(), 3000); QVERIFY(broken.skipped() > 50);
        const QImage frozen = broken.imageAt(3.5); QVERIFY(!frozen.isNull()); QCOMPARE(frozen, image);
        VideoReader missing(dir.filePath("missing.mkv"));
        QTRY_VERIFY_WITH_TIMEOUT(!missing.error().isEmpty(), 3000); QVERIFY(missing.imageAt(0).isNull());
    }
    void loopingVideoPrerollsTheLoopHead() {
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg"); if (ffmpeg.isEmpty()) QSKIP("ffmpeg CLI is unavailable for fixture generation");
        QTemporaryDir dir; const QString file = dir.filePath("loop.mkv");
        QProcess process; process.start(ffmpeg, {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "color=red:s=160x90:r=24:d=1", "-f", "lavfi", "-i", "color=blue:s=160x90:r=24:d=1", "-filter_complex", "[0:v][1:v]concat=n=2:v=1:a=0[v]", "-map", "[v]", "-c:v", "ffv1", file});
        QVERIFY(process.waitForFinished(15000)); QCOMPARE(process.exitCode(), 0);
        const auto asset = loadMedia(file); const double loopStart = .5;
        VideoReader reader(file); reader.setLoop(loopStart, asset.duration); QImage image;
        for (int cycle = 0; cycle < 2; ++cycle) {
            // Play the tail of the loop as Controller::compose would, then wrap.
            for (int frame = 36; frame < 48; ++frame)
                QTRY_VERIFY_WITH_TIMEOUT(!(image = reader.imageAt(frame / 24.)).isNull(), 3000);
            QVERIFY(image.pixelColor(80, 45).blue() > 200);
            QTest::qWait(300);
            image = reader.imageAt(loopStart + std::fmod(48 / 24. - loopStart, asset.duration - loopStart));
            QVERIFY(!image.isNull()); QVERIFY(image.pixelColor(80, 45).red() > 200);
            for (int frame = 13; frame < 30; ++frame)
                QTRY_VERIFY_WITH_TIMEOUT(!(image = reader.imageAt(frame / 24.)).isNull(), 3000);
            QVERIFY(image.pixelColor(80, 45).blue() > 200);
        }
        QVERIFY(reader.error().isEmpty()); QCOMPARE(reader.skipped(), 0u);
    }
    void monoFrontLeftDecodesAsCentre() {
        // WAVE_FORMAT_EXTENSIBLE mono with a front-left mask bypasses direct PCM and decodes through FFmpeg.
        QTemporaryDir dir; const auto path = dir.filePath("front-left.wav");
        auto bytes = waveBytes(16, 1, 1, sampleRate, true); qToLittleEndian<quint32>(1, bytes.data() + 50); writeBytes(path, bytes);
        const auto asset = loadMedia(path); QVERIFY(asset.audio); QVERIFY(asset.audio->mono); QCOMPARE(asset.audio->length(), 17003);
        float samples[200]; asset.audio->storage->read(1000, samples, 100);
        for (int i = 0; i < 100; ++i) {
            QVERIFY(std::abs(samples[i * 2] - waveValue(1000 + i, 0)) < 1e-4f); QCOMPARE(samples[i * 2 + 1], samples[i * 2]);
        }
    }
    void compatiblePcmUsesSourceFile_data() {
        QTest::addColumn<int>("bits"); QTest::addColumn<int>("type");
        QTest::addColumn<int>("channels"); QTest::addColumn<bool>("extensible");
        for (int channels : {1, 2}) for (int bits : {8, 16, 24, 32}) for (bool extensible : {false, true}) {
            const auto name = QString("pcm%1-%2-%3").arg(bits).arg(channels).arg(extensible);
            QTest::newRow(qPrintable(name)) << bits << 1 << channels << extensible;
        }
        for (int channels : {1, 2}) for (bool extensible : {false, true}) {
            const auto name = QString("float32-%1-%2").arg(channels).arg(extensible);
            QTest::newRow(qPrintable(name)) << 32 << 3 << channels << extensible;
        }
    }
    void compatiblePcmUsesSourceFile() {
        QFETCH(int, bits); QFETCH(int, type); QFETCH(int, channels); QFETCH(bool, extensible);
        QTemporaryDir dir; const auto path = dir.filePath("source.wav");
        const auto bytes = waveBytes(bits, type, channels, sampleRate, extensible); writeBytes(path, bytes);
        QDir caches(detail::audioCacheDirectory()); const auto before = caches.entryList(QDir::Files);
        const auto asset = loadMedia(path);
        QVERIFY(asset.audio); QVERIFY(asset.audio->storage); QVERIFY(asset.audio->samples.empty());
        QCOMPARE(asset.audio->length(), 17003); QCOMPARE(asset.audio->mono, channels == 1);
        QCOMPARE(caches.entryList(QDir::Files), before);
        float samples[8]; asset.audio->storage->read(8191, samples, 4);
        for (int i = 0; i < 4; ++i) {
            QCOMPARE(samples[i * 2], waveValue(8191 + i, 0));
            QCOMPARE(samples[i * 2 + 1], waveValue(8191 + i, channels == 1 ? 0 : 1));
        }
        QCOMPARE(asset.audio->peakAt(0), 1.f);
        // A short loop straddles a block boundary and wraps to a nonzero offset.
        auto show = std::make_shared<Show>(); show->duration = frames(1);
        show->tracks.push_back({"audio", "Audio", false, false, false, 1, channels == 1 ? -1. : 0., 0, 1});
        Clip clip; clip.audio = asset.audio; clip.duration = show->duration; clip.offset = 16379; clip.loop = true;
        show->clips.push_back(clip);
        Transport transport; transport.configure(show); QVERIFY(!transport.audioReadFailed()); transport.command({Action::Play});
        std::vector<float> output(4000); transport.render(output.data(), 2000, 2);
        QCOMPARE(transport.bufferWaits(), 0u);
        for (Frame i = 0; i < 2000; ++i) {
            const auto source = clip.offset + i % (17003 - clip.offset);
            QCOMPARE(output[i * 2], waveValue(source, 0));
            QCOMPARE(output[i * 2 + 1], channels == 1 ? 0.f : waveValue(source, 1));
        }
        transport.command({Action::Seek, 7}); transport.render(output.data(), 1, 2);
        QCOMPARE(output[0], waveValue(clip.offset + 7, 0));
        QVERIFY(QFile::remove(path));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, asset.audio->storage->read(0, samples, 1));
        // Direct storage must leave original bytes untouched.
        writeBytes(path, bytes); QFile source(path); QVERIFY(source.open(QIODevice::ReadOnly)); QCOMPARE(source.readAll(), bytes);
    }
    void changedPcmSourceStopsPlayback_data() {
        QTest::addColumn<bool>("preserveTimestamp");
        QTest::newRow("changed-timestamp") << false;
        QTest::newRow("preserved-timestamp") << true;
    }
    void changedPcmSourceStopsPlayback() {
        QFETCH(bool, preserveTimestamp);
        QTemporaryDir dir; const auto path = dir.filePath("source.wav");
        auto bytes = waveBytes(16, 1, 1, sampleRate); writeBytes(path, bytes);
        const auto asset = loadMedia(path);
        QFile source(path); QVERIFY(source.open(QIODevice::ReadWrite));
        const auto changedTime = QFileInfo(path).lastModified().addSecs(preserveTimestamp ? 0 : 2);
        QVERIFY(source.seek(bytes.size() - 2)); QCOMPARE(source.write("\0\0", 2), 2);
        QVERIFY(source.flush()); QVERIFY(source.setFileTime(changedTime, QFileDevice::FileModificationTime)); source.close();
        QCOMPARE(QFileInfo(path).lastModified(), changedTime);
        float sample[2]; QVERIFY_THROWS_EXCEPTION(std::runtime_error, asset.audio->storage->read(asset.audio->length() - 1, sample, 1));
        auto show = std::make_shared<Show>(); show->duration = frames(1); show->tracks.push_back({"audio", "Audio"});
        Clip clip; clip.audio = asset.audio; clip.duration = show->duration; show->clips.push_back(clip);
        Transport transport; transport.configure(show); QVERIFY(transport.audioReadFailed());
        transport.command({Action::Seek, asset.audio->length() - 1}); transport.command({Action::Play}); transport.render(sample, 1, 2);
        QCOMPARE(transport.position(), asset.audio->length() - 1); QCOMPARE(sample[0], 0.f);
        QVERIFY(loadMedia(path).audio->samples.empty()); // Reopening binds the changed source.
    }
    void persistentAudioCacheReuseInvalidationAndRecovery() {
        QTemporaryDir dir; const auto source = dir.filePath("resample.wav");
        const auto original = waveBytes(16, 1, 2, 44100, false, 44100); writeBytes(source, original);
        QDir caches(detail::audioCacheDirectory()); const auto before = caches.entryList({"*.pcm"}, QDir::Files);
        auto first = loadMedia(source); QVERIFY(first.audio); QCOMPARE(first.audio->length(), frames(1));
        auto created = caches.entryList({"*.pcm"}, QDir::Files);
        for (const auto& name : before) created.removeAll(name);
        QCOMPARE(created.size(), 1);
        const auto pcm = caches.filePath(created.front());
        const auto metadata = pcm.left(pcm.size() - 4) + ".meta";
        std::vector<float> expected(static_cast<std::size_t>(first.audio->length()) * 2);
        first.audio->storage->read(0, expected.data(), static_cast<std::size_t>(first.audio->length()));
        const auto peaks = first.audio->peaks; first.audio.reset();
        QVERIFY(QFileInfo::exists(pcm)); // Cache survives the last in-memory owner.
        const auto sentinel = QDateTime::fromMSecsSinceEpoch(946684800000ll).toUTC();
        QFile cacheFile(pcm); QVERIFY(cacheFile.open(QIODevice::ReadWrite));
        QVERIFY(cacheFile.setFileTime(sentinel, QFileDevice::FileModificationTime)); cacheFile.close();
        QFile metaFile(metadata); QVERIFY(metaFile.open(QIODevice::ReadWrite));
        QVERIFY(metaFile.setFileTime(sentinel, QFileDevice::FileModificationTime)); metaFile.close();
        // Same content at another path shares the artifact without rewriting it.
        const auto moved = dir.filePath("moved.wav"); QVERIFY(QFile::copy(source, moved));
        auto reused = loadMedia(moved);
        QCOMPARE(QFileInfo(pcm).lastModified(), sentinel); QCOMPARE(QFileInfo(metadata).lastModified(), sentinel);
        QCOMPARE(reused.audio->peaks, peaks);
        float sample[2]; reused.audio->storage->read(1234, sample, 1); QCOMPARE(sample[0], expected[2468]);
        reused.audio.reset();
        // Same-size cache corruption with a preserved timestamp must rebuild.
        QVERIFY(cacheFile.open(QIODevice::ReadWrite)); QVERIFY(cacheFile.seek(2468 * sizeof(float)));
        QCOMPARE(cacheFile.write("\0\0\0\0", 4), 4); QVERIFY(cacheFile.flush());
        QVERIFY(cacheFile.setFileTime(sentinel, QFileDevice::FileModificationTime)); cacheFile.close();
        QCOMPARE(QFileInfo(pcm).lastModified(), sentinel);
        auto repaired = loadMedia(source); repaired.audio->storage->read(1234, sample, 1); QCOMPARE(sample[0], expected[2468]);
        QCOMPARE(repaired.audio->peaks, peaks);
        // Waveform metadata damage is rejected, including a same-length mutation.
        QVERIFY(metaFile.open(QIODevice::ReadWrite)); QVERIFY(metaFile.seek(metaFile.size() - 1));
        QCOMPARE(metaFile.write("\xff", 1), 1); metaFile.close();
        QCOMPARE(loadMedia(source).audio->peaks, peaks);
        // A truncated artifact is also rebuilt rather than reused.
        QVERIFY(cacheFile.open(QIODevice::ReadWrite)); QVERIFY(cacheFile.resize(32)); cacheFile.close();
        QCOMPARE(loadMedia(source).audio->length(), frames(1)); QCOMPARE(QFileInfo(pcm).size(), frames(1) * 8);
        // Source identity uses all bytes, even when size and timestamp are unchanged.
        const auto sourceTime = QFileInfo(source).lastModified();
        auto changed = original; changed[changed.size() - 8] ^= 0x40; writeBytes(source, changed);
        QFile sourceFile(source); QVERIFY(sourceFile.open(QIODevice::ReadWrite));
        QVERIFY(sourceFile.setFileTime(sourceTime, QFileDevice::FileModificationTime)); sourceFile.close();
        const auto count = caches.entryList({"*.pcm"}, QDir::Files).size();
        QVERIFY(loadMedia(source).audio); QCOMPARE(caches.entryList({"*.pcm"}, QDir::Files).size(), count + 1);
        QCOMPARE(caches.entryList({"preparing-*", "*.lock"}, QDir::Files).size(), 0);
    }
    void concurrentImportsSharePreparedAudio() {
        QTemporaryDir dir; const auto source = dir.filePath("parallel.wav");
        writeBytes(source, waveBytes(24, 1, 2, 32000, false, 32000));
        QDir caches(detail::audioCacheDirectory()); const auto before = caches.entryList({"*.pcm"}, QDir::Files);
        auto first = std::async(std::launch::async, [source] { return loadMedia(source); });
        auto second = std::async(std::launch::async, [source] { return loadMedia(source); });
        const auto a = first.get(), b = second.get();
        QCOMPARE(a.audio->length(), frames(1)); QCOMPARE(a.audio->peaks, b.audio->peaks);
        QCOMPARE(sampleAt(*a.audio, 100), sampleAt(*b.audio, 100));
        QCOMPARE(caches.entryList({"*.pcm"}, QDir::Files).size(), before.size() + 1);
        QCOMPARE(caches.entryList({"preparing-*", "*.lock"}, QDir::Files).size(), 0);
    }
    void diskAudioCacheHasBoundedMemoryAndPersists() {
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg"); if (ffmpeg.isEmpty()) QSKIP("ffmpeg fixture generation unavailable");
        QTemporaryDir dir; const QString file = dir.filePath("long-silence.flac"); QProcess process;
        process.start(ffmpeg, {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "anullsrc=r=8000:cl=mono", "-t", "2800", "-c:a", "flac", file});
        QVERIFY(process.waitForFinished(15000)); QCOMPARE(process.exitCode(), 0);
        QDir caches(detail::audioCacheDirectory()); const auto before = caches.entryList({"*.pcm"}, QDir::Files);
        std::shared_ptr<const AudioData> retained;
        QString cache;
        {
            const auto asset = loadMedia(file); retained = asset.audio;
            QVERIFY(retained); QVERIFY(retained->storage); QVERIFY(retained->samples.empty());
            QCOMPARE(retained->length(), frames(2800));
            QVERIFY(retained->peaks.size() * sizeof(float) < 2 * 1024 * 1024);
            auto created = caches.entryList({"*.pcm"}, QDir::Files);
            for (const auto& name : before) created.removeAll(name);
            QCOMPARE(created.size(), 1); cache = caches.filePath(created.front());
            QVERIFY(QFileInfo(cache).size() > 1024ll * 1024 * 1024);
            // The immutable prepared source survives moves/deletion of the original,
            // and snapshots keep it alive after releasing the importing asset.
            QVERIFY(QFile::remove(file)); QCOMPARE(sampleAt(*retained, retained->length() - 1), 0.f);
        }
        QVERIFY(QFileInfo::exists(cache)); retained.reset(); QVERIFY(QFileInfo::exists(cache));
        const auto complete = caches.entryList(QDir::Files);
        // Failed imports must release partial caches as well.
        QFile broken(dir.filePath("broken.wav")); QVERIFY(broken.open(QIODevice::WriteOnly)); broken.write("broken"); broken.close();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, loadMedia(broken.fileName()));
        QCOMPARE(caches.entryList(QDir::Files), complete);
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
