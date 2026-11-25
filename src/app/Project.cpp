#include "app/Project.h"
#include "media/AudioSources.h"
#include "media/MidiOutput.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QUuid>
#include <QPainter>
#include <QDataStream>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <set>
#include <limits>
#include <stdexcept>
#ifdef Q_OS_WIN
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#ifdef Q_OS_LINUX
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#endif

namespace sav {
namespace {
bool nativeSameFsRename(const QString& src, const QString& dst, QString& error) {
#ifdef Q_OS_WIN
    const std::wstring s = src.toStdWString();
    const std::wstring d = dst.toStdWString();
    // Never pass MOVEFILE_COPY_ALLOWED so GUI-thread publish cannot fall back to a synchronous cross-volume copy.
    if (!MoveFileExW(s.c_str(), d.c_str(), 0)) {
        error = QString("Win32 error %1").arg(GetLastError());
        return false;
    }
    return true;
#else
    if (::rename(QFile::encodeName(src).constData(), QFile::encodeName(dst).constData()) != 0) {
        error = QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    return true;
#endif
}
}
QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString timecode(Frame position, double fps) {
    const auto whole = position / sampleRate;
    const int frame = static_cast<int>(std::floor((position % sampleRate) * fps / sampleRate));
    return QString("%1:%2:%3:%4").arg(whole / 3600, 2, 10, QChar('0')).arg(whole / 60 % 60, 2, 10, QChar('0'))
        .arg(whole % 60, 2, 10, QChar('0')).arg(frame, 2, 10, QChar('0'));
}
QString countdown(Frame duration) {
    const auto total = static_cast<qint64>(std::ceil(seconds(std::max<Frame>(0, duration))));
    return total >= 3600 ? QString("%1:%2:%3").arg(total / 3600).arg(total / 60 % 60, 2, 10, QChar('0')).arg(total % 60, 2, 10, QChar('0'))
                         : QString("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QChar('0'));
}
void retime(Clip& clip, Frame start, Frame duration, Frame offset) {
    // Left trims shift clip-relative automation with the source content.
    const Frame shift = start - clip.start;
    if (offset - clip.offset == shift && shift != 0) for (int property = 0; property < 6; ++property) {
        auto* e = envelope(clip, property);
        if (e->points.empty()) continue;
        Keyframe atTrim = cutPoint(*e, shift, propertyValue(clip, property)); atTrim.time = 0;
        for (auto& point : e->points) point.time -= shift;
        std::erase_if(e->points, [](const Keyframe& point) { return point.time < 0; });
        if (e->points.empty() || e->points.front().time > 0) e->points.insert(e->points.begin(), atTrim);
    }
    clip.start = start; clip.duration = duration; clip.offset = offset;
}
void retimeLinked(Project& p, const std::string& id, Frame start, Frame duration, Frame offset) {
    const auto it = std::find_if(p.show.clips.begin(), p.show.clips.end(), [&](const Clip& c) { return c.id == id; });
    if (it == p.show.clips.end()) return;
    const Frame dStart = start - it->start, dDuration = duration - it->duration, dOffset = offset - it->offset; const auto link = it->link;
    retime(*it, start, duration, offset);
    if (!link.empty()) for (auto& c : p.show.clips) if (c.link == link && c.id != id) retime(c, c.start + dStart, c.duration + dDuration, c.offset + dOffset);
}
std::vector<std::string> linkedClips(const Project& p, const std::string& id) {
    std::vector<std::string> ids{id}; std::string link;
    for (const auto& c : p.show.clips) if (c.id == id) link = c.link;
    if (!link.empty()) for (const auto& c : p.show.clips) if (c.link == link && c.id != id) ids.push_back(c.id);
    return ids;
}
std::string splitLinked(Project& p, const std::string& id, Frame time, const std::string& tailId, bool rejectShapedCut) {
    const auto primary = std::find_if(p.show.clips.begin(), p.show.clips.end(), [&](const Clip& c) { return c.id == id; });
    if (primary == p.show.clips.end()) throw std::runtime_error("Unknown clip ID");
    if (time <= primary->start || time >= primary->end()) throw std::runtime_error("Split time must fall strictly inside the clip");
    const auto ids = linkedClips(p, id);
    for (const auto& c : p.show.clips) {
        if (std::find(ids.begin(), ids.end(), c.id) == ids.end()) continue;
        if (c.loop) throw std::runtime_error("Looping clips cannot be split; their loop starts at the clip's source in");
        if (rejectShapedCut && time > c.start && time < c.end()) {
            const Frame cut = time - c.start;
            for (int property = 0; property < 6; ++property) {
                const auto* e = envelope(c, property);
                const auto right = std::upper_bound(e->points.begin(), e->points.end(), cut,
                    [](Frame t, const Keyframe& k) { return t < k.time; });
                if (right != e->points.begin() && right != e->points.end()) {
                    const auto& prev = *(right - 1);
                    if (prev.time < cut && (prev.shape == CurveShape::SCurve || prev.shape == CurveShape::Curve) && prev.bend != 0)
                        throw std::runtime_error("Cannot split inside a shaped Curve or S-curve automation segment without altering its curve; adjust or flatten that segment first");
                }
            }
        }
    }
    const auto link = primary->link.empty() ? std::string() : newId().toStdString();
    std::string second;
    std::vector<Clip> tails;
    for (auto& c : p.show.clips) {
        if (std::find(ids.begin(), ids.end(), c.id) == ids.end()) continue;
        if (c.start >= time) {
            // A linked partner wholly after the cut belongs to the tail half.
            c.link = link;
            continue;
        }
        if (c.end() <= time) continue; // Wholly before the cut: stays with the head half.
        const Frame cut = time - c.start;
        Clip tail = c;
        tail.id = (c.id == ids.front() && !tailId.empty()) ? tailId : newId().toStdString();
        tail.link = link; tail.start = time; tail.duration = c.duration - cut; tail.offset = c.offset + cut;
        if (c.id == ids.front()) second = tail.id;
        // Clip-relative automation continues across the cut with the value at the cut.
        for (int property = 0; property < 6; ++property) {
            auto* head = envelope(c, property); auto* rest = envelope(tail, property);
            if (head->points.empty()) continue;
            Keyframe at = cutPoint(*head, cut, propertyValue(c, property)); at.time = 0;
            rest->points.clear(); rest->points.push_back(at);
            for (auto point : head->points) if (point.time > cut) { point.time -= cut; rest->points.push_back(point); }
            std::erase_if(head->points, [=](const Keyframe& point) { return point.time >= cut; }); head->points.push_back({cut, at.value});
        }
        c.duration = cut; tails.push_back(std::move(tail));
    }
    p.show.clips.insert(p.show.clips.end(), tails.begin(), tails.end());
    return second;
}
Envelope* envelope(Clip& clip, int property) {
    switch (property) { case 0: return &clip.volume; case 1: return &clip.balance; case 2: return &clip.positionX;
        case 3: return &clip.positionY; case 4: return &clip.size; default: return &clip.alpha; }
}
const Envelope* envelope(const Clip& clip, int property) { return envelope(const_cast<Clip&>(clip), property); }
double propertyValue(const Clip& clip, int property) {
    switch (property) { case 0: return clip.gain; case 1: return clip.pan; case 2: return clip.x;
        case 3: return clip.y; case 4: return clip.scale; default: return clip.opacity; }
}
std::pair<double, double> propertyRange(int property) {
    switch (property) { case 0: return {0, 4}; case 1: return {-1, 1}; case 2: case 3: return {-2, 2};
        case 4: return {.01, 8}; default: return {0, 1}; }
}
Keyframe cutPoint(const Envelope& e, Frame time, double fallback) {
    Keyframe point{time, e.at(time, fallback)};
    const auto right = std::upper_bound(e.points.begin(), e.points.end(), time, [](Frame t, const Keyframe& k) { return t < k.time; });
    if (right != e.points.begin() && right != e.points.end()) { point.shape = (right - 1)->shape; point.bend = (right - 1)->bend; }
    return point;
}
QString shapeName(CurveShape shape) {
    switch (shape) { case CurveShape::Hold: return "hold"; case CurveShape::Curve: return "curve"; case CurveShape::SCurve: return "scurve"; default: return "linear"; }
}
Project newProject() {
    Project p;
    p.show.tracks = {
        {newId().toStdString(), "Background", true}, {newId().toStdString(), "Overlay", true},
        {newId().toStdString(), "Main audio", false, false, false, 1, 0, 0, 1},
        {newId().toStdString(), "Ambience", false, true, false, 1, 0, 0, 1},
        {newId().toStdString(), "Guide", false, false, false, 1, 0, -1, -1},
        {newId().toStdString(), "Click", false, false, false, 1, 0, -1, -1}
    };
    return p;
}
Project demoProject() {
    Project p = newProject(); p.title = "The opening scene"; p.show.duration = frames(70);
    MediaAsset image; image.id = "demo-backdrop"; image.name = "Stage backdrop (generated)"; image.image = true;
    image.duration = 70; image.width = 1920; image.height = 1080;
    image.still = QImage(1920, 1080, QImage::Format_RGB32);
    QPainter painter(&image.still); QLinearGradient gradient(0, 0, 1920, 1080);
    gradient.setColorAt(0, QColor("#121c30")); gradient.setColorAt(.55, QColor("#26364d")); gradient.setColorAt(1, QColor("#765948"));
    painter.fillRect(image.still.rect(), gradient);
    painter.setPen(QColor(255, 255, 255, 24));
    for (int i = 0; i < 10; ++i) painter.drawEllipse(QPointF(1300, 450), 120 + i * 95, 120 + i * 95);
    painter.end(); p.assets.insert(image.id, image);
    Clip backdrop; backdrop.id = newId().toStdString(); backdrop.assetId = image.id.toStdString(); backdrop.track = 0; backdrop.duration = frames(70);
    p.show.clips.push_back(backdrop);
    Clip title; title.id = newId().toStdString(); title.track = 1; title.text = "THE OPENING SCENE\nA moment before the story begins";
    title.start = frames(2); title.duration = frames(36); title.alpha.points = {{0, 0}, {frames(2), 1}, {frames(25), 1}, {frames(28), 0}};
    p.show.clips.push_back(title);
    auto tone = [&](const QString& id, const QString& name, double duration, bool click) {
        MediaAsset a; a.id = id; a.name = name; a.duration = duration;
        auto pcm = std::make_shared<AudioData>(); pcm->mono = true; pcm->samples.resize(frames(duration) * 2);
        for (Frame i = 0; i < pcm->length(); ++i) {
            const double t = seconds(i);
            const double value = click ? (std::fmod(t, .5) < .025 ? std::sin(t * 2 * 3.141592653589793 * 880) * .06 : 0)
                : (std::sin(t * 2 * 3.141592653589793 * 110) + .4 * std::sin(t * 2 * 3.141592653589793 * 165)) * .04;
            pcm->samples[i * 2] = pcm->samples[i * 2 + 1] = static_cast<float>(value);
        }
        a.audio = pcm; p.assets.insert(a.id, a); return a.id.toStdString();
    };
    Clip ambience; ambience.id = newId().toStdString(); ambience.track = 3; ambience.assetId = tone("demo-ambience", "Ambient bed (generated)", 8, false);
    ambience.duration = frames(38); ambience.loop = true;
    ambience.volume.points = {{0, 0, CurveShape::SCurve, .5}, {frames(2), 1}, {frames(12), 1, CurveShape::Curve, .5}, {frames(16), 0}};
    p.show.clips.push_back(ambience);
    Clip click; click.id = newId().toStdString(); click.track = 5; click.assetId = tone("demo-click", "Click · 120 BPM (generated)", 20, true);
    click.start = frames(18); click.duration = frames(20); p.show.clips.push_back(click);
    p.show.markers = {{newId().toStdString(), "Wait for the actor", frames(12), true, MidiTiming::Arrival, {}},
        {newId().toStdString(), "Scene change", frames(38), true, MidiTiming::Arrival, {}}, {newId().toStdString(), "Curtain", frames(60), true, MidiTiming::Arrival, {}}};
    bindAudio(p); return p;
}
void bindAudio(Project& p) {
    for (auto& clip : p.show.clips) clip.audio = p.assets.value(QString::fromStdString(clip.assetId)).audio;
}
void validate(const Project& p) {
    auto fail = [](const char* message) { throw std::runtime_error(message); };
    if (p.width < 320 || p.width > 7680 || p.height < 240 || p.height > 4320 ||
        !std::isfinite(p.fps) || p.fps < 1 || p.fps > 120) fail("Invalid project picture settings");
    if (p.show.duration <= 0 || p.show.duration > maxFrame) fail("Project duration is outside representable frame timing");
    if (p.show.tracks.empty() || p.show.tracks.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        p.show.clips.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        p.show.markers.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) fail("Project indices cannot be represented");
    std::set<std::string> ids;
    for (const auto& t : p.show.tracks) {
        if (t.id.empty() || !ids.insert(t.id).second) fail("Duplicate or missing track ID");
        if (t.left < -1 || t.right < -1 || t.left == std::numeric_limits<int>::max() || t.right == std::numeric_limits<int>::max() || !std::isfinite(t.gain) || t.gain < 0 || t.gain > 4 ||
            !std::isfinite(t.pan) || std::abs(t.pan) > 1) fail("Invalid audio track routing or gain");
    }
    ids.clear();
    auto curve = [&](const Envelope& e, double min, double max) {
        Frame previous = -1;
        for (const auto& point : e.points) {
            if (point.time < 0 || point.time > maxFrame || point.time <= previous || !std::isfinite(point.value) || point.value < min || point.value > max ||
                !std::isfinite(point.bend) || std::abs(point.bend) > 1) fail("Invalid automation keyframe");
            previous = point.time;
        }
    };
    for (const auto& c : p.show.clips) {
        if (c.id.empty() || !ids.insert(c.id).second) fail("Duplicate or missing clip ID");
        if (c.track < 0 || static_cast<std::size_t>(c.track) >= p.show.tracks.size() || c.start < 0 || c.start > p.show.duration || c.duration <= 0 ||
            c.duration > p.show.duration - c.start || c.offset < 0 || c.offset > maxFrame) fail("Clip is outside the project or its track");
        if (!c.assetId.empty() && !p.assets.contains(QString::fromStdString(c.assetId))) fail("Clip references a missing asset");
        const bool visualTrack = p.show.tracks[c.track].visual;
        const auto asset = p.assets.constFind(QString::fromStdString(c.assetId));
        if (asset != p.assets.cend() && asset->offline) {} // Source checks resume once the media is relinked.
        else if (visualTrack) {
            if (c.text.empty() && (asset == p.assets.cend() || (!asset->image && !asset->video))) fail("Visual clip has no picture source");
            if (asset != p.assets.cend() && asset->video && seconds(c.offset) >= asset->duration) fail("Video offset is beyond the source");
        } else if (!c.audio || c.offset >= c.audio->length()) fail("Audio clip has no source or an invalid offset");
        if (!std::isfinite(c.gain) || c.gain < 0 || c.gain > 4 || !std::isfinite(c.pan) || std::abs(c.pan) > 1 ||
            !std::isfinite(c.x) || std::abs(c.x) > 2 || !std::isfinite(c.y) || std::abs(c.y) > 2 ||
            !std::isfinite(c.scale) || c.scale < .01 || c.scale > 8 || !std::isfinite(c.opacity) || c.opacity < 0 || c.opacity > 1)
            fail("Invalid clip transformation");
        for (int property = 0; property < 6; ++property) { const auto [min, max] = propertyRange(property); curve(*envelope(c, property), min, max); }
    }
    // An async picture's linked sound must keep playing with it, or the two drift apart at the first hold.
    std::set<std::string> asyncPictures;
    for (const auto& clip : p.show.clips) if (!clip.link.empty() && p.show.tracks[clip.track].visual && p.show.tracks[clip.track].async) asyncPictures.insert(clip.link);
    for (const auto& clip : p.show.clips)
        if (!p.show.tracks[clip.track].visual && !p.show.tracks[clip.track].async && asyncPictures.contains(clip.link))
            fail("A video on an async picture layer needs its linked sound on an async audio track");
    // Check each layer in timeline order, rather than comparing every clip pair.
    std::vector<const Clip*> pictures;
    for (const auto& clip : p.show.clips) if (p.show.tracks[clip.track].visual) pictures.push_back(&clip);
    std::sort(pictures.begin(), pictures.end(), [](const Clip* a, const Clip* b) {
        return a->track != b->track ? a->track < b->track : a->start < b->start;
    });
    for (std::size_t i = 1; i < pictures.size(); ++i)
        if (pictures[i - 1]->track == pictures[i]->track && pictures[i - 1]->end() > pictures[i]->start)
            fail("Picture clips overlap on one track; use a separate layer for a crossfade");
    ids.clear();
    for (const auto& m : p.show.markers) {
        if (m.id.empty() || !ids.insert(m.id).second) fail("Duplicate or missing marker ID");
        if (m.time < 0 || m.time > p.show.duration || !validMidi(m.message)) fail("Invalid marker time or MIDI message");
    }
}
namespace {
QJsonArray curveJson(const Envelope& e) {
    // Straight segments keep the original [time, value] pair; shaped ones add [shape, bend].
    QJsonArray a;
    for (const auto& p : e.points) {
        QJsonArray point{static_cast<double>(p.time), p.value};
        if (p.shape != CurveShape::Linear || p.bend != 0) { point.append(shapeName(p.shape)); point.append(p.bend); }
        a.append(point);
    }
    return a;
}
Envelope readCurve(const QJsonValue& value) {
    Envelope e;
    if (!value.isArray()) throw std::runtime_error("Automation must be an array");
    for (auto point : value.toArray()) {
        const auto a = point.toArray();
        if ((a.size() != 2 && a.size() != 4) || !a[0].isDouble() || !a[1].isDouble() || (a.size() == 4 && (!a[2].isString() || !a[3].isDouble())))
            throw std::runtime_error("Invalid automation data");
        const double t = a[0].toDouble();
        if (!std::isfinite(t) || t < 0 || t > maxFrame || std::floor(t) != t) throw std::runtime_error("Invalid keyframe time");
        Keyframe k{static_cast<Frame>(t), a[1].toDouble()};
        if (a.size() == 4) {
            const auto name = a[2].toString(); bool known = false;
            for (auto shape : {CurveShape::Linear, CurveShape::Hold, CurveShape::Curve, CurveShape::SCurve}) if (shapeName(shape) == name) { k.shape = shape; known = true; }
            if (!known) throw std::runtime_error("Invalid automation shape");
            k.bend = a[3].toDouble();
        }
        e.points.push_back(k);
    }
    return e;
}
Frame readFrame(const QJsonObject& o, const char* key) {
    const auto v = o.value(key); const double n = v.toDouble(-1);
    if (!v.isDouble() || !std::isfinite(n) || n < 0 || n > maxFrame || std::floor(n) != n)
        throw std::runtime_error(std::string("Invalid frame field: ") + key);
    return static_cast<Frame>(n);
}
// Optional fields keep their version-1 defaults when absent, but a present value must have the written type.
[[noreturn]] void invalidField(const char* key) { throw std::runtime_error(std::string("Invalid project field: ") + key); }
QJsonObject readObject(const QJsonValue& v) {
    if (!v.isObject()) throw std::runtime_error("Invalid project entry");
    return v.toObject();
}
QString readString(const QJsonObject& o, const char* key) {
    const auto v = o.value(key); if (v.isUndefined()) return {};
    if (!v.isString()) invalidField(key);
    return v.toString();
}
bool readBool(const QJsonObject& o, const char* key) {
    const auto v = o.value(key); if (v.isUndefined()) return false;
    if (!v.isBool()) invalidField(key);
    return v.toBool();
}
double readNumber(const QJsonObject& o, const char* key, double fallback) {
    const auto v = o.value(key); if (v.isUndefined()) return fallback;
    if (!v.isDouble() || !std::isfinite(v.toDouble())) invalidField(key);
    return v.toDouble();
}
int readInt(const QJsonObject& o, const char* key, int fallback) {
    const double n = readNumber(o, key, fallback);
    if (std::floor(n) != n || n < std::numeric_limits<int>::min() || n > std::numeric_limits<int>::max()) invalidField(key);
    return static_cast<int>(n);
}
MediaAsset loadAsset(const MediaAsset& entry, const std::function<bool()>& isCancelled = {}) {
    if (isCancelled && isCancelled()) throw std::runtime_error("Project load cancelled");
    try {
        auto a = loadMedia(entry.path, isCancelled);
        a.id = entry.id; if (!entry.name.isEmpty()) a.name = entry.name;
        return a;
    } catch (const std::exception& e) {
        if (isCancelled && isCancelled()) throw;
        // One missing or damaged file must not prevent opening the show.
        MediaAsset a = entry; a.offline = true; a.problem = QString::fromUtf8(e.what());
        if (a.name.isEmpty()) a.name = QFileInfo(a.path).fileName();
        return a;
    }
}
}
QJsonObject projectJson(const Project& p, const QString& directory) {
    QJsonArray tracks, clips, markers, assets;
    const QDir dir(directory);
    for (const auto& t : p.show.tracks) tracks.append(QJsonObject{
        {"id", QString::fromStdString(t.id)}, {"name", QString::fromStdString(t.name)}, {"visual", t.visual}, {"async", t.async},
        {"muted", t.muted}, {"gain", t.gain}, {"pan", t.pan}, {"left", t.left}, {"right", t.right}});
    for (const auto& c : p.show.clips) {
        QJsonObject clip{
            {"id", QString::fromStdString(c.id)}, {"asset", QString::fromStdString(c.assetId)}, {"text", QString::fromStdString(c.text)}, {"track", c.track},
            {"start", static_cast<double>(c.start)}, {"duration", static_cast<double>(c.duration)}, {"offset", static_cast<double>(c.offset)},
            {"loop", c.loop}, {"gain", c.gain}, {"pan", c.pan}, {"x", c.x}, {"y", c.y}, {"scale", c.scale}, {"opacity", c.opacity},
            {"volume", curveJson(c.volume)}, {"balance", curveJson(c.balance)}, {"positionX", curveJson(c.positionX)},
            {"positionY", curveJson(c.positionY)}, {"size", curveJson(c.size)}, {"alpha", curveJson(c.alpha)}};
        if (!c.link.empty()) clip.insert("link", QString::fromStdString(c.link));
        clips.append(clip);
    }
    for (const auto& m : p.show.markers) {
        QJsonArray bytes; for (auto b : m.message) bytes.append(b);
        markers.append(QJsonObject{{"id", QString::fromStdString(m.id)}, {"name", QString::fromStdString(m.name)},
            {"time", static_cast<double>(m.time)}, {"hold", m.hold}, {"timing", m.timing == MidiTiming::Arrival ? "arrival" : "resume"}, {"message", bytes}});
    }
    for (const auto& a : p.assets) {
        QString assetPath = a.path;
        if (!directory.isEmpty() && !assetPath.isEmpty()) {
            // Canonicalize the nearest existing ancestor directory (e.g. /tmp -> /private/tmp or
            // symlinked show folders, even when a synthetic/missing subdirectory does not exist),
            // while preserving media file symlinks (e.g. current.wav -> take-a.wav) verbatim.
            const QFileInfo assetInfo(assetPath);
            QDir parent = assetInfo.absoluteDir();
            QStringList trailing;
            QString canonicalAncestor = parent.canonicalPath();
            while (canonicalAncestor.isEmpty()) {
                const QString current = parent.absolutePath();
                const QString leaf = parent.dirName();
                if (leaf.isEmpty() || !parent.cdUp() || parent.absolutePath() == current) break;
                trailing.prepend(leaf);
                canonicalAncestor = parent.canonicalPath();
            }
            if (!canonicalAncestor.isEmpty()) {
                QDir rebuilt(canonicalAncestor);
                for (const QString& segment : trailing) rebuilt.setPath(rebuilt.filePath(segment));
                assetPath = rebuilt.filePath(assetInfo.fileName());
            }
            assetPath = dir.relativeFilePath(assetPath);
        }
        assets.append(QJsonObject{{"id", a.id}, {"name", a.name}, {"path", assetPath}});
    }
    QJsonObject root{{"format", "simpleavshow"}, {"version", 1}, {"title", p.title}, {"width", p.width}, {"height", p.height}, {"fps", p.fps},
        {"duration", static_cast<double>(p.show.duration)}, {"tracks", tracks}, {"clips", clips}, {"markers", markers}, {"assets", assets}};
    return root;
}
namespace {
QString canonicalManifestDirectory(const QString& filePath) {
    const QFileInfo fi(filePath);
    if (const QString canonicalFile = fi.canonicalFilePath(); !canonicalFile.isEmpty())
        return QFileInfo(canonicalFile).absolutePath();
    const QString canonicalDir = fi.absoluteDir().canonicalPath();
    return canonicalDir.isEmpty() ? fi.absolutePath() : canonicalDir;
}
}
void writeProject(const Project& p, const QString& path, bool allowOverwrite) {
    constexpr qint64 compactThresholdBytes = 32 * 1024 * 1024;
    constexpr qint64 maxProjectFileBytes = 128 * 1024 * 1024;
    validate(p);
    for (const auto& a : p.assets)
        if (a.path.isEmpty()) throw std::runtime_error("Use Export demo first to save generated media");
    const QFileInfo targetInfo(path);
    if (allowOverwrite && targetInfo.isSymLink()) {
        const QString canonical = targetInfo.canonicalFilePath();
        if (!canonical.isEmpty() && !canonical.endsWith(".savshow", Qt::CaseInsensitive))
            throw std::runtime_error("Refusing to overwrite symlink target that is not a .savshow file");
    }
    const QString resolvedPath = (allowOverwrite && targetInfo.isSymLink() && !targetInfo.canonicalFilePath().isEmpty())
        ? targetInfo.canonicalFilePath()
        : path;
    const auto normalizedPath = [](const QString& pth) {
        const QFileInfo fi(pth);
        const QString can = fi.canonicalFilePath();
        return can.isEmpty() ? QDir::cleanPath(fi.absoluteFilePath()) : can;
    };
    const QString resolvedNorm = normalizedPath(resolvedPath);
    for (const auto& a : p.assets) {
        if (!a.path.isEmpty() && normalizedPath(a.path) == resolvedNorm)
            throw std::runtime_error("Cannot overwrite a media file referenced by this show");
    }
    const auto root = projectJson(p, canonicalManifestDirectory(resolvedPath));
    QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (bytes.size() > compactThresholdBytes) bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (bytes.size() > maxProjectFileBytes)
        throw std::runtime_error("Show manifest exceeds the 128 MB save limit");
    if (!allowOverwrite && (QFileInfo::exists(path) || QFileInfo(path).isSymLink()))
        throw std::runtime_error("Destination file already exists; pass overwrite: true to replace it");
    const QFileInfo existingTarget(resolvedPath);
    const bool hadExistingTarget = allowOverwrite && existingTarget.exists();
    if (hadExistingTarget && !existingTarget.isWritable())
        throw std::runtime_error("Destination file is read-only");
    QTemporaryFile temp(QFileInfo(resolvedPath).absolutePath() + "/.simpleavshow-save-XXXXXX");
    temp.setAutoRemove(true);
    if (!temp.open()) throw std::runtime_error(temp.errorString().toStdString());
    if (temp.write(bytes) != bytes.size() || !temp.flush()) throw std::runtime_error(temp.errorString().toStdString());
    // Sync file data to durable storage before atomic rename so a power cut cannot leave an empty manifest.
    if (const int fd = temp.handle(); fd >= 0) {
#ifdef Q_OS_WIN
        if (HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd)); h != INVALID_HANDLE_VALUE) {
            if (!FlushFileBuffers(h))
                throw std::runtime_error("Cannot sync project file to disk (Win32 error " + std::to_string(GetLastError()) + ")");
        }
#elif defined(Q_OS_MACOS)
        if (::fcntl(fd, F_FULLFSYNC) != 0 && ::fsync(fd) != 0)
            throw std::runtime_error(std::string("Cannot sync project file to disk: ") + std::strerror(errno));
#else
        if (::fsync(fd) != 0)
            throw std::runtime_error(std::string("Cannot sync project file to disk: ") + std::strerror(errno));
#endif
    }
    const QString tempPath = temp.fileName();
    temp.close();
    if (hadExistingTarget) {
        // Preserve existing file permissions rather than widening private manifests (e.g. 0600 -> 0644).
        QFile::setPermissions(tempPath, existingTarget.permissions());
    }
    if (allowOverwrite) {
        // Atomic rename replaces the target entry directly and never follows a symlink created after pre-checks.
#ifdef Q_OS_WIN
        const std::wstring srcW = tempPath.toStdWString();
        const std::wstring dstW = resolvedPath.toStdWString();
        if (!MoveFileExW(srcW.c_str(), dstW.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("Cannot save project file (Win32 error " + std::to_string(GetLastError()) + ")");
        }
#else
        if (::rename(QFile::encodeName(tempPath).constData(), QFile::encodeName(resolvedPath).constData()) != 0) {
            throw std::runtime_error(std::string("Cannot save project file: ") + std::strerror(errno));
        }
#endif
        temp.setAutoRemove(false);
        return;
    }
#ifdef Q_OS_WIN
    const std::wstring srcW = tempPath.toStdWString();
    const std::wstring dstW = path.toStdWString();
    if (!MoveFileExW(srcW.c_str(), dstW.c_str(), MOVEFILE_WRITE_THROUGH)) {
        const DWORD err = GetLastError();
        if (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS)
            throw std::runtime_error("Destination file already exists; pass overwrite: true to replace it");
        throw std::runtime_error("Cannot save project file (Win32 error " + std::to_string(err) + ")");
    }
    temp.setAutoRemove(false);
#else
    const QByteArray src = QFile::encodeName(tempPath);
    const QByteArray dst = QFile::encodeName(path);
#ifdef Q_OS_MACOS
    if (::renamex_np(src.constData(), dst.constData(), RENAME_EXCL) == 0) {
        temp.setAutoRemove(false);
        return;
    }
    if (errno == EEXIST)
        throw std::runtime_error("Destination file already exists; pass overwrite: true to replace it");
#elif defined(Q_OS_LINUX) && defined(SYS_renameat2) && defined(RENAME_NOREPLACE)
    if (::syscall(SYS_renameat2, AT_FDCWD, src.constData(), AT_FDCWD, dst.constData(), RENAME_NOREPLACE) == 0) {
        temp.setAutoRemove(false);
        return;
    }
    if (errno == EEXIST)
        throw std::runtime_error("Destination file already exists; pass overwrite: true to replace it");
#endif
    if (::link(src.constData(), dst.constData()) == 0) {
        return; // temp auto-removes its temporary directory entry while dst keeps the linked inode.
    }
    if (errno == EEXIST)
        throw std::runtime_error("Destination file already exists; pass overwrite: true to replace it");
    if (errno == EPERM || errno == EOPNOTSUPP || errno == ENOTSUP || errno == EXDEV || errno == ENOSYS || errno == EINVAL)
        throw std::runtime_error("This filesystem does not support atomic no-overwrite save; choose another location or pass overwrite: true");
    throw std::runtime_error(std::string("Cannot save project file: ") + std::strerror(errno));
#endif
}
Project parseProject(const QByteArray& json, const QString& directory, const QString& fallbackDirectory) {
    QJsonParseError parseError; const auto doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) throw std::runtime_error("Invalid project JSON");
    const auto root = doc.object();
    if (root["format"] != "simpleavshow" || root["version"].toInt() != 1) throw std::runtime_error("Unsupported project version");
    for (const char* name : {"tracks", "clips", "markers", "assets"}) if (!root[name].isArray()) throw std::runtime_error("Missing project array");
    Project p; p.title = readString(root, "title"); p.width = readInt(root, "width", 0); p.height = readInt(root, "height", 0); p.fps = readNumber(root, "fps", 0);
    p.show.duration = readFrame(root, "duration");
    const QDir dir(directory);
    const QDir fallbackDir(fallbackDirectory);
    for (auto value : root["assets"].toArray()) {
        const auto o = readObject(value); MediaAsset a; a.id = readString(o, "id"); a.name = readString(o, "name");
        if (a.id.isEmpty() || p.assets.contains(a.id)) throw std::runtime_error("Duplicate or missing asset ID");
        const auto source = readString(o, "path"); if (source.isEmpty()) invalidField("path");
        QString resolved = dir.absoluteFilePath(source);
        if (!fallbackDirectory.isEmpty() && !QDir::isAbsolutePath(source) && !QFileInfo::exists(resolved)) {
            const QString legacy = fallbackDir.absoluteFilePath(source);
            if (QFileInfo::exists(legacy)) resolved = legacy;
        }
        a.path = resolved; p.assets.insert(a.id, std::move(a));
    }
    for (auto value : root["tracks"].toArray()) {
        const auto o = readObject(value);
        p.show.tracks.push_back({readString(o, "id").toStdString(), readString(o, "name").toStdString(), readBool(o, "visual"), readBool(o, "async"),
            readBool(o, "muted"), readNumber(o, "gain", 1), readNumber(o, "pan", 0), readInt(o, "left", -1), readInt(o, "right", -1)});
    }
    for (auto value : root["clips"].toArray()) {
        const auto o = readObject(value); Clip c;
        c.id = readString(o, "id").toStdString(); c.assetId = readString(o, "asset").toStdString(); c.text = readString(o, "text").toStdString(); c.track = readInt(o, "track", -1);
        c.link = readString(o, "link").toStdString();
        c.start = readFrame(o, "start"); c.duration = readFrame(o, "duration"); c.offset = readFrame(o, "offset"); c.loop = readBool(o, "loop");
        c.gain = readNumber(o, "gain", 1); c.pan = readNumber(o, "pan", 0); c.x = readNumber(o, "x", 0); c.y = readNumber(o, "y", 0);
        c.scale = readNumber(o, "scale", 1); c.opacity = readNumber(o, "opacity", 1);
        c.volume = readCurve(o["volume"]); c.balance = readCurve(o["balance"]); c.positionX = readCurve(o["positionX"]);
        c.positionY = readCurve(o["positionY"]); c.size = readCurve(o["size"]); c.alpha = readCurve(o["alpha"]);
        p.show.clips.push_back(std::move(c));
    }
    for (auto value : root["markers"].toArray()) {
        const auto o = readObject(value); Marker m;
        m.id = readString(o, "id").toStdString(); m.name = readString(o, "name").toStdString(); m.time = readFrame(o, "time"); m.hold = readBool(o, "hold");
        const QString timing = readString(o, "timing");
        if (timing != "arrival" && timing != "resume") throw std::runtime_error("Invalid MIDI timing");
        m.timing = timing == "resume" ? MidiTiming::Resume : MidiTiming::Arrival;
        if (!o["message"].isArray()) throw std::runtime_error("Invalid MIDI bytes");
        for (auto byte : o["message"].toArray()) {
            const double number = byte.toDouble(-1);
            if (!byte.isDouble() || number < 0 || number > 255 || std::floor(number) != number) throw std::runtime_error("Invalid MIDI byte");
            m.message.push_back(static_cast<unsigned char>(number));
        }
        p.show.markers.push_back(std::move(m));
    }
    return p;
}
void loadProjectMedia(Project& p, const std::function<bool()>& isCancelled) {
    for (auto& a : p.assets) {
        if (isCancelled && isCancelled()) throw std::runtime_error("Project load cancelled");
        a = loadAsset(a, isCancelled);
    }
    if (isCancelled && isCancelled()) throw std::runtime_error("Project load cancelled");
    bindAudio(p); validate(p);
}
Project readProject(const QString& path, const std::function<bool()>& isCancelled) {
    if (isCancelled && isCancelled()) throw std::runtime_error("Project load cancelled");
    if (isProjectPackage(path)) return readProjectPackage(path, isCancelled);
    constexpr qint64 maxProjectFileBytes = 128 * 1024 * 1024;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error(file.errorString().toStdString());
    // Reject non-JSON binary files immediately before allocating a manifest buffer,
    // while allowing an optional UTF-8 BOM and arbitrary leading JSON whitespace.
    qint64 contentStart = 0;
    {
        char bom[3]{};
        const qint64 nBom = file.peek(bom, 3);
        if (nBom == 3 && static_cast<unsigned char>(bom[0]) == 0xef &&
            static_cast<unsigned char>(bom[1]) == 0xbb && static_cast<unsigned char>(bom[2]) == 0xbf) {
            contentStart = 3;
            file.seek(3);
        }
        bool foundObject = false;
        QByteArray chunk(4096, '\0');
        qint64 scanned = 0;
        while (!file.atEnd() && scanned < maxProjectFileBytes) {
            const qint64 n = file.read(chunk.data(), chunk.size());
            if (n <= 0) break;
            scanned += n;
            for (qint64 i = 0; i < n; ++i) {
                const char ch = chunk[static_cast<qsizetype>(i)];
                if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
                if (ch == '{') { foundObject = true; break; }
                throw std::runtime_error("Invalid project JSON");
            }
            if (foundObject) break;
        }
        if (!foundObject) throw std::runtime_error("Invalid project JSON");
        if (!file.seek(contentStart)) throw std::runtime_error(file.errorString().toStdString());
    }
    if (file.size() > maxProjectFileBytes)
        throw std::runtime_error("Project file is too large to be a valid .savshow manifest");
    const QByteArray bytes = file.read(maxProjectFileBytes + 1);
    if (bytes.size() > maxProjectFileBytes)
        throw std::runtime_error("Project file is too large to be a valid .savshow manifest");
    const QString canonicalDir = canonicalManifestDirectory(path);
    const QFileInfo openInfo(path);
    const QString aliasDir = openInfo.absolutePath();
    const QString fallbackDir = (openInfo.isSymLink() && !aliasDir.isEmpty() && aliasDir != canonicalDir)
        ? aliasDir
        : QString();
    Project p = parseProject(bytes, canonicalDir, fallbackDir);
    loadProjectMedia(p, isCancelled); return p;
}
namespace {
void copyFileCancellable(const QString& source, const QString& target, const std::function<bool()>& isCancelled) {
    QFile in(source);
    if (!in.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read packaged media: " + source.toStdString());
    QSaveFile out(target);
    if (!out.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot copy packaged media: " + target.toStdString());
    QByteArray buffer(1024 * 1024, '\0');
    while (!in.atEnd()) {
        if (isCancelled && isCancelled()) throw std::runtime_error("Save cancelled");
        const qint64 n = in.read(buffer.data(), buffer.size());
        if (n < 0) throw std::runtime_error("Cannot read packaged media: " + source.toStdString());
        if (n == 0) break;
        if (out.write(buffer.constData(), n) != n) throw std::runtime_error("Cannot write packaged media: " + target.toStdString());
    }
    if (isCancelled && isCancelled()) throw std::runtime_error("Save cancelled");
    if (!out.commit()) throw std::runtime_error("Cannot finalize packaged media: " + target.toStdString());
}

void writeGeneratedAsset(const MediaAsset& a, const QString& target, const std::function<bool()>& isCancelled) {
    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error(file.errorString().toStdString());
    if (a.image) {
        if (isCancelled && isCancelled()) throw std::runtime_error("Save cancelled");
        if (!a.still.save(&file, "PNG")) throw std::runtime_error("Cannot save generated image");
    } else if (a.audio && !a.audio->storage) {
        QDataStream out(&file); out.setByteOrder(QDataStream::LittleEndian);
        const quint16 channels = a.audio->mono ? 1 : 2;
        const auto size = static_cast<quint64>(a.audio->length()) * channels * 2;
        if (size > std::numeric_limits<quint32>::max() - 36) throw std::runtime_error("Generated audio is too large for WAV export");
        const auto bytes = static_cast<quint32>(size);
        out.writeRawData("RIFF", 4); out << quint32(bytes + 36); out.writeRawData("WAVEfmt ", 8);
        out << quint32(16) << quint16(1) << channels << quint32(sampleRate) << quint32(sampleRate * channels * 2) << quint16(channels * 2) << quint16(16);
        out.writeRawData("data", 4); out << bytes;
        const std::size_t step = a.audio->mono ? 2 : 1;
        for (std::size_t i = 0; i < a.audio->samples.size(); i += step) {
            if (((i / step) & 0xffff) == 0 && isCancelled && isCancelled()) throw std::runtime_error("Save cancelled");
            out << static_cast<qint16>(std::clamp(a.audio->samples[i], -1.f, 1.f) * 32767);
        }
        if (out.status() != QDataStream::Ok) throw std::runtime_error("Cannot write generated audio");
    } else {
        throw std::runtime_error("Asset has no source file");
    }
    if (isCancelled && isCancelled()) throw std::runtime_error("Save cancelled");
    if (!file.commit()) throw std::runtime_error(file.errorString().toStdString());
}
}

void materializeProjectMedia(Project& p, const QString& directory, const std::function<bool()>& isCancelled) {
    QDir dir(directory);
    const bool existed = dir.exists();
    QMap<QString, QString> copied;
    QStringList created;
    try {
        for (auto& a : p.assets) {
            if (isCancelled && isCancelled()) throw std::runtime_error("Save cancelled");
            const bool extracted = p.packageDirectory && !a.path.isEmpty() &&
                QFileInfo(a.path).absoluteFilePath().startsWith(p.packageDirectory->path() + "/");
            if (!a.path.isEmpty() && !extracted) continue;
            if (!QDir().mkpath(dir.absolutePath())) throw std::runtime_error("Cannot create media directory");
            if (extracted) {
                if (!copied.contains(a.path)) {
                    const QString target = dir.filePath(newId() + "." + QFileInfo(a.path).suffix());
                    copyFileCancellable(a.path, target, isCancelled);
                    created.append(target); copied.insert(a.path, target);
                }
                a.path = copied.value(a.path);
                // Direct PCM storage must follow the saved copy before package
                // extraction is released. Converted sources reuse their disk cache.
                if (a.audio) a.audio = loadMedia(a.path).audio;
                continue;
            }
            const QString target = dir.filePath(newId() + (a.image ? ".png" : ".wav"));
            writeGeneratedAsset(a, target, isCancelled);
            created.append(target); a.path = target;
        }
    } catch (...) {
        for (const auto& file : created) QFile::remove(file);
        if (!existed) QDir().rmdir(dir.absolutePath());
        throw;
    }
    p.packageDirectory.reset();
    bindAudio(p);
}

StagedProjectMedia stageProjectMedia(const Project& p, const QString& stagingDirectory,
                                     const QString& finalTargetDirectory,
                                     const std::function<bool()>& isCancelled) {
    if (isCancelled && isCancelled()) throw std::runtime_error("Save cancelled");
    QDir dir(stagingDirectory);
    QDir finalDir(finalTargetDirectory);
    if (!QDir().mkpath(dir.absolutePath())) throw std::runtime_error("Cannot create media staging directory");
    StagedProjectMedia staged;
    QMap<QString, QString> copiedNames;
    QMap<QString, qint64> stagedSizes;
    for (auto it = p.assets.cbegin(); it != p.assets.cend(); ++it) {
        if (isCancelled && isCancelled()) throw std::runtime_error("Save cancelled");
        const auto& a = it.value();
        const bool extracted = p.packageDirectory && !a.path.isEmpty() &&
            QFileInfo(a.path).absoluteFilePath().startsWith(p.packageDirectory->path() + "/");
        if (!a.path.isEmpty() && !extracted) continue;
        if (extracted) {
            if (!copiedNames.contains(a.path)) {
                const QString fileName = newId() + "." + QFileInfo(a.path).suffix();
                const QString stagedPath = dir.filePath(fileName);
                copyFileCancellable(a.path, stagedPath, isCancelled);
                copiedNames.insert(a.path, fileName);
                stagedSizes.insert(fileName, QFileInfo(stagedPath).size());
            }
            const QString fileName = copiedNames.value(a.path);
            staged.assetFileNames.insert(it.key(), fileName);
            // Pre-build rebound WaveStorage + peaks on the worker thread so GUI publish is pointer-swap only.
            if (a.audio) {
                if (auto rebound = detail::rebindPcmWavePath(a.audio, finalDir.filePath(fileName), stagedSizes.value(fileName, -1)))
                    staged.reboundAudio.insert(it.key(), std::move(rebound));
            }
        } else {
            const QString fileName = newId() + (a.image ? ".png" : ".wav");
            writeGeneratedAsset(a, dir.filePath(fileName), isCancelled);
            staged.assetFileNames.insert(it.key(), fileName);
        }
    }
    return staged;
}

void PublishedMediaRollback::rollback() const {
    for (const auto& file : movedFiles) QFile::remove(file);
    if (directoryCreated && !targetDirectory.isEmpty()) QDir().rmdir(targetDirectory);
}

PublishedMediaRollback publishStagedProjectMedia(Project& p, const QString& stagingDirectory, const QString& targetDirectory, const StagedProjectMedia& staged) {
    PublishedMediaRollback rollback;
    if (staged.assetFileNames.isEmpty()) {
        p.packageDirectory.reset();
        bindAudio(p);
        return rollback;
    }
    QDir stageDir(stagingDirectory);
    QDir targetDir(targetDirectory);
    rollback.targetDirectory = targetDir.absolutePath();
    rollback.directoryCreated = !targetDir.exists();
    QMap<QString, QString> publishedNames;
    try {
        if (!QDir().mkpath(targetDir.absolutePath())) throw std::runtime_error("Cannot create media directory");
        for (auto it = staged.assetFileNames.cbegin(); it != staged.assetFileNames.cend(); ++it) {
            const QString& fileName = it.value();
            if (!publishedNames.contains(fileName)) {
                const QString src = stageDir.filePath(fileName);
                const QString dst = targetDir.filePath(fileName);
                QString renameErr;
                if (!nativeSameFsRename(src, dst, renameErr))
                    throw std::runtime_error("Cannot publish staged media atomically: " + dst.toStdString() + " (" + renameErr.toStdString() + ")");
                rollback.movedFiles.append(dst);
                publishedNames.insert(fileName, dst);
            }
        }
        for (auto it = staged.assetFileNames.cbegin(); it != staged.assetFileNames.cend(); ++it) {
            auto assetIt = p.assets.find(it.key());
            if (assetIt == p.assets.end()) continue;
            assetIt->path = publishedNames.value(it.value());
            if (const auto rebound = staged.reboundAudio.constFind(it.key()); rebound != staged.reboundAudio.cend()) {
                assetIt->audio = rebound.value();
            }
        }
    } catch (...) {
        rollback.rollback();
        throw;
    }
    p.packageDirectory.reset();
    bindAudio(p);
    return rollback;
}
}
