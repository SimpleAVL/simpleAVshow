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
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QSettings>
#include <QLockFile>
#include <QCheckBox>
#include <QFileDialog>
#include <QScrollBar>
#include <QScreen>
#include <QWindow>
#include <archive.h>
#include <archive_entry.h>
#include <limits>
#include <QStandardPaths>
#include <QUuid>
#include <QtEndian>
#include <bit>
#include <chrono>
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
struct PaintSpy : QObject {
    QRegion region; int count = 0;
    bool eventFilter(QObject*, QEvent* e) override { if (e->type() == QEvent::Paint) { region += static_cast<QPaintEvent*>(e)->region(); ++count; } return false; }
};
// A long, empty show: the playhead can travel without holds or clips.
Project longShow() { auto p = newProject(); p.show.clips.clear(); p.show.markers.clear(); p.show.duration = frames(600); return p; }
// Imports a resampled source with a unique last frame and returns its new cache key.
QString importCacheKey(const QString& path, Frame count, std::shared_ptr<const AudioData>* retained = nullptr) {
    auto bytes = waveBytes(16, 1, 2, 44100, false, count); bytes.replace(bytes.size() - 4, 4, QUuid::createUuid().toRfc4122().first(4));
    writeBytes(path, bytes);
    QDir caches(detail::audioCacheDirectory()); const auto before = caches.entryList({"*.pcm"}, QDir::Files);
    auto asset = loadMedia(path); if (retained) *retained = asset.audio;
    auto created = caches.entryList({"*.pcm"}, QDir::Files);
    for (const auto& name : before) created.removeAll(name);
    if (created.size() != 1) throw std::runtime_error("Expected one new audio cache");
    return created.front().chopped(4);
}
void setModified(const QString& path, const QDateTime& time) {
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite) || !file.setFileTime(time, QFileDevice::FileModificationTime)) throw std::runtime_error("Cannot set fixture time");
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
    // Most media tests cover the prepared cache; streaming tests opt in.
    void init() { QSettings().clear(); QSettings().setValue("audio/sources", "prepared"); setAudioPreparation(AudioPreparation::Prepared); }
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
    void autosaveRecoveryReplacesTheShowAndIsRemovedOnSave() {
        QTemporaryDir dir; const auto show = dir.filePath("show.savshow");
        Project saved = newProject(); saved.title = "Saved"; writeProject(saved, show);
        Project edited = saved; edited.title = "Autosaved"; writeProject(edited, show + ".autosave");
        QFile sidecar(show + ".autosave"); QVERIFY(sidecar.open(QIODevice::ReadWrite));
        QVERIFY(sidecar.setFileTime(QFileInfo(show).lastModified().addSecs(60), QFileDevice::FileModificationTime)); sidecar.close();
        MainWindow window; auto& c = window.controller(); bool asked = false;
        QTimer::singleShot(0, &window, [&] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()); if (!box) return;
            asked = true; for (auto* button : box->buttons()) if (button->text() == "Recover autosave") button->click();
        });
        window.openPath(show); QTRY_VERIFY_WITH_TIMEOUT(!c.busy, 15000);
        QVERIFY(asked); QCOMPARE(c.project.title, QString("Autosaved")); QCOMPARE(c.path, show); QVERIFY(c.dirty);
        c.save(c.path); QVERIFY(!c.dirty); QVERIFY(!QFileInfo::exists(show + ".autosave"));
        QCOMPARE(readProject(show).title, QString("Autosaved"));
        // Opening a sidecar directly also targets its show.
        writeProject(saved, show + ".autosave"); window.openPath(show + ".autosave"); QTRY_VERIFY_WITH_TIMEOUT(!c.busy, 15000);
        QCOMPARE(c.path, show); QVERIFY(c.dirty); QCOMPARE(c.project.title, QString("Saved"));
        QCOMPARE(c.autosavePath(), show + ".autosave");
        c.path.clear(); QVERIFY(c.autosavePath().endsWith("/Untitled.savshow.autosave"));
    }
    void readoutShowsNextCueAndTimeLeft() {
        MainWindow window; auto project = newProject(); project.show.duration = frames(90);
        project.show.markers = {{"late", "Finale", frames(75), true}, {"early", "Act one", frames(12), true}};
        window.controller().replace(project); window.show();
        auto* next = window.findChild<QLabel*>("nextCue"); QVERIFY(next);
        QTRY_VERIFY(next->text().contains("Act one · in 0:12")); QVERIFY(next->text().contains("1:30 left"));
        window.controller().seek(frames(20)); QTRY_VERIFY(next->text().contains("Finale · in 0:55"));
        window.controller().setShowMode(false); window.controller().dirty = false;
    }
    void inspectorAppliesTouchedFieldsAutomatically() {
        MainWindow window; window.show(); auto& c = window.controller();
        QVERIFY(c.edit([](Project& p) { Clip clip; clip.id = "t"; clip.text = "Hello"; clip.track = 0; clip.duration = frames(5); clip.opacity = 1. / 3; p.show.clips.push_back(clip); }));
        window.findChild<Timeline*>()->selectClip("t");
        auto* scale = window.findChild<QDoubleSpinBox*>("clipScale"); QVERIFY(scale);
        const auto clip = [&] { return c.project.show.clips.back(); };
        scale->setValue(2); QVERIFY(QMetaObject::invokeMethod(scale, "editingFinished"));
        QCOMPARE(clip().scale, 2.); QCOMPARE(clip().opacity, 1. / 3); // Untouched fields keep exact values.
        c.undo(); QCOMPARE(clip().scale, 1.); c.redo();
        // A pending field edit is flushed without overwriting a change made elsewhere (e.g. a timeline drag).
        scale->setValue(3);
        QVERIFY(c.edit([](Project& p) { p.show.clips.back().start = frames(2); }));
        QCOMPARE(clip().start, frames(2)); QCOMPARE(clip().scale, 3.);
        // Lengthening a one-shot video clip stops at the end of its source.
        MediaAsset video; video.id = "video"; video.name = "Clip.mov"; video.video = true; video.duration = 4; video.path = "/nonexistent/clip.mov";
        QVERIFY(c.edit([&](Project& p) { p.assets.insert(video.id, video); Clip v; v.id = "v"; v.assetId = "video"; v.track = 0; v.start = frames(10); v.duration = frames(2); v.offset = frames(1); p.show.clips.push_back(v); }));
        window.findChild<Timeline*>()->selectClip("v");
        auto* length = window.findChildren<QDoubleSpinBox*>().at(0); for (auto* spin : window.findChildren<QDoubleSpinBox*>()) if (spin->isVisible() && spin->value() == 2.) length = spin;
        length->setValue(10); QVERIFY(QMetaObject::invokeMethod(length, "editingFinished"));
        QCOMPARE(clip().duration, frames(3)); QVERIFY(c.status.contains("end of the source"));
        c.dirty = false;
    }
    void missingMediaOpensOfflineAndRelinks() {
        QTemporaryDir dir; QDir().mkpath(dir.filePath("a")); QDir().mkpath(dir.filePath("b"));
        writeBytes(dir.filePath("a/sound.wav"), waveBytes(16, 1, 2, 48000));
        Project project = newProject(); auto asset = loadMedia(dir.filePath("a/sound.wav")); project.assets.insert(asset.id, asset);
        Clip clip; clip.id = "c"; clip.assetId = asset.id.toStdString(); clip.track = 2; clip.duration = asset.audio->length(); project.show.clips.push_back(clip);
        bindAudio(project); writeProject(project, dir.filePath("show.savshow"));
        QVERIFY(QFile::rename(dir.filePath("a/sound.wav"), dir.filePath("b/sound.wav")));
        auto opened = readProject(dir.filePath("show.savshow"));
        QVERIFY(opened.assets.first().offline); QCOMPARE(opened.show.clips.size(), std::size_t{1});
        Controller c; c.replace(opened, dir.filePath("show.savshow"));
        QCOMPARE(c.missingMedia(), QStringList{dir.filePath("a/sound.wav")});
        c.toggle(); QCOMPARE(c.transport.state(), State::Stopped); QVERIFY(c.status.contains("missing"));
        // Saving keeps the original reference so a later relink or restored drive still finds it.
        c.save(dir.filePath("show.savshow")); QVERIFY(readProject(dir.filePath("show.savshow")).assets.first().offline);
        auto relinked = loadMedia(dir.filePath("b/sound.wav")); relinked.id = asset.id; relinked.name = asset.name;
        QVERIFY(c.edit([&](Project& p) { p.assets.insert(relinked.id, relinked); }));
        QVERIFY(c.missingMedia().isEmpty()); QVERIFY(c.project.show.clips[0].audio);
    }
    void realDeviceHeartbeatAdvances() {
        if (qEnvironmentVariable("SAV_TEST_AUDIO_DEVICE") != "1") QSKIP("Set SAV_TEST_AUDIO_DEVICE=1 to open the default output (plays silence)");
        Transport transport; transport.configure(std::make_shared<Show>()); AudioDevice device(transport);
        const auto selected = playbackDevice(device.devices(RtAudio::UNSPECIFIED), {}); QVERIFY(selected);
        QString error; QVERIFY2(device.open(RtAudio::UNSPECIFIED, selected->id, 2, 512, error), qPrintable(error));
        const auto before = device.callbacks(); QTest::qWait(300);
        QVERIFY(device.running()); QVERIFY(device.callbacks() > before); QVERIFY(device.lastError().isEmpty());
        // Re-arming pauses and resumes the open stream instead of reopening the driver.
        device.pause(); QVERIFY(!device.running()); transport.configure(std::make_shared<Show>());
        const auto paused = device.callbacks(); QVERIFY(device.resume()); QTest::qWait(200);
        QVERIFY(device.running()); QVERIFY(device.callbacks() > paused);
        device.close(); QVERIFY(!device.running()); QVERIFY(!device.resume());
    }
    void connectedAsioDeviceRemainsAvailableWhileStreamIsOpen() {
        const QString name = qEnvironmentVariable("SAV_TEST_ASIO_DEVICE");
        if (name.isEmpty()) QSKIP("Set SAV_TEST_ASIO_DEVICE to a Windows ASIO output name");
#ifndef Q_OS_WIN
        QSKIP("Requires Windows ASIO");
#else
        if (QGuiApplication::platformName() != "windows") QSKIP("ASIO requires the native Windows desktop platform");
        Transport transport; transport.configure(std::make_shared<Show>()); AudioDevice device(transport);
        const auto selected = playbackDevice(device.devices(RtAudio::WINDOWS_ASIO), name); QVERIFY(selected);
        QString error; QVERIFY2(device.open(RtAudio::WINDOWS_ASIO, selected->id, std::min(16u, selected->channels), 512, error), qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(device.callbacks() > 0, 3000);
        const auto before = device.callbacks();
        // Outputs must still offer the connected device while its ASIO stream is open.
        const auto live = playbackDevice(device.devices(RtAudio::WINDOWS_ASIO), name);
        QVERIFY(live); QCOMPARE(live->id, selected->id); QCOMPARE(live->channels, selected->channels);
        QVERIFY(playbackDevice(device.devices(RtAudio::UNSPECIFIED), name));
        QVERIFY(device.running()); QTRY_VERIFY_WITH_TIMEOUT(device.callbacks() > before, 3000);
        device.pause(); QVERIFY(!device.running());
        QVERIFY(playbackDevice(device.devices(RtAudio::WINDOWS_ASIO), name));
        const auto paused = device.callbacks(); QVERIFY(device.resume());
        QTRY_VERIFY_WITH_TIMEOUT(device.callbacks() > paused, 3000);
        QVERIFY(device.lastError().isEmpty()); device.close();
#endif
    }
    void noteOffAsZeroVelocityKeepsItsBytesAndCueNamesStayUnique() {
        MidiMessageEditor editor; std::vector<unsigned char> bytes; QString error;
        editor.setMessage({0x91, 60, 0}); QVERIFY(editor.message(bytes, error));
        QVERIFY(bytes == std::vector<unsigned char>({0x91, 60, 0})); QVERIFY(midiSummary(bytes).startsWith("Stop"));
        MainWindow window; window.show(); auto& c = window.controller(); QPushButton* hold = nullptr;
        for (auto* b : window.findChildren<QPushButton*>()) if (b->text() == "+ Hold / MIDI cue") hold = b;
        QVERIFY(hold); QTest::mouseClick(hold, Qt::LeftButton); c.seek(frames(1)); QTest::mouseClick(hold, Qt::LeftButton);
        QVERIFY(c.edit([](Project& p) { p.show.markers.erase(p.show.markers.begin()); }));
        c.seek(frames(2)); QTest::mouseClick(hold, Qt::LeftButton);
        QCOMPARE(c.project.show.markers[0].name, std::string("Hold 2")); QCOMPARE(c.project.show.markers[1].name, std::string("Hold 3"));
        c.dirty = false;
    }
    void editingActionsSplitDuplicateNudgePasteAndJump() {
        MainWindow window; window.show(); auto& c = window.controller();
        const auto act = [&](const QString& text) { for (auto* a : window.findChildren<QAction*>()) if (a->text() == text) { a->trigger(); return true; } return false; };
        QVERIFY(c.edit([](Project& p) {
            Clip clip; clip.id = "t"; clip.text = "Hello"; clip.track = 0; clip.start = frames(2); clip.duration = frames(4);
            clip.alpha.points = {{0, 0}, {frames(4), 1}}; p.show.clips.push_back(clip);
            p.show.markers = {{"a", "A", frames(1), true}, {"b", "B", frames(9), true}};
        }));
        auto* timeline = window.findChild<Timeline*>(); timeline->selectClip("t");
        c.seek(frames(3)); QVERIFY(act("Split clip at playhead"));
        QCOMPARE(c.project.show.clips.size(), std::size_t{2});
        const auto& head = c.project.show.clips[0]; const auto& tail = c.project.show.clips[1];
        QCOMPARE(head.duration, frames(1)); QCOMPARE(tail.start, frames(3)); QCOMPARE(tail.duration, frames(3)); QCOMPARE(tail.offset, frames(1));
        QCOMPARE(head.alpha.points.back().value, .25); QCOMPARE(tail.alpha.points.front().value, .25); QCOMPARE(tail.alpha.points.back().time, frames(3));
        QVERIFY(act("Duplicate clip")); QCOMPARE(c.project.show.clips.back().start, frames(6));
        QVERIFY(act("Nudge right one frame")); QCOMPARE(c.project.show.clips.back().start, frames(6) + frames(1. / 24));
        QVERIFY(act("Copy clip")); timeline->selectedTrack = 0; c.seek(frames(12)); QVERIFY(act("Paste clip at playhead"));
        QCOMPARE(c.project.show.clips.size(), std::size_t{4}); QCOMPARE(c.project.show.clips.back().start, frames(12));
        c.seek(frames(5)); QVERIFY(act("Next cue")); QCOMPARE(c.position(), frames(9)); QCOMPARE(timeline->selectedMarker, QString("b"));
        QVERIFY(act("Previous cue")); QCOMPARE(c.position(), frames(1));
        QVERIFY(act("Nudge right one second")); QCOMPARE(c.project.show.markers[0].time, frames(2));
        // Markers drag along the ruler.
        timeline->resize(1200, 400); timeline->setZoom(22); timeline->horizontalScrollBar()->setValue(0);
        auto* viewport = timeline->viewport(); const int x = 190 + 2 * 22;
        QTest::mousePress(viewport, Qt::LeftButton, {}, QPoint(x, 40));
        QMouseEvent move(QEvent::MouseMove, QPointF(x + 44, 40), viewport->mapToGlobal(QPointF(x + 44, 40)), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &move);
        QTest::mouseRelease(viewport, Qt::LeftButton, {}, QPoint(x + 44, 40));
        QCOMPARE(c.project.show.markers[0].time, frames(4));
        c.dirty = false;
    }
    void showModeEscapeNeedsTwoPresses() {
        MainWindow window; window.show(); auto& c = window.controller();
        const auto play = [&] { QVERIFY(c.transport.command({Action::Play})); c.transport.render(nullptr, 1, 0); QCOMPARE(c.transport.state(), State::Playing); };
        play(); QTest::keyClick(&window, Qt::Key_Escape); QCOMPARE(c.transport.state(), State::Stopped); // One press outside Show mode.
        c.setShowMode(true); play();
        QTest::keyClick(&window, Qt::Key_Escape); c.transport.render(nullptr, 1, 0); QCOMPARE(c.transport.state(), State::Playing);
        QVERIFY(c.status.contains("Escape again"));
        QTest::keyClick(&window, Qt::Key_Escape); c.transport.render(nullptr, 1, 0); QCOMPARE(c.transport.state(), State::Stopped);
        play(); QTest::keyClick(&window, Qt::Key_Escape); QTest::qWait(700); QTest::keyClick(&window, Qt::Key_Escape);
        c.transport.render(nullptr, 1, 0); QCOMPARE(c.transport.state(), State::Playing); // Presses too far apart.
        c.stopAll(); c.setShowMode(false); c.dirty = false;
    }
    void inspectorTimesUseTimecode() {
        MainWindow window; window.show(); auto& c = window.controller();
        QVERIFY(c.edit([](Project& p) { Clip clip; clip.id = "t"; clip.text = "Hi"; clip.track = 0; clip.start = frames(1) + 7; clip.duration = frames(4); p.show.clips.push_back(clip); }));
        window.findChild<Timeline*>()->selectClip("t");
        auto* start = window.findChild<QDoubleSpinBox*>("clipStart"); QVERIFY(start);
        QCOMPARE(start->text(), QString("00:00:01:00"));
        // Leaving the field unedited keeps the sub-frame start.
        start->setFocus(); QVERIFY(QMetaObject::invokeMethod(start, "editingFinished")); QCOMPARE(c.project.show.clips[0].start, frames(1) + 7);
        const auto type = [&](const QString& text) { start->selectAll(); QTest::keyClicks(start, text); QTest::keyClick(start, Qt::Key_Return); };
        type("00:00:03:12"); QCOMPARE(c.project.show.clips[0].start, frames(3.5));
        type("0:05"); QCOMPARE(c.project.show.clips[0].start, frames(5));
        type("2.25"); QCOMPARE(c.project.show.clips[0].start, frames(2.25));
        c.dirty = false;
    }
    void linkedPictureAndSoundMoveTrimSplitAndDeleteTogether() {
        MainWindow window; window.show(); auto& c = window.controller();
        const auto act = [&](const QString& text) { for (auto* a : window.findChildren<QAction*>()) if (a->text() == text) { a->trigger(); return true; } return false; };
        QTemporaryDir dir; writeBytes(dir.filePath("tone.wav"), waveBytes(16, 1, 2, 48000, false, frames(8)));
        QImage still(64, 36, QImage::Format_RGB32); still.fill(Qt::blue); QVERIFY(still.save(dir.filePath("still.png")));
        auto sound = loadMedia(dir.filePath("tone.wav")); auto image = loadMedia(dir.filePath("still.png"));
        QVERIFY(c.edit([&](Project& p) {
            p.assets.insert(sound.id, sound); p.assets.insert(image.id, image);
            Clip picture; picture.id = "pic"; picture.assetId = image.id.toStdString(); picture.track = 0; picture.start = frames(2); picture.duration = frames(4); picture.link = "L";
            Clip audio = picture; audio.id = "snd"; audio.assetId = sound.id.toStdString(); audio.track = 2;
            p.show.clips = {picture, audio};
        }));
        const auto clip = [&](const char* id) { for (const auto& x : c.project.show.clips) if (x.id == id) return x; return Clip{}; };
        auto* timeline = window.findChild<Timeline*>(); timeline->selectClip("pic");
        QVERIFY(act("Nudge right one second")); QCOMPARE(clip("snd").start, frames(3)); QCOMPARE(clip("pic").start, frames(3));
        auto* start = window.findChild<QDoubleSpinBox*>("clipStart"); start->setValue(4); QVERIFY(QMetaObject::invokeMethod(start, "editingFinished"));
        QCOMPARE(clip("snd").start, frames(4));
        c.seek(frames(5)); QVERIFY(act("Split clip at playhead"));
        QCOMPARE(c.project.show.clips.size(), std::size_t{4});
        const auto tails = std::count_if(c.project.show.clips.begin(), c.project.show.clips.end(), [&](const Clip& x) { return x.start == frames(5); });
        QCOMPARE(tails, 2); QCOMPARE(c.project.show.clips[2].link, c.project.show.clips[3].link); QVERIFY(c.project.show.clips[2].link != "L");
        // The link survives saving.
        writeProject(c.project, dir.filePath("linked.savshow")); QCOMPARE(readProject(dir.filePath("linked.savshow")).show.clips[0].link, std::string("L"));
        timeline->selectClip("pic"); window.findChild<Timeline*>()->setFocus(); QVERIFY(act("Delete selected clip / cue"));
        QCOMPARE(c.project.show.clips.size(), std::size_t{2});
        timeline->selectClip(QString::fromStdString(c.project.show.clips[0].id)); QVERIFY(act("Unlink clip"));
        QVERIFY(c.project.show.clips[0].link.empty()); QVERIFY(c.project.show.clips[1].link.empty());
        c.dirty = false;
    }
    void timelineKeyboardNavigationAndDescription() {
        MainWindow window; window.show(); auto& c = window.controller();
        QVERIFY(c.edit([](Project& p) {
            for (int i = 0; i < 3; ++i) { Clip clip; clip.id = "t" + std::to_string(i); clip.text = "Slide " + std::to_string(i); clip.track = 1; clip.start = frames(2 + 5 * i); clip.duration = frames(4); p.show.clips.push_back(clip); }
        }));
        auto* timeline = window.findChild<Timeline*>(); timeline->setFocus(); QTRY_VERIFY(timeline->hasFocus());
        QCOMPARE(timeline->selectedTrack, 0);
        QTest::keyClick(timeline, Qt::Key_Down); QCOMPARE(timeline->selectedTrack, 1);
        QVERIFY(timeline->accessibleDescription().startsWith("Track 2 of"));
        QTest::keyClick(timeline, Qt::Key_Right); QCOMPARE(timeline->selectedClip, QString("t0"));
        QTest::keyClick(timeline, Qt::Key_Right); QCOMPARE(timeline->selectedClip, QString("t1"));
        QVERIFY(timeline->accessibleDescription().startsWith("Clip Slide 1 on")); QVERIFY(timeline->accessibleDescription().contains("starts 00:00:07:00"));
        QTest::keyClick(timeline, Qt::Key_Left); QCOMPARE(timeline->selectedClip, QString("t0"));
        QTest::keyClick(timeline, Qt::Key_End); QCOMPARE(c.position(), c.project.show.duration);
        QTest::keyClick(timeline, Qt::Key_Home); QCOMPARE(c.position(), Frame{0});
        c.dirty = false;
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
    void videoErrorsNeverStopPlayback() {
        Controller c; auto project = newProject();
        MediaAsset video; video.id = "broken"; video.name = "Broken.mov"; video.video = true; video.duration = 10; video.path = "/nonexistent/broken.mov";
        project.assets.insert(video.id, video);
        Clip clip; clip.id = "v"; clip.assetId = "broken"; clip.track = 0; clip.duration = frames(10); project.show.clips.push_back(clip);
        c.replace(project); QVERIFY(c.transport.command({Action::Play})); c.transport.render(nullptr, 1, 0);
        QTRY_VERIFY_WITH_TIMEOUT((c.compose(), c.status.contains("Video error in Broken.mov")), 5000);
        c.transport.render(nullptr, 1, 0); QCOMPARE(c.transport.state(), State::Playing); QCOMPARE(c.playbackProblem, c.status);
        c.report("Later message"); c.compose(); QCOMPARE(c.status, QString("Later message"));
        c.stopAll();
    }
    void onlyTimelineVideosCanBlockPlayback() {
        Controller c; auto project = newProject();
        MediaAsset unused; unused.id = "unused"; unused.name = "Unused"; unused.video = true; unused.duration = 1; unused.path = "/nonexistent/unused.mov";
        project.assets.insert(unused.id, unused); c.replace(project);
        QVERIFY(c.missingMedia().isEmpty());
        QVERIFY(c.edit([](Project& p) { Clip clip; clip.id = "v"; clip.assetId = "unused"; clip.track = 0; clip.duration = frames(1); p.show.clips.push_back(clip); }));
        QCOMPARE(c.missingMedia(), QStringList{"/nonexistent/unused.mov"});
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
        // Configuring MIDI clears only the problem MIDI reported, even when another one mentions "MIDI".
        Controller other; auto missing = newProject();
        MediaAsset video; video.id = "cue"; video.name = "Cue"; video.video = true; video.duration = 1; video.path = "/MIDI/cue.mov";
        missing.assets.insert(video.id, video); missing.show.clips.clear(); missing.show.markers.clear();
        Clip clip; clip.id = "v"; clip.assetId = "cue"; clip.track = 0; clip.duration = frames(1); missing.show.clips.push_back(clip);
        other.replace(missing); other.toggle(); QVERIFY(other.playbackProblem.contains("/MIDI/cue.mov"));
        QVERIFY(other.configureMidi(-1)); QVERIFY(other.playbackProblem.contains("/MIDI/cue.mov"));
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
    void playbackSettingsInOutputs() {
        QSettings().setValue("audio/late", "gap");
        QTemporaryDir dir; QDir caches(detail::audioCacheDirectory());
        std::shared_ptr<const AudioData> held;
        const auto unused = importCacheKey(dir.filePath("unused.wav"), 4410), inUse = importCacheKey(dir.filePath("held.wav"), 4420, &held);
        MainWindow window; window.show(); bool shown = false, cleared = false;
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = window.findChild<QDialog*>("outputSettingsDialog"); if (!dialog) return;
            auto* sources = dialog->findChild<QComboBox*>("audioSourcePreparation");
            auto* late = dialog->findChild<QComboBox*>("lateAudioPolicy");
            shown = sources && late && sources->currentData().toInt() == static_cast<int>(AudioPreparation::Prepared) && sources->toolTip().contains("Streaming") &&
                late->currentData().toInt() == static_cast<int>(LateAudio::Gap);
            if (late) { late->setCurrentIndex(late->findData(static_cast<int>(LateAudio::Freeze))); emit late->activated(late->currentIndex()); }
            // Clearing keeps caches in use and reports what it freed.
            if (auto* clear = dialog->findChild<QPushButton*>("clearAudioCache")) {
                clear->click();
                cleared = !QFileInfo::exists(caches.filePath(unused + ".pcm")) && QFileInfo::exists(caches.filePath(inUse + ".pcm")) &&
                    clear->text().startsWith("Clear audio cache · ");
            }
            dialog->reject();
        });
        window.findChild<QPushButton*>("outputSettingsButton")->click();
        QVERIFY(shown); QVERIFY(cleared); QCOMPARE(QSettings().value("audio/late").toString(), QString("freeze"));
        QVERIFY(std::isfinite(sampleAt(*held, 100)));
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
    void savingAnOpenedPackageCopiesMediaOffTheGuiThread() {
        QTemporaryDir dir; const auto package = dir.filePath("demo.savpack"); writeProjectPackage(demoProject(), package);
        MainWindow window; window.openPath(package); QTRY_VERIFY_WITH_TIMEOUT(!window.controller().busy, 15000);
        auto& c = window.controller(); QVERIFY(c.savingCopiesMedia());
        c.path = dir.filePath("copy.savshow"); QAction* save = nullptr;
        for (auto* action : window.findChildren<QAction*>()) if (action->text() == "Save") save = action;
        QVERIFY(save); save->trigger();
        QVERIFY2(!c.dirty, qPrintable(c.status)); QVERIFY(!c.busy); QVERIFY(!c.savingCopiesMedia());
        for (const auto& asset : c.project.assets) { QVERIFY(asset.path.startsWith(dir.filePath("copy.media") + "/")); QVERIFY(QFileInfo::exists(asset.path)); }
        QCOMPARE(readProject(c.path).assets.size(), c.project.assets.size());
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
        // Missing media opens offline; clips keep their places until relinked.
        QCOMPARE(failure(missing), QString());
        const auto offline = readProject(dir.filePath("typed.savshow"));
        for (const auto& asset : offline.assets) { QVERIFY(asset.offline); QVERIFY(asset.problem.startsWith("Media file is missing")); }
        QCOMPARE(offline.show.clips.size(), loaded.show.clips.size());
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
    void asyncVideoKeepsPlayingThroughHolds() {
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg"); if (ffmpeg.isEmpty()) QSKIP("ffmpeg fixture generation unavailable");
        QTemporaryDir dir; const auto source = dir.filePath("change.mkv");
        QProcess process; process.start(ffmpeg, {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "color=red:s=320x180:r=24:d=1",
            "-vf", "drawbox=x=0:y=0:w=iw:h=ih:color=blue:t=fill:enable='gte(t,0.5)'", "-c:v", "ffv1", source});
        QVERIFY(process.waitForFinished(15000)); QCOMPARE(process.exitCode(), 0);
        for (const bool async : {true, false}) {
            Project p = newProject(); p.width = 320; p.height = 240; p.show.tracks[1].async = async;
            auto asset = loadMedia(source); QVERIFY(asset.video); p.assets.insert(asset.id, asset);
            Clip picture; picture.id = newId().toStdString(); picture.assetId = asset.id.toStdString(); picture.track = 1; picture.duration = frames(1);
            p.show.clips.push_back(picture); p.show.markers.push_back({newId().toStdString(), "Hold", frames(.1), true});
            Controller c; c.replace(p);
            QVERIFY(c.transport.command({Action::Play})); c.transport.render(nullptr, static_cast<unsigned>(frames(.75)), 0);
            QCOMPARE(c.transport.state(), State::Held); QCOMPARE(c.position(), frames(.1));
            QCOMPARE(c.transport.asyncVoices(), async ? 1 : 0);
            if (async) QTRY_VERIFY_WITH_TIMEOUT(c.compose().pixelColor(160, 120).blue() > 200, 3000);
            else QTRY_VERIFY_WITH_TIMEOUT(c.compose().pixelColor(160, 120).red() > 200, 3000);
            c.transport.render(nullptr, static_cast<unsigned>(frames(1)), 0);
            QCOMPARE(c.transport.asyncSource(0), async ? Transport::finishedSource : Transport::dormantSource);
            if (async) QTRY_VERIFY_WITH_TIMEOUT(c.compose().pixelColor(160, 120).blue() > 200, 3000); // A finished one-shot keeps its last frame.
        }
    }
    void asyncPictureLayersNeedAsyncLinkedSound() {
        Project p = newProject(); p.show.tracks[1].async = true;
        MediaAsset video; video.id = "video"; video.name = "Clip.mov"; video.video = true; video.duration = 4; video.path = "/nonexistent/clip.mov";
        video.audio = std::make_shared<AudioData>(AudioData{std::vector<float>(static_cast<std::size_t>(frames(4)) * 2)});
        p.assets.insert(video.id, video);
        Clip picture; picture.id = "picture"; picture.assetId = "video"; picture.track = 1; picture.duration = frames(4); picture.link = "L";
        Clip sound = picture; sound.id = "sound"; sound.track = 2; p.show.clips = {picture, sound}; bindAudio(p);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, validate(p));
        p.show.clips[1].track = 3; validate(p); // Ambience is async.
        p.show.tracks[1].async = false; validate(p);
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
        // Preflight lists the missing destination; the operator may still enter Show mode.
        QString preflight;
        QTimer::singleShot(0, &window, [&] {
            auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()); if (!box) return;
            preflight = box->informativeText();
            for (auto* button : box->buttons()) if (button->text() == "Enter Show mode anyway") button->click();
        });
        QVERIFY(find("Show mode")); QTest::mouseClick(find("Show mode"), Qt::LeftButton); QVERIFY(window.controller().showMode);
        QVERIFY(preflight.contains("MIDI cues need a destination")); QVERIFY(preflight.contains("unsaved changes"));
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
    void programOutputUsesSecondaryScreen() {
        if (qEnvironmentVariable("SAV_TEST_DISPLAYS") != "1") QSKIP("Set SAV_TEST_DISPLAYS=1 with two displays and the native platform plugin");
        MainWindow window; window.show();
        auto* editorScreen = window.screen();
        const auto screens = QGuiApplication::screens();
        QScreen* target = nullptr;
        int targetIndex = -1;
        for (int i = 0; i < screens.size(); ++i) if (screens[i] != editorScreen) { target = screens[i]; targetIndex = i; break; }
        QVERIFY2(target, "This test requires a second display distinct from the editor's display");
        auto* outputs = window.findChild<QPushButton*>("outputSettingsButton"); QVERIFY(outputs);
        for (int attempt = 0; attempt < 2; ++attempt) {
            bool selected = false;
            QTimer::singleShot(0, &window, [&] {
                auto* dialog = window.findChild<QDialog*>("outputSettingsDialog"); if (!dialog) return;
                auto* displays = dialog->findChild<QComboBox*>("audienceOutputDisplay");
                auto* open = dialog->findChild<QPushButton*>("openProgramDisplay");
                if (displays && open) {
                    const int item = displays->findData(targetIndex);
                    if (item >= 0) { displays->setCurrentIndex(item); open->click(); selected = true; }
                }
                dialog->reject();
            });
            outputs->click(); QVERIFY(selected);
            QWidget* program = nullptr;
            for (auto* widget : window.findChildren<QWidget*>()) if (widget->windowTitle() == "SimpleAVShow — Program") { program = widget; break; }
            QVERIFY(program); QTRY_VERIFY(program->isVisible()); QVERIFY(program->isFullScreen());
            QTRY_COMPARE(program->screen(), target);
            QTRY_COMPARE(program->geometry(), target->geometry());
            QTRY_COMPARE(program->windowHandle()->geometry(), target->geometry());
            QVERIFY(window.isVisible()); QCOMPARE(window.screen(), editorScreen);
            program->hide();
        }
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
        // A seek during playback fades the new position in; it reaches full level after the fade.
        output.resize(Transport::fadeFrames * 2);
        transport.command({Action::Seek, 7}); transport.render(output.data(), Transport::fadeFrames, 2);
        QCOMPARE(output[(Transport::fadeFrames - 1) * 2], waveValue(clip.offset + 6 + Transport::fadeFrames, 0));
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
        // Same content at another path shares the artifact without rewriting it;
        // reuse only touches the metadata timestamp for least-recently-used trimming.
        QVERIFY(metaFile.open(QIODevice::ReadOnly)); const auto metaBytes = metaFile.readAll(); metaFile.close();
        const auto moved = dir.filePath("moved.wav"); QVERIFY(QFile::copy(source, moved));
        auto reused = loadMedia(moved);
        QCOMPARE(QFileInfo(pcm).lastModified(), sentinel); QVERIFY(QFileInfo(metadata).lastModified() > sentinel);
        QVERIFY(metaFile.open(QIODevice::ReadOnly)); QCOMPARE(metaFile.readAll(), metaBytes); metaFile.close();
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
        // Source identity uses all bytes; a same-size rewrite with a new timestamp is rehashed.
        const auto sourceTime = QFileInfo(source).lastModified();
        auto changed = original; changed[changed.size() - 8] ^= 0x40; writeBytes(source, changed);
        QFile sourceFile(source); QVERIFY(sourceFile.open(QIODevice::ReadWrite));
        QVERIFY(sourceFile.setFileTime(sourceTime.addSecs(-10), QFileDevice::FileModificationTime)); sourceFile.close();
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
    void audioCacheTrimRemovesLeastRecentlyUsedUnlockedCaches() {
        QTemporaryDir dir; QDir caches(detail::audioCacheDirectory());
        std::shared_ptr<const AudioData> held;
        const auto oldest = importCacheKey(dir.filePath("a.wav"), 4410), older = importCacheKey(dir.filePath("b.wav"), 4420);
        const auto newer = importCacheKey(dir.filePath("c.wav"), 4430), inUse = importCacheKey(dir.filePath("d.wav"), 4440, &held);
        const auto locked = importCacheKey(dir.filePath("e.wav"), 4450);
        const auto now = QDateTime::currentDateTimeUtc();
        // Every other cache is older than these; the held and locked ones are the least recently used.
        setModified(caches.filePath(oldest + ".meta"), now.addSecs(3600)); setModified(caches.filePath(older + ".meta"), now.addSecs(7200));
        setModified(caches.filePath(newer + ".meta"), now.addSecs(10800));
        for (const auto& key : {inUse, locked}) setModified(caches.filePath(key + ".meta"), now.addDays(-1));
        QCOMPARE(caches.entryList({inUse + ".*.use"}, QDir::Files).size(), 1);
        QLockFile preparing(caches.filePath(locked + ".lock")); QVERIFY(preparing.tryLock(0));
        qint64 budget = 0;
        for (const auto& key : {older, newer, inUse, locked})
            budget += QFileInfo(caches.filePath(key + ".pcm")).size() + QFileInfo(caches.filePath(key + ".meta")).size();
        detail::trimAudioCache(budget);
        QVERIFY(!QFileInfo::exists(caches.filePath(oldest + ".pcm"))); QVERIFY(!QFileInfo::exists(caches.filePath(oldest + ".meta")));
        for (const auto& key : {older, newer, inUse, locked}) {
            QVERIFY(QFileInfo::exists(caches.filePath(key + ".pcm"))); QVERIFY(QFileInfo::exists(caches.filePath(key + ".meta")));
        }
        QVERIFY(std::isfinite(sampleAt(*held, 100)));
        // Released and unlocked caches become removable again; markers disappear with their owners.
        held.reset(); preparing.unlock();
        QCOMPARE(caches.entryList({inUse + ".*.use"}, QDir::Files).size(), 0);
        detail::trimAudioCache(0);
        QCOMPARE(caches.entryList({"*.pcm", "*.meta", "*.lock", "*.use"}, QDir::Files).size(), 0);
    }
    void audioCacheTrimExpiresStaleRevisions() {
        QTemporaryDir dir; QDir caches(detail::audioCacheDirectory());
        const auto expired = importCacheKey(dir.filePath("a.wav"), 4410), recent = importCacheKey(dir.filePath("b.wav"), 4420);
        const auto current = importCacheKey(dir.filePath("c.wav"), 4430), legacy = importCacheKey(dir.filePath("d.wav"), 4440);
        const auto now = QDateTime::currentDateTimeUtc();
        // Another decoder revision or FFmpeg build is stale; only a week without use expires it.
        for (const auto& key : {expired, recent}) {
            QFile meta(caches.filePath(key + ".meta")); QVERIFY(meta.open(QIODevice::ReadWrite));
            QVERIFY(meta.seek(8)); QCOMPARE(meta.write("\xff", 1), 1); meta.close();
        }
        setModified(caches.filePath(expired + ".meta"), now.addDays(-8)); setModified(caches.filePath(recent + ".meta"), now.addDays(-6));
        setModified(caches.filePath(current + ".meta"), now.addDays(-30));
        // Metadata from before revisions were recorded counts as stale once unused for a week.
        QFile meta(caches.filePath(legacy + ".meta")); QVERIFY(meta.open(QIODevice::ReadOnly));
        auto bytes = meta.readAll(); meta.close(); bytes.remove(8, 32); bytes.replace(0, 8, "SAVPCM01");
        const auto orphan = QString(64, u'f'); writeBytes(caches.filePath(orphan + ".meta"), bytes);
        writeBytes(caches.filePath(orphan + ".pcm"), QByteArray(16, '\0'));
        for (const auto& name : {orphan + ".meta", orphan + ".pcm"}) setModified(caches.filePath(name), now.addDays(-8));
        writeBytes(caches.filePath("preparing-crashed"), "x"); setModified(caches.filePath("preparing-crashed"), now.addDays(-2));
        detail::trimAudioCache(std::numeric_limits<qint64>::max());
        QVERIFY(!QFileInfo::exists(caches.filePath(expired + ".meta"))); QVERIFY(!QFileInfo::exists(caches.filePath(expired + ".pcm")));
        QVERIFY(!QFileInfo::exists(caches.filePath(orphan + ".meta"))); QVERIFY(!QFileInfo::exists(caches.filePath("preparing-crashed")));
        for (const auto& key : {recent, current}) QVERIFY(QFileInfo::exists(caches.filePath(key + ".pcm")));
        // Legacy metadata at a current key is still valid and upgraded in place when reused.
        writeBytes(caches.filePath(legacy + ".meta"), bytes);
        const auto pcmTime = QFileInfo(caches.filePath(legacy + ".pcm")).lastModified();
        QVERIFY(loadMedia(dir.filePath("d.wav")).audio);
        QCOMPARE(QFileInfo(caches.filePath(legacy + ".pcm")).lastModified(), pcmTime);
        QVERIFY(meta.open(QIODevice::ReadOnly)); QCOMPARE(meta.read(8), QByteArray("SAVMETA2")); meta.close();
    }
    void unchangedSourceStampSkipsRehashing() {
        QTemporaryDir dir; QDir caches(detail::audioCacheDirectory()); const auto source = dir.filePath("indexed.wav");
        importCacheKey(source, 4410);
        const auto original = QDateTime::currentDateTimeUtc().addDays(-1); setModified(source, original);
        auto first = loadMedia(source); const auto value = sampleAt(*first.audio, 2000); first.audio.reset();
        QVERIFY(QFileInfo::exists(caches.filePath("source-index")));
        auto rewrite = [&](const QDateTime& time) {
            QFile file(source); QVERIFY(file.open(QIODevice::ReadWrite)); QVERIFY(file.seek(file.size() - 4410 * 4));
            const auto bytes = file.read(4410 * 4); QVERIFY(file.seek(file.size() - 4410 * 4));
            auto changed = bytes; for (auto& byte : changed) byte = static_cast<char>(~byte);
            QCOMPARE(file.write(changed), changed.size()); file.close(); setModified(source, time);
        };
        // Tradeoff: a same-size rewrite that keeps the timestamp is not rehashed and reuses the old cache.
        const auto count = caches.entryList({"*.pcm"}, QDir::Files).size();
        rewrite(original);
        auto stale = loadMedia(source); QCOMPARE(caches.entryList({"*.pcm"}, QDir::Files).size(), count);
        QCOMPARE(sampleAt(*stale.audio, 2000), value); stale.audio.reset();
        // Any timestamp (or size) change hashes the whole file again.
        setModified(source, original.addSecs(4)); // More than FAT's 2 s step.
        auto fresh = loadMedia(source); QCOMPARE(caches.entryList({"*.pcm"}, QDir::Files).size(), count + 1);
        QVERIFY(sampleAt(*fresh.audio, 2000) != value);
        // Files modified within the timestamp granularity are never indexed, so they are always rehashed.
        // Ahead of the clock, so the file stays recent however long the test takes (FAT: 2 s steps).
        const auto recent = dir.filePath("recent.wav");
        auto recentBytes = waveBytes(16, 1, 2, 44100, false, 4420); recentBytes.replace(recentBytes.size() - 4, 4, QUuid::createUuid().toRfc4122().first(4));
        writeBytes(recent, recentBytes); setModified(recent, QDateTime::currentDateTime().addSecs(60));
        const auto recentTime = QFileInfo(recent).lastModified();
        QVERIFY(loadMedia(recent).audio); QCOMPARE(caches.entryList({"*.pcm"}, QDir::Files).size(), count + 2);
        QFile file(recent); QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.seek(file.size() - 2)); QCOMPARE(file.write("\x01\x02", 2), 2); file.close(); setModified(recent, recentTime);
        QVERIFY(loadMedia(recent).audio); QCOMPARE(caches.entryList({"*.pcm"}, QDir::Files).size(), count + 3);
    }
    void directPcmSidecarSkipsHashingAndKeepsChangeDetection() {
        QTemporaryDir dir; QDir caches(detail::audioCacheDirectory()); const auto source = dir.filePath("direct.wav");
        writeBytes(source, waveBytes(16, 1, 2, sampleRate));
        const auto time = QDateTime::currentDateTimeUtc().addDays(-1); setModified(source, time);
        const auto before = caches.entryList({"*.wave"}, QDir::Files).size();
        const auto first = loadMedia(source); QCOMPARE(caches.entryList({"*.wave"}, QDir::Files).size(), before + 1);
        const auto reopened = loadMedia(source); QCOMPARE(reopened.audio->peaks, first.audio->peaks);
        QCOMPARE(sampleAt(*reopened.audio, 9000), waveValue(9000, 0));
        // A same-stamp rewrite reuses the recorded hashes, so reads still detect it and drop the sidecar.
        QFile file(source); QVERIFY(file.open(QIODevice::ReadWrite)); QVERIFY(file.seek(file.size() - 4));
        QCOMPARE(file.write("\x00\x40\x00\x40", 4), 4); file.close(); setModified(source, time);
        const auto stale = loadMedia(source); QCOMPARE(stale.audio->peaks, first.audio->peaks);
        QVERIFY_THROWS_EXCEPTION(AudioSourceChanged, sampleAt(*stale.audio, stale.audio->length() - 1));
        QCOMPARE(caches.entryList({"*.wave"}, QDir::Files).size(), before);
        const auto fresh = loadMedia(source); QCOMPARE(sampleAt(*fresh.audio, fresh.audio->length() - 1), .5f);
        QCOMPARE(caches.entryList({"*.wave"}, QDir::Files).size(), before + 1);
    }
    void streamedAudioMatchesPreparedCache_data() {
        QTest::addColumn<QStringList>("encode");
        // Exact rows must match bit for bit after any seek. Some decoders depend on everything decoded before:
        // AC-3 dithers quiet bands from a random generator (like AAC noise substitution), MP2 and IMA ADPCM
        // carry predictor state across packets. Their rows must land on the same frames, within a small error.
        QTest::addColumn<bool>("exact");
        QTest::newRow("flac-44k-stereo") << QStringList{"-ar", "44100", "-c:a", "flac", "x.flac"} << true;
        QTest::newRow("wav-44k-resampled") << QStringList{"-ar", "44100", "-c:a", "pcm_s16le", "x.wav"} << true;
        QTest::newRow("mp3-44k-mono") << QStringList{"-ar", "44100", "-ac", "1", "-c:a", "libmp3lame", "-b:a", "192k", "x.mp3"} << true;
        // AAC noise substitution draws from a generator whose state depends on where decoding began, so
        // noise bands differ (inaudibly) after any seek; without it, placement must be exact.
        QTest::newRow("aac-48k-m4a") << QStringList{"-ar", "48000", "-c:a", "aac", "-aac_pns", "0", "-b:a", "192k", "x.m4a"} << true;
        QTest::newRow("opus-ogg") << QStringList{"-c:a", "libopus", "-b:a", "128k", "x.opus"} << true;
        QTest::newRow("opus-webm-ms") << QStringList{"-c:a", "libopus", "-b:a", "128k", "x.webm"} << true;
        QTest::newRow("aac-mka-44k-ms") << QStringList{"-ar", "44100", "-c:a", "aac", "-aac_pns", "0", "x.mka"} << true;
        QTest::newRow("aac-adts-32k") << QStringList{"-ar", "32000", "-c:a", "aac", "-aac_pns", "0", "x.aac"} << true;
        QTest::newRow("mp3-22k-stereo") << QStringList{"-ar", "22050", "-c:a", "libmp3lame", "x.mp3"} << true;
        QTest::newRow("wav-32k-short-mono") << QStringList{"-t", "0.03", "-ar", "32000", "-ac", "1", "-c:a", "pcm_s16le", "x.wav"} << true;
        QTest::newRow("wav-22k-stereo") << QStringList{"-ar", "22050", "-c:a", "pcm_s16le", "x.wav"} << true;
        QTest::newRow("vorbis-32k") << QStringList{"-ar", "32000", "-c:a", "libvorbis", "x.ogg"} << true;
        QTest::newRow("ac3-48k") << QStringList{"-c:a", "ac3", "x.ac3"} << false;
        QTest::newRow("eac3-48k") << QStringList{"-c:a", "eac3", "x.eac3"} << false;
        QTest::newRow("mp2-44k") << QStringList{"-ar", "44100", "-c:a", "mp2", "x.mp2"} << false;
        // G.722 in WAV: after a byte seek FFmpeg computes timestamps a linear decode gave other packets.
        QTest::newRow("g722-wav") << QStringList{"-ar", "16000", "-ac", "1", "-c:a", "g722", "x.wav"} << false;
        QTest::newRow("ima-adpcm-mov") << QStringList{"-ar", "44100", "-c:a", "adpcm_ima_qt", "x.mov"} << false;
        QTest::newRow("mp3-in-mpeg-ps") << QStringList{"-ar", "44100", "-c:a", "libmp3lame", "-f", "vob", "x.mpg"} << true;
        QTest::newRow("vorbis-native-44k") << QStringList{"-ar", "44100", "-c:a", "vorbis", "-strict", "experimental", "x.ogg"} << true;
    }
    void streamedAudioMatchesPreparedCache() {
        QFETCH(QStringList, encode); QFETCH(bool, exact);
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg"); if (ffmpeg.isEmpty()) QSKIP("ffmpeg fixture generation unavailable");
        QTemporaryDir dir; const QString file = dir.filePath(encode.takeLast()); QProcess process;
        // A sweep with noise exercises every band of the codec and resampler.
        process.start(ffmpeg, QStringList{"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
            "aevalsrc=0.4*sin(2*PI*(100+800*t)*t)+0.1*random(0)|0.3*sin(2*PI*660*t)+0.1*random(1):s=48000:d=12"} + encode + QStringList{file});
        QVERIFY(process.waitForFinished(30000));
        if (process.exitCode() != 0) QSKIP(qPrintable("encoder unavailable: " + QString::fromUtf8(process.readAllStandardError())));
        const auto prepared = loadMedia(file).audio; QVERIFY(prepared); QVERIFY(!prepared->storage->decodes());
        setAudioPreparation(AudioPreparation::Stream);
        QDir caches(detail::audioCacheDirectory()); const auto pcm = caches.entryList({"*.pcm"}, QDir::Files);
        const auto streamed = loadMedia(file).audio; QVERIFY(streamed); QVERIFY(streamed->storage->decodes());
        QCOMPARE(caches.entryList({"*.pcm"}, QDir::Files), pcm); // Streaming writes no PCM.
        QCOMPARE(streamed->length(), prepared->length()); QCOMPARE(streamed->mono, prepared->mono); QCOMPARE(streamed->peaks, prepared->peaks);
        // Out of order: sequential continuation, forward skips, seeks back, and more places than decoders.
        const Frame length = streamed->length();
        const std::vector<Frame> starts{0, 1000, 1512, 8191, frames(3) + 7, frames(7) + 12345, 400, frames(9), frames(2), frames(5) + 1, frames(11), length - 600, 30, length - 1};
        for (const Frame start : starts) {
            if (start < 0 || start >= length) continue;
            const auto count = static_cast<std::size_t>(std::min<Frame>(512, length - start));
            std::vector<float> a(count * 2), b(count * 2);
            prepared->storage->read(start, a.data(), count); streamed->storage->read(start, b.data(), count);
            if (!exact) {
                // Placement: no shift fits better than none; level: differences stay small.
                const auto error = [&](int lag) { double e = 0; for (std::size_t i = 64; i + 64 < count; ++i) e += std::abs(a[i * 2] - b[(i + lag) * 2]); return e; };
                for (int lag = -48; lag <= 48 && count > 256; ++lag) if (lag) QVERIFY2(error(lag) > error(0), qPrintable(QString("read at %1 fits shifted by %2").arg(start).arg(lag)));
                for (std::size_t i = 0; i < a.size(); ++i) QVERIFY2(std::abs(a[i] - b[i]) < .1f, qPrintable(QString("read at %1 differs by %2").arg(start).arg(a[i] - b[i])));
                continue;
            }
            const auto diff = std::mismatch(a.begin(), a.end(), b.begin(), [](float x, float y) {
                return std::bit_cast<quint32>(x) == std::bit_cast<quint32>(y);
            });
            if (diff.first != a.end()) {
                const auto sample = diff.first - a.begin();
                QFAIL(qPrintable(QString("read at %1: first mismatch at frame %2 channel %3 (%4 vs %5)")
                    .arg(start).arg(start + sample / 2).arg(sample % 2).arg(*diff.first, 0, 'g', 9).arg(*diff.second, 0, 'g', 9)));
            }
        }
        // Reopening reuses the waveform summary.
        QCOMPARE(loadMedia(file).audio->peaks, streamed->peaks);
        QCOMPARE(caches.entryList({"*.stream"}, QDir::Files).isEmpty(), false);
    }
    void streamedAudioPlaysAndDetectsChanges() {
        setAudioPreparation(AudioPreparation::Stream);
        QTemporaryDir dir; const auto path = dir.filePath("resample.wav");
        writeBytes(path, waveBytes(16, 1, 2, 44100, false, 44100 * 3));
        const auto asset = loadMedia(path); QVERIFY(asset.audio->storage->decodes()); QCOMPARE(asset.audio->length(), frames(3));
        auto show = std::make_shared<Show>(); show->duration = asset.audio->length();
        show->tracks.push_back({"audio", "Audio", false, false, false, 1, 0, 0, 1});
        Clip clip; clip.audio = asset.audio; clip.duration = show->duration; show->clips.push_back(clip);
        Transport transport; transport.configure(show); QVERIFY(!transport.audioReadFailed()); transport.command({Action::Play});
        std::vector<float> out(4000); transport.render(out.data(), 2000, 2);
        QCOMPARE(transport.bufferWaits(), 0u);
        for (Frame i = 0; i < 2000; ++i) QCOMPARE(out[i * 2], std::clamp(sampleAt(*asset.audio, i), -1.f, 1.f));
        // A rewritten source is a change, a missing one a retryable read failure.
        const auto time = QFileInfo(path).lastModified();
        writeBytes(path, waveBytes(16, 1, 2, 44100, false, 44100 * 2)); setModified(path, time.addSecs(5));
        float sample[2]; QVERIFY_THROWS_EXCEPTION(AudioSourceChanged, asset.audio->storage->read(10, sample, 1));
        QVERIFY(QFile::remove(path));
        try { asset.audio->storage->read(10, sample, 1); QFAIL("missing source read"); }
        catch (const AudioSourceChanged&) { QFAIL("a missing source can come back"); } catch (const std::runtime_error&) {}
    }
    void streamSummaryRejectsStaleIndexedHash() {
        // An in-place rewrite that keeps size, time and identity reuses the indexed hash of the old
        // content; the summary would then be filed under content other copies share.
        setAudioPreparation(AudioPreparation::Stream);
        QTemporaryDir dir; const auto path = dir.filePath("rewritten.wav");
        auto bytes = waveBytes(16, 1, 2, 44100, false, 44100); bytes.replace(bytes.size() - 4, 4, QUuid::createUuid().toRfc4122().first(4));
        // Whole even seconds: FAT keeps modification times in 2 s steps.
        auto time = QDateTime::currentDateTime().addSecs(-3600); time = time.addMSecs(-(time.toMSecsSinceEpoch() % 2000));
        writeBytes(path, bytes); setModified(path, time);
        QVERIFY(loadMedia(path).audio);
        detail::trimAudioCache(0); // The summary goes; the source index stays.
        {
            QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite)); QVERIFY(file.seek(bytes.size() - 4000));
            QCOMPARE(file.write(QByteArray(4000, '\x40')), 4000);
        }
        setModified(path, time); // After closing: exFAT updates the time when a written file closes.
        QCOMPARE(QFileInfo(path).size(), bytes.size()); QCOMPARE(QFileInfo(path).lastModified(), time);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, loadMedia(path));
        QVERIFY(QDir(detail::audioCacheDirectory()).entryList({"*.stream"}, QDir::Files).isEmpty());
        QVERIFY(loadMedia(path).audio); // The stale index entry is gone, so the next import hashes again.
    }
    void streamSummaryRechecksRecentSources() {
        // A file modified just before import can be rewritten during it within one timestamp step (2 s on
        // FAT) at the same size; its summary must not be filed under the content hashed at the start.
        QTemporaryDir dir; const auto path = dir.filePath("recent.wav");
        auto bytes = waveBytes(16, 1, 2, 44100, false, 44100); bytes.replace(bytes.size() - 4, 4, QUuid::createUuid().toRfc4122().first(4));
        // Ahead of the clock, so the source stays "recent" however long the test takes (FAT: 2 s steps).
        writeBytes(path, bytes); setModified(path, QDateTime::currentDateTime().addSecs(60)); const auto time = QFileInfo(path).lastModified();
        const detail::StreamSummary summary(path, "wav");
        {
            QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite)); QVERIFY(file.seek(bytes.size() - 4000));
            QCOMPARE(file.write(QByteArray(4000, '\x40')), 4000);
        }
        setModified(path, time); QCOMPARE(QFileInfo(path).lastModified(), time);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, summary.save(detail::AudioSummary{44100, false, {}, {}}));
    }
    void newSummaryIsCheckedAgainstTheFile() {
        // A rewrite during import that keeps an older timestamp (a share whose clock differs) must not
        // file the edited file's summary under the original content.
        QTemporaryDir dir; const auto path = dir.filePath("old.wav");
        auto bytes = waveBytes(16, 1, 2, 44100, false, 44100); bytes.replace(bytes.size() - 4, 4, QUuid::createUuid().toRfc4122().first(4));
        writeBytes(path, bytes); setModified(path, QDateTime::currentDateTime().addSecs(-3600)); const auto time = QFileInfo(path).lastModified();
        const detail::StreamSummary summary(path, "wav");
        {
            QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite)); QVERIFY(file.seek(bytes.size() - 4000));
            QCOMPARE(file.write(QByteArray(4000, '\x40')), 4000);
        }
        setModified(path, time); QCOMPARE(QFileInfo(path).lastModified(), time);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, summary.save(detail::AudioSummary{44100, false, {}, {}}));
    }
    void reusedSummaryRechecksRecentSources() {
        // The same rewrite after hashing must not be accepted with the old summary when one exists already.
        setAudioPreparation(AudioPreparation::Stream);
        QTemporaryDir dir; const auto path = dir.filePath("recent-reuse.wav");
        auto bytes = waveBytes(16, 1, 2, 44100, false, 44100); bytes.replace(bytes.size() - 4, 4, QUuid::createUuid().toRfc4122().first(4));
        writeBytes(path, bytes); setModified(path, QDateTime::currentDateTime().addSecs(60)); const auto time = QFileInfo(path).lastModified();
        QVERIFY(loadMedia(path).audio); // Saves the summary.
        const detail::StreamSummary summary(path, "wav");
        QVERIFY(summary.reuse(false));
        {
            QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite)); QVERIFY(file.seek(bytes.size() - 4000));
            QCOMPARE(file.write(QByteArray(4000, '\x40')), 4000);
        }
        setModified(path, time); QCOMPARE(QFileInfo(path).lastModified(), time);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, summary.verify());
    }
    void streamedAudioCallbackHeadroom() {
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg"); if (ffmpeg.isEmpty()) QSKIP("ffmpeg fixture generation unavailable");
        QTemporaryDir dir; QVERIFY(dir.isValid()); setAudioPreparation(AudioPreparation::Stream);
        std::vector<std::shared_ptr<const AudioData>> sources;
        for (int i = 0; i < 8; ++i) {
            const QString path = dir.filePath(QString::number(i) + (i % 2 ? ".m4a" : ".mp3"));
            const QStringList codec = i % 2 ? QStringList{"-c:a", "aac"} : QStringList{"-c:a", "libmp3lame"};
            QProcess ff;
            ff.start(ffmpeg, QStringList{"-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
                QString("aevalsrc=0.2*sin(2*PI*%1*t)|0.2*sin(2*PI*%2*t):s=48000:d=18").arg(211 + i*37).arg(547 + i*19),
                "-ar", "44100"} + codec + QStringList{"-b:a", "160k", path});
            QVERIFY(ff.waitForFinished(30000));
            const auto errors = ff.readAllStandardError();
            if (errors.contains("Unknown encoder")) QSKIP(qPrintable(QString::fromUtf8(errors)));
            QVERIFY2(ff.exitStatus() == QProcess::NormalExit && ff.exitCode() == 0, errors.constData());
            auto audio = loadMedia(path).audio; QVERIFY(audio && audio->storage->decodes()); sources.push_back(std::move(audio));
        }
        using Clock = std::chrono::steady_clock;
        constexpr unsigned block = 512;
        constexpr auto period = std::chrono::nanoseconds(block * 1000000000LL / sampleRate);
        const auto run = [&](unsigned clips, unsigned seekEvery, bool cold) {
            auto show = std::make_shared<Show>(); show->duration = frames(40);
            show->tracks.push_back({"audio", "Audio"});
            for (unsigned i = 0; i < clips; ++i) {
                Clip clip; clip.id = std::to_string(i); clip.audio = sources[i % sources.size()];
                const Frame offsets[] = {0, frames(3.7), frames(8.3), clip.audio->length() - (2*8192 + 1500),
                    frames(11.1), frames(14.4), frames(6.4), frames(1.5)};
                clip.offset = offsets[(i / sources.size()) % std::size(offsets)];
                clip.loop = true; clip.duration = show->duration; clip.gain = 1. / clips;
                show->clips.push_back(std::move(clip));
            }
            Transport transport; transport.configure(show);
            if (!transport.command({Action::Play})) throw std::runtime_error("Cannot queue Play");
            std::array<float, block*2> out;
            auto next = Clock::now();
            unsigned seeks = 0, deadlineMisses = 0, stalls = 0;
            bool audible = false;
            constexpr unsigned callbacks = 5 * sampleRate / block;
            for (unsigned i = 0; i < callbacks; ++i) {
                std::this_thread::sleep_until(next); next += period;
                Frame expected = transport.position() + block;
                if (seekEvery && i && i % seekEvery == 0) {
                    const Frame destinations[] = {frames(12.2), frames(26.5), frames(6.4), frames(18.9)};
                    const Frame destination = cold ? destinations[seeks % std::size(destinations)] : transport.position() + 97;
                    if (!transport.command({Action::Seek, destination})) throw std::runtime_error("Cannot queue Seek");
                    ++seeks; expected = destination + block;
                }
                transport.render(out.data(), block, 2);
                // Freeze stops the clock on any miss, so position shows stalls the seek grace hides from the counters.
                if (transport.position() != expected) ++stalls;
                audible |= transport.peak() > 0.f;
                if (Clock::now() > next) ++deadlineMisses;
            }
            if (transport.audioReadFailed() || transport.readRetries() || transport.clipped() || !audible)
                throw std::runtime_error("Unhealthy stress fixture playback");
            return std::array{transport.bufferWaits(), seeks, deadlineMisses, stalls};
        };
        const auto baseline = run(32, 12, false);
        QCOMPARE(baseline[0], 0u); QCOMPARE(baseline[3], 0u);
        // Opt-in measurements. Cold jumps deliberately leave the four-block window.
        if (qEnvironmentVariableIsSet("SAV_MEASURE_STREAM_HEADROOM")) {
            const auto measure = [&](unsigned clips, unsigned interval, bool cold) {
                const auto result = run(clips, interval, cold);
                qInfo("stream load: %u clips, %s seeks, %u callbacks/seek, %u seeks, %u waits, %u stalled callbacks, %u callback deadlines missed",
                    clips, cold ? "cold" : "warm", interval, result[1], result[0], result[3], result[2]);
            };
            for (unsigned clips : {32, 64, 128, 256, 384, 512, 768, 1024}) measure(clips, 0, false);
            for (unsigned interval : {48, 12, 3, 1}) measure(32, interval, false);
            for (unsigned interval : {192, 48, 12, 3}) measure(32, interval, true);
            measure(1, 192, true);
        }
    }
    void streamedAudioGapRecoversAtShowTime() {
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg"); if (ffmpeg.isEmpty()) QSKIP("ffmpeg fixture generation unavailable");
        QTemporaryDir dir; QVERIFY(dir.isValid()); const QString path = dir.filePath("gap.mp3");
        QProcess ff; ff.start(ffmpeg, {"-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
            "aevalsrc=0.3*sin(2*PI*233*t)|0.4*sin(2*PI*547*t):s=44100:d=8", "-c:a", "libmp3lame", path});
        QVERIFY(ff.waitForFinished(30000));
        const auto errors = ff.readAllStandardError();
        if (errors.contains("Unknown encoder")) QSKIP(qPrintable(QString::fromUtf8(errors)));
        QVERIFY2(ff.exitStatus() == QProcess::NormalExit && ff.exitCode() == 0, errors.constData());
        const auto reference = loadMedia(path).audio; QVERIFY(reference);
        setAudioPreparation(AudioPreparation::Stream);
        const auto streamed = loadMedia(path).audio; QVERIFY(streamed && streamed->storage->decodes());
        auto audio = std::make_shared<AudioData>(*streamed);
        struct InterruptedStorage final : AudioStorage {
            std::shared_ptr<const AudioStorage> source;
            std::atomic<bool> interrupted{false};
            Frame length() const noexcept override { return source->length(); }
            bool decodes() const noexcept override { return true; }
            void read(Frame first, float* out, std::size_t count) const override {
                if (interrupted.load()) throw std::runtime_error("Temporary streamed read interruption");
                source->read(first, out, count);
            }
        };
        auto storage = std::make_shared<InterruptedStorage>(); storage->source = audio->storage; audio->storage = storage;
        auto resident = std::make_shared<AudioData>(); resident->samples = {.125f, .125f};
        auto show = std::make_shared<Show>(); show->duration = audio->length();
        show->tracks = {{"stream", "Stream"}, {"resident", "Resident", false, true, false, 1, 0, 2, 3}};
        Clip clip; clip.id = "late"; clip.audio = audio; clip.duration = show->duration; show->clips.push_back(clip);
        clip.id = "steady"; clip.track = 1; clip.audio = resident; clip.loop = true; show->clips.push_back(clip);
        Transport transport; transport.setLateAudio(LateAudio::Gap); transport.configure(show);
        QVERIFY(!transport.audioReadFailed());
        constexpr unsigned block = 512;
        std::array<float, block*4> out;
        QVERIFY(transport.command({Action::Play})); transport.render(out.data(), block, 4);
        QVERIFY(transport.peak() > .125f); QCOMPARE(transport.audioGaps(), 0u);
        storage->interrupted.store(true);
        const Frame destination = frames(4) + 301;
        QVERIFY(transport.command({Action::Seek, destination}));
        Frame position = destination;
        auto next = std::chrono::steady_clock::now();
        const auto render = [&] {
            std::this_thread::sleep_until(next); next += std::chrono::nanoseconds(block * 1000000000LL / sampleRate);
            transport.render(out.data(), block, 4); position += block;
        };
        // Gaps right after the seek are expected and uncounted; a source still late after seekGrace counts.
        // The block holding frame seekGrace is the first that counts.
        const unsigned graceBlocks = static_cast<unsigned>(Transport::seekGrace / block);
        for (unsigned i = 0; i < graceBlocks + 6; ++i) {
            render(); QCOMPARE(transport.position(), position); QCOMPARE(transport.audioGaps(), i + 1 > graceBlocks ? i + 1 - graceBlocks : 0u);
            for (unsigned f = 0; f < block; ++f) {
                if (i || f >= Transport::fadeFrames) { QCOMPARE(out[4*f], 0.f); QCOMPARE(out[4*f + 1], 0.f); }
                QCOMPARE(out[4*f + 2], .125f); QCOMPARE(out[4*f + 3], .125f);
            }
        }
        QVERIFY(transport.readRetries() > 0);
        storage->interrupted.store(false);
        bool fadingIn = false, recovered = false;
        for (unsigned i = 0; i < 100; ++i) {
            const auto gaps = transport.audioGaps();
            render(); QCOMPARE(transport.position(), position); QCOMPARE(transport.state(), State::Playing);
            std::array<float, block*2> expected;
            reference->storage->read(position - block, expected.data(), block);
            for (unsigned f = 0; f < block; ++f) { QCOMPARE(out[4*f + 2], .125f); QCOMPARE(out[4*f + 3], .125f); }
            for (unsigned f = 0; f < block && !fadingIn; ++f) {
                if (out[4*f] == 0.f && out[4*f + 1] == 0.f) continue;
                const double attack = 1. - static_cast<double>(Transport::fadeFrames - 1) / Transport::fadeFrames;
                QCOMPARE(out[4*f], static_cast<float>(expected[2*f] * attack));
                QCOMPARE(out[4*f + 1], static_cast<float>(expected[2*f + 1] * attack));
                fadingIn = true;
            }
            if (transport.audioGaps() != gaps) continue;
            for (unsigned f = Transport::fadeFrames; f < block; ++f) {
                QCOMPARE(std::bit_cast<quint32>(out[4*f]), std::bit_cast<quint32>(expected[2*f]));
                QCOMPARE(std::bit_cast<quint32>(out[4*f + 1]), std::bit_cast<quint32>(expected[2*f + 1]));
            }
            recovered = true; break;
        }
        QVERIFY(fadingIn && recovered); QCOMPARE(transport.bufferWaits(), 0u); QVERIFY(!transport.audioReadFailed());
    }
    void playbackPreferencesPersistAndSwapAudioWithoutUndo() {
        QSettings().setValue("audio/sources", "stream"); QSettings().setValue("audio/late", "gap");
        {
            Controller c; QCOMPARE(audioPreparation(), AudioPreparation::Stream); QCOMPARE(c.transport.lateAudio(), LateAudio::Gap);
            c.setLateAudio(LateAudio::Freeze); c.setAudioPreparation(AudioPreparation::Prepared);
        }
        QCOMPARE(QSettings().value("audio/late").toString(), QString("freeze")); QCOMPARE(QSettings().value("audio/sources").toString(), QString("prepared"));
        QTemporaryDir dir; const auto path = dir.filePath("resample.wav"); writeBytes(path, waveBytes(16, 1, 2, 44100, false, 44100 * 2));
        setAudioPreparation(AudioPreparation::Stream); auto streamed = loadMedia(path);
        setAudioPreparation(AudioPreparation::Prepared); const auto prepared = loadMedia(path).audio;
        Controller c; auto project = newProject(); project.show.clips.clear(); project.show.markers.clear(); project.assets.clear();
        project.show.tracks = {{"audio", "Audio", false, false, false, 1, 0, 0, 1}};
        project.assets.insert(streamed.id, streamed);
        Clip clip; clip.id = "a"; clip.assetId = streamed.id.toStdString(); clip.audio = streamed.audio; clip.duration = streamed.audio->length(); project.show.clips.push_back(clip);
        c.replace(project);
        const auto streaming = [&] { return c.preflight().filter("Audio streams from the original media files").size() == 1; };
        QVERIFY(streaming());
        // Prepared is chosen, so this source is one that could not be prepared: the check names it.
        QVERIFY(!c.preflight().filter("preparing them failed").filter(streamed.name).isEmpty());
        setAudioPreparation(AudioPreparation::Stream); QVERIFY(!c.preflight().filter("choose Prepared audio sources").isEmpty());
        setAudioPreparation(AudioPreparation::Prepared);
        {
            // A video's sound on a picture layer never plays, so it is no reason to warn.
            auto pictures = project; auto video = streamed; video.video = true; video.duration = 2; pictures.assets.insert(video.id, video);
            pictures.show.tracks = {{"picture", "Picture", true}}; Controller other; other.replace(pictures);
            QVERIFY(other.project.show.clips[0].audio); QVERIFY(other.preflight().filter("Audio streams from the original media files").isEmpty());
        }
        QVERIFY(c.edit([](Project& p) { p.show.clips[0].gain = .5; }));
        QCOMPARE(c.audioSources(), QStringList{streamed.path});
        QCOMPARE(c.replaceAudio({{streamed.path, prepared}}), Controller::AudioSwap::Applied);
        QVERIFY(!streaming()); QCOMPARE(c.project.show.clips[0].audio, prepared);
        // Swapping storage is not an edit, and undo keeps the chosen preparation.
        c.undo(); QCOMPARE(c.project.show.clips[0].gain, 1.); QCOMPARE(c.project.show.clips[0].audio, prepared);
        // An asset only redo can bring back is still a source to prepare.
        const auto other = dir.filePath("other.wav"); writeBytes(other, waveBytes(16, 1, 2, 44100, false, 44100));
        setAudioPreparation(AudioPreparation::Stream); auto added = loadMedia(other);
        QVERIFY(c.edit([&](Project& p) { p.assets.insert(added.id, added); }));
        c.undo(); QVERIFY(!c.project.assets.contains(added.id)); QVERIFY(c.audioSources().contains(other));
        QVERIFY(c.audioDiffersFrom(AudioPreparation::Prepared)); // Only redo still holds streamed audio.
        setAudioPreparation(AudioPreparation::Prepared); const auto preparedOther = loadMedia(other).audio;
        QCOMPARE(c.replaceAudio({{other, preparedOther}}), Controller::AudioSwap::Applied); QVERIFY(!c.audioDiffersFrom(AudioPreparation::Prepared));
        c.redo(); QCOMPARE(c.project.assets[added.id].audio, preparedOther);
        // A snapshot the new audio would invalidate keeps its previous audio, so undo can still arm it.
        QVERIFY(c.edit([](Project& p) { p.show.clips[0].offset = frames(1.5); p.show.clips[0].duration = frames(.4); }));
        QVERIFY(c.edit([](Project& p) { p.show.clips.clear(); }));
        const auto shorter = loadMedia([&] { const auto f = dir.filePath("short.wav"); writeBytes(f, waveBytes(16, 1, 2, 44100, false, 44100)); return f; }()).audio;
        QCOMPARE(c.replaceAudio({{streamed.path, shorter}}), Controller::AudioSwap::Applied);
        QCOMPARE(c.project.assets[streamed.id].duration, seconds(shorter->length())); // New clips use the reloaded length.
        c.save(dir.filePath("show.savshow")); QVERIFY2(!c.dirty, qPrintable(c.status)); // Saving must not rebind history to the new audio.
        c.undo(); QCOMPARE(c.project.show.clips[0].offset, frames(1.5)); QCOMPARE(c.project.show.clips[0].audio, prepared);
    }
    void manyStreamedSourcesShareABoundedDecoderBudget() {
#ifdef Q_OS_WIN
        QSKIP("counts descriptors through /dev/fd");
#else
        // Every armed clip prebuffers; without a shared budget each source would keep its own file open.
        setAudioPreparation(AudioPreparation::Stream);
        QTemporaryDir dir; auto show = std::make_shared<Show>();
        show->tracks.push_back({"audio", "Audio", false, false, false, 1, 0, 0, 1});
        constexpr int sources = 160;
        for (int i = 0; i < sources; ++i) {
            const auto path = dir.filePath(QString("cue-%1.wav").arg(i)); writeBytes(path, waveBytes(16, 1, 2, 44100, false, 4410 + i));
            Clip clip; clip.audio = loadMedia(path).audio; clip.start = frames(.2) * i; clip.duration = clip.audio->length(); show->clips.push_back(clip);
        }
        show->duration = frames(.2) * sources;
        const auto descriptors = [] { return QDir("/dev/fd").entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot).size(); };
        const auto before = descriptors();
        Transport transport; transport.configure(show);
        QVERIFY(!transport.audioReadFailed());
        QVERIFY2(descriptors() - before <= 72, qPrintable(QString("%1 descriptors opened").arg(descriptors() - before)));
        // Evicted decoders reopen on demand: a later cue still plays exactly.
        transport.command({Action::Seek, show->clips[150].start}); transport.command({Action::Play});
        std::vector<float> out(512 * 2); transport.render(out.data(), 512, 2); // The seek reads its new place cold.
        for (int i = 0; i < 200 && transport.position() == show->clips[150].start; ++i) { QTest::qWait(2); transport.render(out.data(), 512, 2); }
        QVERIFY(!transport.audioReadFailed()); QVERIFY(transport.position() > show->clips[150].start);
#endif
    }
    void saveLeavesARelinkedAssetsHistoryAlone() {
        // Undo after relink and Save restores the asset as it was, not the relinked file.
        QTemporaryDir dir; const auto first = dir.filePath("a.wav"), second = dir.filePath("b.wav");
        writeBytes(first, waveBytes(16, 1, 2, sampleRate, false, 48000)); writeBytes(second, waveBytes(16, 1, 2, sampleRate, false, 96000));
        Controller c; auto project = newProject(); project.show.clips.clear(); project.show.markers.clear(); project.assets.clear();
        project.show.tracks = {{"audio", "Audio", false, false, false, 1, 0, 0, 1}};
        auto original = loadMedia(first); project.assets.insert(original.id, original);
        Clip clip; clip.id = "a"; clip.assetId = original.id.toStdString(); clip.audio = original.audio; clip.duration = original.audio->length(); project.show.clips.push_back(clip);
        c.replace(project);
        auto relinked = loadMedia(second); relinked.id = original.id; relinked.name = original.name;
        QVERIFY(c.edit([&](Project& p) { p.assets.insert(relinked.id, relinked); }));
        c.save(dir.filePath("show.savshow")); QVERIFY2(!c.dirty, qPrintable(c.status));
        c.undo(); QCOMPARE(c.project.assets[original.id].path, original.path); QCOMPARE(c.project.assets[original.id].audio, original.audio);
    }
    void undoKeepsPackageMediaAliveAcrossSaves() {
        // Opened package, relink the extracted file to an outside copy, Save twice, Undo: the
        // extracted original the undo step still uses must outlive both saves.
        QTemporaryDir dir; const auto source = dir.filePath("cue.wav"); writeBytes(source, waveBytes(16, 1, 2, sampleRate, false, 48000 + 31));
        auto project = newProject(); project.show.clips.clear(); project.show.markers.clear(); project.assets.clear();
        project.show.tracks = {{"audio", "Audio", false, false, false, 1, 0, 0, 1}};
        auto asset = loadMedia(source); project.assets.insert(asset.id, asset);
        Clip clip; clip.id = "a"; clip.assetId = asset.id.toStdString(); clip.audio = asset.audio; clip.duration = asset.audio->length(); project.show.clips.push_back(clip);
        const auto package = dir.filePath("show.savpack"); writeProjectPackage(project, package);
        Controller c; c.replace(readProject(package)); const auto extracted = c.project.assets[asset.id].path;
        QVERIFY(c.project.packageDirectory); QVERIFY(extracted.startsWith(c.project.packageDirectory->path()));
        const auto outside = dir.filePath("outside.wav"); QVERIFY(QFile::copy(source, outside));
        auto relinked = loadMedia(outside); relinked.id = asset.id; relinked.name = asset.name;
        QVERIFY(c.edit([&](Project& p) { p.assets.insert(relinked.id, relinked); }));
        c.save(dir.filePath("one.savshow")); QVERIFY2(!c.dirty, qPrintable(c.status));
        QVERIFY(c.edit([](Project& p) { p.title = "Second save"; }));
        c.save(dir.filePath("two.savshow")); QVERIFY2(!c.dirty, qPrintable(c.status));
        c.undo(); c.undo();
        QCOMPARE(c.project.assets[asset.id].path, extracted); QVERIFY(QFileInfo::exists(extracted));
    }
    void identicalBytesInDifferentRawFormatsDoNotShareCaches() {
        // Raw formats are chosen by extension: these 44,100 16-bit samples last 1 s as .sw and 5.5 s as .sln (8 kHz).
        QTemporaryDir dir; QByteArray bytes(88200, '\0');
        for (qsizetype i = 0; i + 1 < bytes.size(); i += 2) qToLittleEndian<qint16>(static_cast<qint16>((i * 37) % 20000 - 10000), bytes.data() + i);
        const auto sln = dir.filePath("cue.sln"), sw = dir.filePath("cue.sw"); writeBytes(sln, bytes); writeBytes(sw, bytes);
        for (const auto preparation : {AudioPreparation::Stream, AudioPreparation::Prepared}) {
            const auto first = loadMedia(sln, preparation).audio, second = loadMedia(sw, preparation).audio;
            QCOMPARE(first->length(), Frame{264600}); QCOMPARE(second->length(), Frame{48000});
            float sample[2]; second->storage->read(second->length() - 1, sample, 1);
        }
    }
    void deletedStreamedSourceIsMissingMedia() {
        // Streamed audio needs its original: Play and preflight must say so before cues start.
        setAudioPreparation(AudioPreparation::Stream);
        QTemporaryDir dir; const auto path = dir.filePath("cue.wav"); writeBytes(path, waveBytes(16, 1, 2, 44100, false, 44100 + 211));
        auto asset = loadMedia(path); QVERIFY(asset.audio->storage->readsSource());
        Controller c; auto project = newProject(); project.show.clips.clear(); project.show.markers.clear(); project.assets.clear();
        project.show.tracks = {{"audio", "Audio", false, false, false, 1, 0, 0, 1}}; project.assets.insert(asset.id, asset);
        Clip clip; clip.id = "a"; clip.assetId = asset.id.toStdString(); clip.audio = asset.audio; clip.duration = asset.audio->length(); project.show.clips.push_back(clip);
        c.replace(project); QVERIFY(c.missingMedia().isEmpty());
        QVERIFY(QFile::remove(path));
        QCOMPARE(c.missingMedia(), QStringList{asset.path}); QVERIFY(!c.preflight().filter(asset.path).isEmpty());
        QVERIFY(Controller::needsRelink(c.project.assets[asset.id])); // Relink offers it, though it was online when opened.
        c.toggle(); QCOMPARE(c.transport.state(), State::Stopped);
        // A prepared cache does not need the original. Restoring the file under its name needs the
        // streamed reader released, which happens for unopenable sources (FAT/exFAT keep the name until then).
        closeIdleStreams(true);
        writeBytes(path, waveBytes(16, 1, 2, 44100, false, 44100 + 211)); const auto prepared = loadMedia(path, AudioPreparation::Prepared).audio;
        QVERIFY(!prepared->storage->readsSource()); QCOMPARE(c.replaceAudio({{asset.path, prepared}}), Controller::AudioSwap::Applied); QVERIFY(QFile::remove(path));
        QVERIFY(c.missingMedia().isEmpty()); QVERIFY(!Controller::needsRelink(c.project.assets[asset.id]));
    }
    void saveKeepsUndoStepsThatSavedMediaWouldInvalidate() {
        // Opened package; remove a clip that starts 0.8 s into its source; the extracted file then
        // shrinks to 0.3 s and is reloaded; Save; Undo must restore a valid, playable clip on its old
        // audio. A prepared cache keeps the old samples (a direct WAV would read the shrunk file and
        // report it changed, which is the truth about that file).
        setAudioPreparation(AudioPreparation::Prepared);
        QTemporaryDir dir; const auto source = dir.filePath("cue.wav"); writeBytes(source, waveBytes(16, 1, 2, 44100, false, 44100 + 37));
        auto project = newProject(); project.show.clips.clear(); project.show.markers.clear(); project.assets.clear();
        project.show.tracks = {{"audio", "Audio", false, false, false, 1, 0, 0, 1}};
        auto asset = loadMedia(source); project.assets.insert(asset.id, asset);
        Clip clip; clip.id = "a"; clip.assetId = asset.id.toStdString(); clip.audio = asset.audio; clip.offset = frames(.8); clip.duration = frames(.2); project.show.clips.push_back(clip);
        const auto package = dir.filePath("show.savpack"); writeProjectPackage(project, package);
        Controller c; c.replace(readProject(package)); const auto extracted = c.project.assets[asset.id].path; const auto before = c.project.show.clips[0].audio;
        QVERIFY(c.edit([](Project& p) { p.show.clips.clear(); }));
        writeBytes(extracted, waveBytes(16, 1, 2, 44100, false, 13230));
        QCOMPARE(c.replaceAudio({{extracted, loadMedia(extracted).audio}}), Controller::AudioSwap::Applied);
        c.save(dir.filePath("show.savshow")); QVERIFY2(!c.dirty, qPrintable(c.status));
        c.undo(); QCOMPARE(c.project.show.clips.size(), std::size_t{1}); QCOMPARE(c.project.show.clips[0].audio, before);
        validate(c.project); QVERIFY(!before->storage->readsSource()); QVERIFY(!c.transport.audioReadFailed());
    }
    void replacingAnArmedPackageRemovesItsExtraction() {
        // The armed transport streams from the extraction; replacing the show must release those files
        // before the extraction is removed (Windows cannot remove a folder whose files are open).
        setAudioPreparation(AudioPreparation::Stream);
        QTemporaryDir dir; const auto source = dir.filePath("cue.wav"); writeBytes(source, waveBytes(16, 1, 2, 44100, false, 44100 + 59));
        auto project = newProject(); project.show.clips.clear(); project.show.markers.clear(); project.assets.clear();
        project.show.tracks = {{"audio", "Audio", false, false, false, 1, 0, 0, 1}};
        auto asset = loadMedia(source); project.assets.insert(asset.id, asset);
        Clip clip; clip.id = "a"; clip.assetId = asset.id.toStdString(); clip.audio = asset.audio; clip.duration = asset.audio->length(); project.show.clips.push_back(clip);
        const auto package = dir.filePath("show.savpack"); writeProjectPackage(project, package);
        QString extraction;
        {
            Controller c; setAudioPreparation(AudioPreparation::Stream); // The controller applied the saved preference.
            c.replace(readProject(package));
            QVERIFY(c.project.assets.first().audio->storage->decodes()); extraction = c.project.packageDirectory->path();
            c.replace(newProject());
            QVERIFY2(!QFileInfo::exists(extraction), qPrintable(extraction));
        }
    }
    void fileIdentityComparesWhatBothSidesKnow() {
        // Unknown parts (a share not reporting them this time) match anything; known parts must agree.
        using detail::FileIdentity;
        QVERIFY(detail::sameIdentity(FileIdentity{}, FileIdentity{7, 5}));
        QVERIFY(detail::sameIdentity(FileIdentity{0, 5}, FileIdentity{7, 5}));
        QVERIFY(!detail::sameIdentity(FileIdentity{7, 5}, FileIdentity{8, 5})); // Another stick, same file number.
        QVERIFY(!detail::sameIdentity(FileIdentity{7, 5}, FileIdentity{7, 6}));
        QVERIFY(detail::identityUncertain(FileIdentity{}, FileIdentity{7, 5}));
        QVERIFY(detail::identityUncertain(FileIdentity{0, 5}, FileIdentity{7, 5}));
        QVERIFY(!detail::identityUncertain(FileIdentity{7, 5}, FileIdentity{7, 5}));
        QVERIFY(!detail::identityUncertain(FileIdentity{0, 5}, FileIdentity{0, 5}));
        QVERIFY(detail::sameIdentity(FileIdentity{7, 0}, FileIdentity{7, 5})); // A share that left out the file number.
        QVERIFY(detail::identityUncertain(FileIdentity{7, 0}, FileIdentity{7, 5}));
#ifdef Q_OS_MACOS
        // Local volumes report a stable identifier, which tells two sticks mounted at one path apart.
        QTemporaryDir dir; const auto path = dir.filePath("id.wav"); writeBytes(path, "x");
        QVERIFY(detail::fileIdentity(path).first != 0);
#endif
    }
    void extractionStillInUseIsRemovedLater() {
        // A decoder can still be closing an extracted file on the background sweep when its project
        // closes; Windows then cannot remove the folder, so removal is retried once the file is free.
        QTemporaryDir dir; const auto source = dir.filePath("cue.wav"); writeBytes(source, waveBytes(16, 1, 2, 44100, false, 4410));
        auto project = newProject(); project.show.clips.clear(); project.show.markers.clear(); project.assets.clear();
        auto asset = loadMedia(source); project.assets.insert(asset.id, asset);
        const auto package = dir.filePath("held.savpack"); writeProjectPackage(project, package);
        auto opened = readProject(package); const auto extraction = opened.packageDirectory->path();
        QFile held(opened.assets.first().path); QVERIFY(held.open(QIODevice::ReadOnly));
        opened = Project{};
        held.close(); retryPackageRemovals(true);
        QVERIFY2(!QFileInfo::exists(extraction), qPrintable(extraction));
    }
    void idleStreamDecodersCloseTheirFiles() {
#ifdef Q_OS_WIN
        QSKIP("counts descriptors through /dev/fd");
#else
        // A fully buffered clip stops reading; its decoder must not hold the file for the rest of the show.
        setAudioPreparation(AudioPreparation::Stream);
        QTemporaryDir dir; const auto path = dir.filePath("short.wav"); writeBytes(path, waveBytes(16, 1, 2, 44100, false, 4410 + 97));
        const auto audio = loadMedia(path).audio;
        const auto descriptors = [] { return QDir("/dev/fd").entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot).size(); };
        const auto before = descriptors(); float sample[2]; audio->storage->read(0, sample, 1);
        QCOMPARE(descriptors(), before + 1);
        closeIdleStreams(true); QCOMPARE(descriptors(), before + 1); // Just read: not idle yet.
        QTest::qWait(3100); closeIdleStreams(true); QCOMPARE(descriptors(), before);
        audio->storage->read(4000, sample, 1); QCOMPARE(descriptors(), before + 1); // Reopens on demand.
        // A deleted source's decoder closes at the next sweep, idle or not, so its name can be reused at once
        // where deleting an open file only marks it (FAT/exFAT on Windows).
        QVERIFY(QFile::remove(path)); closeIdleStreams(true); QCOMPARE(descriptors(), before);
#endif
    }
    void rejectedSourceSwitchKeepsThePreviousPreference() {
        // Selecting Prepared reloads a stem that has since shrunk below a clip's offset: the show keeps
        // its streamed audio, so the preference must stay Stream (and a later retry must reload).
        QSettings().setValue("audio/sources", "stream");
        QTemporaryDir dir; const auto path = dir.filePath("stem.wav"); writeBytes(path, waveBytes(16, 1, 2, 44100, false, 88200 + 3));
        MainWindow window; window.show(); auto& c = window.controller(); QCOMPARE(audioPreparation(), AudioPreparation::Stream);
        auto project = newProject(); project.show.clips.clear(); project.show.markers.clear(); project.assets.clear();
        project.show.tracks = {{"audio", "Audio", false, false, false, 1, 0, 0, 1}};
        auto asset = loadMedia(path); project.assets.insert(asset.id, asset);
        Clip clip; clip.id = "a"; clip.assetId = asset.id.toStdString(); clip.audio = asset.audio; clip.offset = frames(1.5); clip.duration = frames(.4); project.show.clips.push_back(clip);
        c.replace(project);
        writeBytes(path, waveBytes(16, 1, 2, 44100, false, 44100 + 5));
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = window.findChild<QDialog*>("outputSettingsDialog"); if (!dialog) return;
            auto* sources = dialog->findChild<QComboBox*>("audioSourcePreparation"); if (!sources) return;
            sources->setCurrentIndex(sources->findData(static_cast<int>(AudioPreparation::Prepared))); emit sources->activated(sources->currentIndex());
        });
        window.findChild<QPushButton*>("outputSettingsButton")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!c.busy, 15000);
        QCOMPARE(audioPreparation(), AudioPreparation::Stream); QCOMPARE(QSettings().value("audio/sources").toString(), QString("stream"));
        QVERIFY2(c.status.startsWith("Audio sources were not changed"), qPrintable(c.status));
        QVERIFY(c.project.show.clips[0].audio->storage->decodes());
    }
    void manifestsThatReferToOtherFilesAreRejected() {
        // Two identical manifests can name different files, so their bytes cannot identify the audio.
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg"); if (ffmpeg.isEmpty()) QSKIP("ffmpeg fixture generation unavailable");
        QTemporaryDir dir; QProcess process;
        process.start(ffmpeg, {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=1", dir.filePath("cue.flac")});
        QVERIFY(process.waitForFinished(30000)); QCOMPARE(process.exitCode(), 0);
        const auto manifest = dir.filePath("show.ffconcat"); writeBytes(manifest, "ffconcat version 1.0\nfile cue.flac\n");
        for (const auto preparation : {AudioPreparation::Prepared, AudioPreparation::Stream})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, loadMedia(manifest, preparation));
        // Refused from its bytes, before FFmpeg would open the playlist and fetch its (missing) segment.
        const auto playlist = dir.filePath("show.m3u8");
        writeBytes(playlist, "#EXTM3U\n#EXT-X-TARGETDURATION:4\n#EXTINF:4.0,\nmissing-segment.ts\n#EXT-X-ENDLIST\n");
        try { loadMedia(playlist); QFAIL("playlist imported"); }
        catch (const std::runtime_error& e) { QVERIFY2(QString::fromUtf8(e.what()).startsWith("Playlists"), e.what()); }
    }
    void explicitPreparationOverridesThePreference() {
        // A batch prepares as it was asked to, even if the operator changes the preference meanwhile.
        QTemporaryDir dir; const auto path = dir.filePath("batch.wav"); writeBytes(path, waveBytes(16, 1, 2, 44100, false, 44100 + 13));
        setAudioPreparation(AudioPreparation::Stream); QVERIFY(!loadMedia(path, AudioPreparation::Prepared).audio->storage->decodes());
        setAudioPreparation(AudioPreparation::Prepared); QVERIFY(loadMedia(path, AudioPreparation::Stream).audio->storage->decodes());
    }
    void damagedStreamSidecarIsRejected() {
        setAudioPreparation(AudioPreparation::Stream);
        QTemporaryDir dir; const auto path = dir.filePath("source.wav"); writeBytes(path, waveBytes(16, 1, 2, 44100, false, 44100 + 77)); // Unique content, unique sidecar.
        QDir caches(detail::audioCacheDirectory()); const auto before = caches.entryList({"*.stream"}, QDir::Files);
        const auto first = loadMedia(path).audio;
        auto created = caches.entryList({"*.stream"}, QDir::Files); for (const auto& name : before) created.removeAll(name);
        QCOMPARE(created.size(), 1);
        // Appended whole seek-point records no longer match the stored count; the file is rebuilt, not trusted.
        QFile sidecar(caches.filePath(created.front())); QVERIFY(sidecar.open(QIODevice::Append)); QCOMPARE(sidecar.write(QByteArray(16 * 4096, '\x7f')), 16 * 4096); sidecar.close();
        const auto reopened = loadMedia(path).audio;
        QCOMPARE(reopened->peaks, first->peaks); QVERIFY(QFileInfo(sidecar.fileName()).size() < 16 * 4096);
    }
    void streamedAudioSurvivesTimestampJumps() {
        // A broadcast recording whose timestamps jump forward: positions follow counted samples, not timestamps.
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg"); if (ffmpeg.isEmpty()) QSKIP("ffmpeg fixture generation unavailable");
        QTemporaryDir dir; QByteArray joined;
        for (const auto& [name, start, offset] : {std::tuple{"a.ts", "0", "0"}, std::tuple{"b.ts", "4", "6"}}) {
            QProcess process;
            process.start(ffmpeg, {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
                "aevalsrc=0.4*sin(2*PI*(100+800*t)*t)+0.1*random(0)|0.3*sin(2*PI*660*t):s=48000:d=18", "-ss", start, "-t", QString(name) == "a.ts" ? "4" : "14",
                "-c:a", "aac", "-aac_pns", "0", "-output_ts_offset", offset, "-f", "mpegts", dir.filePath(name)});
            QVERIFY(process.waitForFinished(30000)); QCOMPARE(process.exitCode(), 0);
            QFile part(dir.filePath(name)); QVERIFY(part.open(QIODevice::ReadOnly)); joined += part.readAll();
        }
        const auto file = dir.filePath("jump.ts"); writeBytes(file, joined);
        const auto prepared = loadMedia(file).audio;
        setAudioPreparation(AudioPreparation::Stream); const auto streamed = loadMedia(file).audio;
        QCOMPARE(streamed->length(), prepared->length());
        for (const Frame start : {frames(10), frames(12.5), frames(2), frames(16), frames(5) + 77}) {
            std::vector<float> a(1024), b(1024);
            prepared->storage->read(start, a.data(), 512); streamed->storage->read(start, b.data(), 512);
            QCOMPARE(b, a);
        }
    }
    void audioFormatChangePartwayIsRejected() {
        // Stereo ADTS followed by 5.1 ADTS: the decoder changes layout mid-stream.
        const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg"); if (ffmpeg.isEmpty()) QSKIP("ffmpeg fixture generation unavailable");
        QTemporaryDir dir; QByteArray joined;
        for (const auto& [name, layout] : {std::pair{"a.aac", "stereo"}, std::pair{"b.aac", "5.1"}}) {
            QProcess process;
            process.start(ffmpeg, {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", QString("sine=frequency=440:sample_rate=48000:duration=1"),
                "-af", QString("aformat=channel_layouts=") + layout, "-c:a", "aac", dir.filePath(name)});
            QVERIFY(process.waitForFinished(30000)); QCOMPARE(process.exitCode(), 0);
            QFile part(dir.filePath(name)); QVERIFY(part.open(QIODevice::ReadOnly)); joined += part.readAll();
        }
        const auto file = dir.filePath("changing.aac"); writeBytes(file, joined);
        for (const auto preparation : {AudioPreparation::Prepared, AudioPreparation::Stream}) {
            setAudioPreparation(preparation);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, loadMedia(file));
        }
    }
    void replacedStreamedSourceIsDetected() {
        // An editor saving through a new file keeps size and modification time but not the file itself.
        setAudioPreparation(AudioPreparation::Stream);
        QTemporaryDir dir; const auto path = dir.filePath("source.wav");
        auto bytes = waveBytes(16, 1, 2, 44100, false, 44100); writeBytes(path, bytes);
        const auto time = QDateTime::currentDateTimeUtc().addDays(-1); setModified(path, time);
        const auto stored = QFileInfo(path).lastModified(); // FAT and exFAT round to 2 s.
        const auto asset = loadMedia(path); float sample[2]; asset.audio->storage->read(100, sample, 1);
        bytes[bytes.size() - 2] ^= 0x40; const auto replacement = dir.filePath("replacement.wav"); writeBytes(replacement, bytes);
        setModified(replacement, time); QVERIFY(QFile::remove(path));
        // Where deleting an open file only marks it (FAT/exFAT), its name frees once the open reader is
        // released, which a read of the missing source does; that read is retryable, not "changed".
        try { asset.audio->storage->read(100, sample, 1); QFAIL("missing source read"); }
        catch (const AudioSourceChanged&) { QFAIL("a missing source can come back"); } catch (const std::runtime_error&) {}
        QVERIFY(QFile::rename(replacement, path));
        QCOMPARE(QFileInfo(path).size(), static_cast<qint64>(bytes.size())); QCOMPARE(QFileInfo(path).lastModified(), stored);
        QVERIFY_THROWS_EXCEPTION(AudioSourceChanged, asset.audio->storage->read(100, sample, 1));
        // Reopening, as the error asks, must measure the replacement rather than reuse the old summary.
        const auto copy = dir.filePath("copy.wav"); QVERIFY(QFile::copy(path, copy));
        const auto reopened = loadMedia(path).audio, fresh = loadMedia(copy).audio;
        QCOMPARE(reopened->peaks, fresh->peaks); QVERIFY(reopened->peaks != asset.audio->peaks);
        QCOMPARE(sampleAt(*reopened, reopened->length() - 1), sampleAt(*fresh, fresh->length() - 1));
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
        for (const auto preparation : {AudioPreparation::Prepared, AudioPreparation::Stream}) {
        setAudioPreparation(preparation);
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
    }
    void timelineTrimStopsAtNonLoopingSourceEnd() {
        Controller c; auto project = newProject(); project.show.clips.clear(); project.show.markers.clear(); project.assets.clear();
        project.show.tracks = {{"audio", "Audio", false, false, false, 1, 0, 0, 1}}; project.show.duration = frames(60);
        auto audio = std::make_shared<AudioData>(); audio->samples.assign(static_cast<std::size_t>(frames(4)) * 2, 0.f);
        MediaAsset asset; asset.id = "tone"; asset.name = "Tone"; asset.audio = audio; project.assets.insert(asset.id, asset);
        Clip clip; clip.id = "a"; clip.assetId = "tone"; clip.audio = audio; clip.offset = frames(1); clip.duration = frames(2); project.show.clips.push_back(clip);
        c.replace(project); Timeline timeline(c); timeline.resize(900, 300); timeline.show(); QVERIFY(QTest::qWaitForWindowExposed(&timeline));
        timeline.setZoom(50); auto* view = timeline.viewport();
        auto send = [&](QEvent::Type type, int x, Qt::MouseButtons buttons) {
            QMouseEvent e(type, QPointF(x, 90), view->mapToGlobal(QPointF(x, 90)), Qt::LeftButton, buttons, Qt::NoModifier); QApplication::sendEvent(view, &e);
        };
        send(QEvent::MouseButtonPress, 288, Qt::LeftButton); send(QEvent::MouseMove, 500, Qt::LeftButton); send(QEvent::MouseMove, 800, Qt::LeftButton);
        send(QEvent::MouseButtonRelease, 800, Qt::NoButton);
        QCOMPARE(c.project.show.clips[0].duration, frames(3)); QCOMPARE(c.project.show.clips[0].offset, frames(1));
        c.undo(); QVERIFY(c.edit([](Project& p) { p.show.clips[0].loop = true; }));
        send(QEvent::MouseButtonPress, 288, Qt::LeftButton); send(QEvent::MouseMove, 800, Qt::LeftButton); send(QEvent::MouseButtonRelease, 800, Qt::NoButton);
        QVERIFY(c.project.show.clips[0].duration > frames(10)); c.dirty = false;
    }
    void automationShapesRoundTripTrimAndSplit() {
        QTemporaryDir dir; Project p = demoProject(); materializeProjectMedia(p, dir.filePath("show.media"));
        auto& clip = p.show.clips[0];
        clip.alpha.points = {{0, 0, CurveShape::SCurve, .5}, {frames(2), 1, CurveShape::Hold}, {frames(10), .5, CurveShape::Curve, -.25}, {frames(20), 0}};
        const auto path = dir.filePath("shapes.savshow"); writeProject(p, path);
        QFile input(path); QVERIFY(input.open(QIODevice::ReadOnly)); const auto root = QJsonDocument::fromJson(input.readAll()).object(); input.close();
        const auto saved = root["clips"].toArray()[0].toObject()["alpha"].toArray();
        QCOMPARE(saved[0].toArray(), (QJsonArray{0., 0., "scurve", .5})); QCOMPARE(saved[3].toArray(), (QJsonArray{static_cast<double>(frames(20)), 0.}));
        const auto loaded = readProject(path).show.clips[0].alpha.points;
        QCOMPARE(loaded.size(), std::size_t{4}); QVERIFY(loaded[1].shape == CurveShape::Hold); QVERIFY(loaded[2].shape == CurveShape::Curve); QCOMPARE(loaded[2].bend, -.25);
        auto invalid = p; invalid.show.clips[0].alpha.points[0].bend = 1.5; QVERIFY_THROWS_EXCEPTION(std::runtime_error, validate(invalid));
        auto edited = [&](const QJsonValue& point) {
            auto o = root; auto clips = o["clips"].toArray(); auto first = clips[0].toObject(); first["alpha"] = QJsonArray{point};
            clips[0] = first; o["clips"] = clips; return openFailure(dir.filePath("bad.savshow"), QJsonDocument(o).toJson());
        };
        QCOMPARE(edited(QJsonArray{0., 0., "wobble", 0.}), QString("Invalid automation shape"));
        QCOMPARE(edited(QJsonArray{0., 0., "hold"}), QString("Invalid automation data"));
        // A left trim inside the hold keeps holding from the new start.
        Clip trimmed = clip; retime(trimmed, trimmed.start + frames(4), trimmed.duration - frames(4), trimmed.offset + frames(4));
        QCOMPARE(trimmed.alpha.points.front().time, Frame{0}); QCOMPARE(trimmed.alpha.points.front().value, 1.);
        QVERIFY(trimmed.alpha.points.front().shape == CurveShape::Hold); QCOMPARE(trimmed.alpha.at(frames(5), 0), 1.);
    }
    void timelineEditsKeyframesWithTheMouse() {
        Controller c; auto project = newProject(); project.show.clips.clear(); project.show.markers.clear(); project.assets.clear();
        project.show.tracks = {{"audio", "Audio", false, false, false, 1, 0, 0, 1}}; project.show.duration = frames(60);
        auto audio = std::make_shared<AudioData>(); audio->samples.assign(static_cast<std::size_t>(frames(20)) * 2, 0.f);
        MediaAsset asset; asset.id = "tone"; asset.name = "Tone"; asset.audio = audio; project.assets.insert(asset.id, asset);
        Clip clip; clip.id = "a"; clip.assetId = "tone"; clip.audio = audio; clip.duration = frames(10); clip.volume.points = {{0, 0}, {frames(2), 1}};
        project.show.clips.push_back(clip);
        c.replace(project); Timeline timeline(c); timeline.resize(900, 300); timeline.show(); QVERIFY(QTest::qWaitForWindowExposed(&timeline));
        timeline.setZoom(50); timeline.selectClip("a"); auto* view = timeline.viewport();
        auto send = [&](QEvent::Type type, QPointF at, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
            QMouseEvent e(type, at, view->mapToGlobal(at), Qt::LeftButton, buttons, modifiers); QApplication::sendEvent(view, &e);
        };
        auto drag = [&](QPointF from, QPointF to, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
            send(QEvent::MouseButtonPress, from, Qt::LeftButton, modifiers); send(QEvent::MouseMove, (from + to) / 2, Qt::LeftButton, modifiers);
            send(QEvent::MouseMove, to, Qt::LeftButton, modifiers); send(QEvent::MouseButtonRelease, to, Qt::NoButton, modifiers);
        };
        // The clip spans y 63–109; its curve band runs from 81 (value 1) to 105 (value 0), at 50 px per second.
        const auto& points = [&]() -> const std::vector<Keyframe>& { return c.project.show.clips[0].volume.points; };
        drag({290, 81}, {340, 93});
        QCOMPARE(points().size(), std::size_t{2}); QCOMPARE(points()[1].time, frames(3)); QCOMPARE(points()[1].value, .5);
        QCOMPARE(c.project.show.clips[0].start, Frame{0}); // The clip itself stayed put.
        // Clicking the line adds a point on it; the double-click that follows keeps it, a later one deletes it.
        drag({215, 103}, {215, 103});
        QCOMPARE(points().size(), std::size_t{3}); QCOMPARE(points()[1].time, frames(.5)); QVERIFY(std::abs(points()[1].value - .5 / 6) < 1e-9);
        send(QEvent::MouseButtonDblClick, {215, 103}, Qt::LeftButton); send(QEvent::MouseButtonRelease, {215, 103}, Qt::NoButton);
        QCOMPARE(points().size(), std::size_t{3});
        QTest::qWait(QApplication::doubleClickInterval() + 50);
        send(QEvent::MouseButtonDblClick, {215, 103}, Qt::LeftButton); send(QEvent::MouseButtonRelease, {215, 103}, Qt::NoButton);
        QCOMPARE(points().size(), std::size_t{2});
        // Alt-dragging a straight segment upwards bends it into a rising-early curve.
        drag({240, 101}, {240, 71}, Qt::AltModifier);
        QVERIFY(points()[0].shape == CurveShape::Curve); QCOMPARE(points()[0].bend, .5);
        QVERIFY(points()[0].shape == CurveShape::Curve && c.project.show.clips[0].volume.at(frames(1), 0) > .5 / 3);
        c.undo(); QVERIFY(points()[0].shape == CurveShape::Linear);
        // Show mode locks keyframes like every other edit.
        c.showMode = true; drag({340, 93}, {390, 81}); QCOMPARE(points()[1].time, frames(3)); c.showMode = false;
        c.dirty = false;
    }
    void timelineZoomAnchorsPlayheadAndCursor() {
        Controller c; c.replace(longShow()); Timeline timeline(c); timeline.resize(900, 300); timeline.show(); QVERIFY(QTest::qWaitForWindowExposed(&timeline));
        auto x = [&](double t) { return 190 + t * timeline.zoom() - timeline.horizontalScrollBar()->value(); };
        c.seek(frames(20)); const double before = x(20); timeline.setZoom(40);
        QCOMPARE(timeline.zoom(), 40.); QVERIFY(std::abs(x(20) - before) <= 1); QVERIFY(timeline.horizontalScrollBar()->value() > 0);
        QSignalSpy zoomed(&timeline, &Timeline::zoomChanged);
        const double anchor = (400 - 190 + timeline.horizontalScrollBar()->value()) / timeline.zoom();
        QWheelEvent wheel(QPointF(400, 120), timeline.viewport()->mapToGlobal(QPointF(400, 120)), {}, {0, 120}, Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(timeline.viewport(), &wheel);
        QCOMPARE(zoomed.count(), 1); QVERIFY(timeline.zoom() > 40); QVERIFY(std::abs(x(anchor) - 400) <= 1);
        timeline.zoomToFit(); QCOMPARE(zoomed.count(), 2); QCOMPARE(timeline.horizontalScrollBar()->maximum(), 0);
        QVERIFY(600 * timeline.zoom() <= timeline.viewport()->width() - 190);
    }
    void timelineFollowsPlayheadUnlessUserScrolled() {
        Controller c; c.replace(longShow()); Timeline timeline(c); timeline.resize(900, 300); timeline.show(); QVERIFY(QTest::qWaitForWindowExposed(&timeline));
        const double width = timeline.viewport()->width() - 190; QVERIFY(c.transport.command({Action::Play}));
        c.transport.render(nullptr, static_cast<unsigned>(frames(10)), 0); emit c.tick(); QCOMPARE(timeline.horizontalScrollBar()->value(), 0);
        c.transport.render(nullptr, static_cast<unsigned>(frames(20)), 0); emit c.tick();
        const double x = 190 + 30 * timeline.zoom() - timeline.horizontalScrollBar()->value(); QVERIFY(std::abs(x - (190 + width * .15)) <= 2);
        timeline.horizontalScrollBar()->setValue(0); c.transport.render(nullptr, static_cast<unsigned>(frames(10)), 0); emit c.tick();
        QCOMPARE(timeline.horizontalScrollBar()->value(), 0); c.stopAll();
    }
    void timelinePlayheadTicksRepaintOnlyStrips() {
        Controller c; c.replace(longShow()); Timeline timeline(c); timeline.resize(900, 300); timeline.show(); QVERIFY(QTest::qWaitForWindowExposed(&timeline));
        QTest::qWait(50); PaintSpy spy; timeline.viewport()->installEventFilter(&spy);
        QVERIFY(c.transport.command({Action::Play})); c.transport.render(nullptr, static_cast<unsigned>(frames(1)), 0); emit c.tick();
        QTRY_VERIFY(spy.count > 0); QVERIFY(spy.region.boundingRect().width() < 60); QCOMPARE(spy.region.boundingRect().height(), timeline.viewport()->height());
        spy.count = 0; emit c.tick(); QTest::qWait(50); QCOMPARE(spy.count, 0);
        timeline.viewport()->removeEventFilter(&spy); c.stopAll();
    }
};
QTEST_MAIN(AppTests)
#include "app_tests.moc"
