#include "media/AudioSources.h"
#include "core/AudioBuffer.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QMutex>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QSysInfo>
#include <QTemporaryFile>
#include <QtEndian>
#include <cstring>
#include <map>
#include <stdexcept>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

namespace sav::detail {
namespace {
constexpr qint64 stereoBytes = 2 * sizeof(float);
constexpr qint64 legacyHeader = 8 + 8 + 1 + 32 + 32, metadataHeader = legacyHeader + 32;
constexpr char legacyMagic[] = "SAVPCM01", cacheMagic[] = "SAVMETA2";
constexpr char indexMagic[] = "SAVIDX01";
constexpr qint64 staleAge = 7ll * 24 * 3600 * 1000;
constexpr qint64 racyWindow = 2000; // Coarse (FAT: 2 s) timestamps can hide a rewrite during hashing.
constexpr qsizetype maxIndexEntries = 4096;
[[noreturn]] void fail(const QString& message) { throw std::runtime_error(message.toStdString()); }
[[noreturn]] void changed(const QString& message) { throw AudioSourceChanged(message.toStdString()); }

QByteArray fileHash(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) fail("Cannot read audio source: " + file.errorString());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const auto bytes = file.read(1024 * 1024);
        if (bytes.isEmpty() || file.error() != QFileDevice::NoError) fail("Cannot checksum audio source: " + file.errorString());
        hash.addData(bytes);
    }
    return hash.result();
}
// Canonical path, size and modification time identify an unchanged file. Content
// rewritten with the same size and timestamp is not noticed; size or timestamp
// changes (every normal save or copy) are.
struct Stamp {
    QString path; qint64 size = -1, modified = 0;
    bool operator==(const Stamp&) const = default;
};
Stamp stamp(const QString& path) {
    const QFileInfo info(path);
    return {info.canonicalFilePath(), info.size(), info.lastModified().toMSecsSinceEpoch()};
}
// Records only files that did not change while hashed and were not modified within
// the timestamp granularity, so a later same-second rewrite still gets a new stamp.
bool settled(const Stamp& before, const Stamp& after, qint64 started) {
    return !before.path.isEmpty() && before == after && started - before.modified > racyWindow;
}

// The index maps source stamps to content hashes so unchanged sources skip the
// full-file hash. Readers see whole files (QSaveFile); writers serialize on a lock
// file and skip the update if it is busy, since the index only saves work.
struct IndexEntry { Stamp source; QByteArray hash; };
QString indexPath() { return audioCacheDirectory() + "/source-index"; }
QList<IndexEntry> readIndex() {
    QList<IndexEntry> entries;
    QFile file(indexPath());
    if (!file.open(QIODevice::ReadOnly)) return entries;
    QDataStream stream(&file); stream.setByteOrder(QDataStream::LittleEndian);
    char magic[8]; quint32 count = 0;
    if (stream.readRawData(magic, 8) != 8 || std::memcmp(magic, indexMagic, 8)) return entries;
    stream >> count;
    for (quint32 i = 0; i < std::min<quint32>(count, maxIndexEntries); ++i) {
        IndexEntry entry; stream >> entry.source.path >> entry.source.size >> entry.source.modified >> entry.hash;
        if (stream.status() != QDataStream::Ok) break;
        if (entry.hash.size() == 32 && !entry.source.path.isEmpty()) entries.push_back(std::move(entry));
    }
    return entries;
}
void updateIndex(const Stamp& source, const QByteArray& hash) {
    if (source.path.isEmpty() || !QDir().mkpath(audioCacheDirectory())) return;
    QLockFile lock(indexPath() + ".lock");
    if (!lock.tryLock(5000)) return;
    auto entries = readIndex();
    entries.removeIf([&](const IndexEntry& entry) { return entry.source.path == source.path; });
    if (!hash.isEmpty()) entries.push_back({source, hash});
    if (entries.size() > maxIndexEntries) entries.remove(0, entries.size() - maxIndexEntries);
    QSaveFile file(indexPath());
    if (!file.open(QIODevice::WriteOnly)) return;
    QDataStream stream(&file); stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData(indexMagic, 8); stream << static_cast<quint32>(entries.size());
    for (const auto& entry : entries) stream << entry.source.path << entry.source.size << entry.source.modified << entry.hash;
    if (stream.status() == QDataStream::Ok) file.commit();
}
QByteArray sourceHash(const QString& path) {
    const auto before = stamp(path);
    if (!before.path.isEmpty())
        for (const auto& entry : readIndex()) if (entry.source == before) return entry.hash;
    const auto started = QDateTime::currentMSecsSinceEpoch();
    auto hash = fileHash(path);
    if (settled(before, stamp(path), started)) updateIndex(before, hash);
    return hash;
}

QByteArray peaksHash(const std::vector<float>& peaks) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView(reinterpret_cast<const char*>(peaks.data()), static_cast<qsizetype>(peaks.size() * sizeof(float))));
    return hash.result();
}
// Bump the revision when decoding changes so stale caches are rebuilt (02: one-channel input is centre mono).
const QByteArray& decoderRevision() {
    static const auto revision = QByteArray("SAVPCM02:") + QByteArray::number(avformat_version()) + ':' +
        QByteArray::number(avcodec_version()) + ':' + QByteArray::number(swresample_version());
    return revision;
}
const QByteArray& revisionId() {
    static const auto id = QCryptographicHash::hash(decoderRevision(), QCryptographicHash::Sha256);
    return id;
}
void touch(const QString& path) {
    QFile file(path);
    if (file.open(QIODevice::ReadWrite)) file.setFileTime(QDateTime::currentDateTimeUtc(), QFileDevice::FileModificationTime);
}
void writeMetadata(const QString& path, Frame length, bool mono, const QByteArray& checksum, const std::vector<float>& peaks) {
    QSaveFile metadata(path);
    if (!metadata.open(QIODevice::WriteOnly)) fail("Cannot save audio cache metadata: " + metadata.errorString());
    QDataStream stream(&metadata); stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData(cacheMagic, 8); stream.writeRawData(revisionId().constData(), 32);
    stream << static_cast<qint64>(length) << static_cast<quint8>(mono);
    stream.writeRawData(checksum.constData(), 32); stream.writeRawData(peaksHash(peaks).constData(), 32);
    const auto peakBytes = static_cast<qint64>(peaks.size() * sizeof(float));
    if (metadata.write(reinterpret_cast<const char*>(peaks.data()), peakBytes) != peakBytes ||
        stream.status() != QDataStream::Ok || !metadata.commit()) fail("Cannot finish audio cache metadata: " + metadata.errorString());
}

// Caches referenced by live storage in this process. Each also holds a per-process
// marker lock so other instances keep it too; Qt treats markers of dead processes as stale.
struct CacheUse { int owners = 0; std::unique_ptr<QLockFile> marker; };
QMutex usesMutex;
std::map<QString, CacheUse>& cacheUses() { static std::map<QString, CacheUse> uses; return uses; }
void retainCache(const QString& pcm) {
    const QMutexLocker locker(&usesMutex);
    auto& use = cacheUses()[pcm];
    if (use.owners++) return;
    use.marker = std::make_unique<QLockFile>(pcm.chopped(4) + '.' + QString::number(QCoreApplication::applicationPid()) + ".use");
    use.marker->setStaleLockTime(0);
    if (!use.marker->tryLock(0)) use.marker.reset();
}
void releaseCache(const QString& pcm) {
    const QMutexLocker locker(&usesMutex);
    const auto use = cacheUses().find(pcm);
    if (use != cacheUses().end() && !--use->second.owners) cacheUses().erase(use);
}
bool cacheInUse(const QString& pcm) {
    {
        const QMutexLocker locker(&usesMutex);
        if (cacheUses().count(pcm)) return true;
    }
    const QFileInfo info(pcm);
    for (const auto& marker : info.dir().entryInfoList({info.completeBaseName() + ".*.use"}, QDir::Files)) {
        QLockFile other(marker.filePath()); other.setStaleLockTime(0);
        if (!other.tryLock(0)) return true;
        other.unlock(); // Left by a dead process; unlocking removes it.
    }
    return false;
}

void checkRange(Frame first, std::size_t count, Frame length) {
    if (first < 0 || first > length || count > static_cast<std::size_t>(length - first)) fail("Invalid audio source range");
}

class PcmStorage final : public AudioStorage {
public:
    PcmStorage(QString path, Frame length) : path_(std::move(path)), length_(length), modified_(QFileInfo(path_).lastModified()) { retainCache(path_); }
    ~PcmStorage() override { releaseCache(path_); }
    Frame length() const noexcept override { return length_; }
    void read(Frame first, float* stereo, std::size_t count) const override {
        checkRange(first, count, length_);
        const QFileInfo info(path_);
        QFile file(path_);
        const auto bytes = static_cast<qint64>(count) * stereoBytes;
        if (info.exists() && (info.size() != length_ * stereoBytes || info.lastModified() != modified_))
            changed("Prepared audio cache changed: " + path_);
        if (!file.open(QIODevice::ReadOnly) || !file.seek(first * stereoBytes) || file.read(reinterpret_cast<char*>(stereo), bytes) != bytes)
            fail("Cannot read prepared audio cache: " + path_);
    }
private:
    QString path_;
    Frame length_;
    QDateTime modified_;
};

class WaveStorage final : public AudioStorage {
public:
    WaveStorage(QString path, qint64 offset, Frame length, int channels, int bits, bool floating)
        : path_(std::move(path)), offset_(offset), length_(length), channels_(channels), bits_(bits), floating_(floating),
          size_(QFileInfo(path_).size()) {}
    Frame length() const noexcept override { return length_; }
    void prepare(AudioData& data) {
        data.peaks.resize(static_cast<std::size_t>((length_ + AudioData::peakStride - 1) / AudioData::peakStride));
        std::array<float, AudioBuffer::blockFrames * 2> scratch{};
        for (Frame first = 0; first < length_; first += AudioBuffer::blockFrames) {
            const auto count = std::min(AudioBuffer::blockFrames, length_ - first);
            const auto bytes = readBytes(first, static_cast<std::size_t>(count));
            blockHashes_.push_back(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
            decode(reinterpret_cast<const uchar*>(bytes.constData()), scratch.data(), static_cast<std::size_t>(count));
            for (Frame i = 0; i < count; ++i) {
                auto& peak = data.peaks[static_cast<std::size_t>((first + i) / AudioData::peakStride)];
                peak = std::max(peak, std::max(std::abs(scratch[i * 2]), std::abs(scratch[i * 2 + 1])));
            }
        }
    }
    void read(Frame first, float* stereo, std::size_t count) const override {
        checkRange(first, count, length_);
        if (!count) return;
        const Frame blockFirst = first / AudioBuffer::blockFrames * AudioBuffer::blockFrames;
        const Frame end = std::min(length_, (first + static_cast<Frame>(count) + AudioBuffer::blockFrames - 1) / AudioBuffer::blockFrames * AudioBuffer::blockFrames);
        const auto bytes = readBytes(blockFirst, static_cast<std::size_t>(end - blockFirst));
        const auto stride = channels_ * (bits_ / 8);
        for (Frame frame = blockFirst; frame < end; frame += AudioBuffer::blockFrames) {
            const auto size = std::min(AudioBuffer::blockFrames, end - frame) * stride;
            const QByteArrayView block(bytes.constData() + (frame - blockFirst) * stride, static_cast<qsizetype>(size));
            if (QCryptographicHash::hash(block, QCryptographicHash::Sha256) != blockHashes_[static_cast<std::size_t>(frame / AudioBuffer::blockFrames)])
                changed("PCM audio source changed; reopen the project: " + path_);
        }
        decode(reinterpret_cast<const uchar*>(bytes.constData()) + (first - blockFirst) * stride, stereo, count);
    }
private:
    QByteArray readBytes(Frame first, std::size_t count) const {
        const QFileInfo info(path_);
        const qint64 stride = channels_ * (bits_ / 8);
        QFile file(path_);
        // Block hashes verify content, so a touched modification time alone is harmless.
        if (info.exists() && info.size() != size_) changed("PCM audio source changed; reopen the project: " + path_);
        if (!file.open(QIODevice::ReadOnly) || !file.seek(offset_ + first * stride))
            fail("PCM audio source is unavailable: " + path_);
        const auto bytes = file.read(static_cast<qint64>(count) * stride);
        if (bytes.size() != static_cast<qint64>(count) * stride) fail("Cannot read PCM audio source: " + path_);
        return bytes;
    }
    void decode(const uchar* source, float* stereo, std::size_t count) const {
        for (std::size_t i = 0; i < count; ++i) {
            float samples[2]{};
            for (int c = 0; c < channels_; ++c, source += bits_ / 8) {
                if (floating_) {
                    const quint32 value = qFromLittleEndian<quint32>(source);
                    std::memcpy(&samples[c], &value, sizeof(float));
                } else if (bits_ == 8) samples[c] = (static_cast<int>(*source) - 128) / 128.f;
                else if (bits_ == 16) samples[c] = qFromLittleEndian<qint16>(source) / 32768.f;
                else if (bits_ == 24) {
                    qint32 value = source[0] | (source[1] << 8) | (source[2] << 16);
                    if (value & 0x800000) value -= 0x1000000;
                    samples[c] = static_cast<float>(value / 8388608.);
                } else samples[c] = static_cast<float>(qFromLittleEndian<qint32>(source) / 2147483648.);
            }
            stereo[i * 2] = samples[0]; stereo[i * 2 + 1] = channels_ == 1 ? samples[0] : samples[1];
        }
    }
    QString path_;
    qint64 offset_;
    Frame length_;
    int channels_, bits_;
    bool floating_;
    qint64 size_;
    std::vector<QByteArray> blockHashes_;
};
}

QString audioCacheDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
        (QSysInfo::ByteOrder == QSysInfo::LittleEndian ? "/audio-pcm-v1-le" : "/audio-pcm-v1-be");
}

AudioCache::AudioCache(const QString& source) : source_(source), sourceHash_(sourceHash(source)) {
    // Content identity also survives moved media and fresh package extraction paths.
    const auto key = QString::fromLatin1(QCryptographicHash::hash(decoderRevision() + sourceHash_, QCryptographicHash::Sha256).toHex());
    const QDir directory(audioCacheDirectory());
    if (!QDir().mkpath(directory.path())) fail("Cannot create audio cache directory: " + directory.path());
    pcm_ = directory.filePath(key + ".pcm"); metadata_ = directory.filePath(key + ".meta");
    lock_ = std::make_unique<QLockFile>(directory.filePath(key + ".lock"));
    lock_->setStaleLockTime(0); // A lengthy conversion is not a stale lock; dead processes are detected by Qt.
    if (!lock_->tryLock(30000)) fail("Audio preparation is already in progress; try opening the project again");
}
AudioCache::~AudioCache() = default;
QString AudioCache::temporaryTemplate() const { return audioCacheDirectory() + "/preparing-XXXXXX"; }

std::shared_ptr<const AudioData> AudioCache::reuse(bool mono) const {
    QFile metadata(metadata_);
    if (!metadata.open(QIODevice::ReadOnly)) return {};
    QDataStream stream(&metadata); stream.setByteOrder(QDataStream::LittleEndian);
    char magic[8], revision[32], pcmHash[32], waveformHash[32]; qint64 length = 0; quint8 savedMono = 0;
    if (stream.readRawData(magic, 8) != 8) return {};
    // The key already covers the revision, so older metadata without it is upgraded in place.
    const bool legacy = !std::memcmp(magic, legacyMagic, 8);
    if (!legacy && (std::memcmp(magic, cacheMagic, 8) || stream.readRawData(revision, 32) != 32 ||
        QByteArray(revision, 32) != revisionId())) return {};
    stream >> length >> savedMono;
    if (length <= 0 || length > maxFrame || savedMono > 1 || static_cast<bool>(savedMono) != mono ||
        QFileInfo(pcm_).size() != length * stereoBytes) return {};
    const auto peakCount = (length + AudioData::peakStride - 1) / AudioData::peakStride;
    const auto peakBytes = peakCount * static_cast<qint64>(sizeof(float));
    if (metadata.size() != (legacy ? legacyHeader : metadataHeader) + peakBytes || stream.readRawData(pcmHash, 32) != 32 || stream.readRawData(waveformHash, 32) != 32) return {};
    auto data = std::make_shared<AudioData>(); data->mono = mono;
    data->peaks.resize(static_cast<std::size_t>(peakCount));
    if (metadata.read(reinterpret_cast<char*>(data->peaks.data()), peakBytes) != peakBytes ||
        peaksHash(data->peaks) != QByteArray(waveformHash, 32) ||
        !std::all_of(data->peaks.begin(), data->peaks.end(), [](float value) { return std::isfinite(value) && value >= 0; })) return {};
    try {
        if (fileHash(pcm_) != QByteArray(pcmHash, 32)) return {};
    } catch (const std::runtime_error&) { return {}; }
    metadata.close();
    // The metadata timestamp records use for least-recently-used trimming.
    if (!legacy) touch(metadata_);
    else try { writeMetadata(metadata_, length, mono, QByteArray(pcmHash, 32), data->peaks); } catch (const std::runtime_error&) {}
    data->storage = std::make_shared<PcmStorage>(pcm_, length);
    return data;
}

std::shared_ptr<const AudioStorage> AudioCache::publish(QTemporaryFile& file, const AudioData& data, Frame length) const {
    if (length <= 0 || length > maxFrame || file.size() != length * stereoBytes ||
        data.peaks.size() != static_cast<std::size_t>((length + AudioData::peakStride - 1) / AudioData::peakStride)) fail("Invalid prepared audio length");
    if (!file.flush()) fail("Cannot finish audio cache: " + file.errorString());
    if (fileHash(source_) != sourceHash_) {
        updateIndex(stamp(source_), {}); // An indexed hash may predate a same-stamp rewrite.
        fail("Audio source changed during preparation; import it again");
    }
    const auto checksum = fileHash(file.fileName());
    bool same = false;
    if (QFileInfo(pcm_).size() == file.size()) {
        try { same = fileHash(pcm_) == checksum; } catch (const std::runtime_error&) {}
    }
    if (!same) {
        // Publish only complete files. The metadata is the atomic commit marker;
        // interrupted/failed writes are rejected and rebuilt on the next import.
        if (QFile::exists(pcm_) && !QFile::remove(pcm_)) fail("Cannot replace damaged audio cache: " + pcm_);
        file.close();
        if (!file.rename(pcm_)) fail("Cannot publish audio cache: " + file.errorString());
        file.setAutoRemove(false);
    }
    writeMetadata(metadata_, length, data.mono, checksum, data.peaks);
    auto storage = std::make_shared<PcmStorage>(pcm_, length);
    trimAudioCache();
    return storage;
}

void trimAudioCache(qint64 maxBytes) {
    const QDir directory(audioCacheDirectory());
    if (!directory.exists()) return;
    QLockFile trimming(directory.filePath("trim.lock")); trimming.setStaleLockTime(0);
    if (!trimming.tryLock(0)) return; // Another import or instance is already trimming.
    struct Entry { QString key; qint64 bytes = 0, used = 0; bool committed = false, current = false; };
    std::map<QString, Entry> entries;
    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (const auto& file : directory.entryInfoList(QDir::Files)) {
        const auto modified = file.lastModified().toMSecsSinceEpoch();
        // Live conversions keep writing; a day-old temporary was left by a crash.
        if (file.fileName().startsWith("preparing-")) {
            if (now - modified > 24ll * 3600 * 1000) QFile::remove(file.filePath());
            continue;
        }
        if (file.suffix() != "pcm" && file.suffix() != "meta") continue;
        auto& entry = entries[file.completeBaseName()];
        entry.key = file.completeBaseName(); entry.bytes += file.size();
        if (file.suffix() == "pcm") {
            if (!entry.committed) entry.used = modified;
            continue;
        }
        // Metadata without the current revision (including the legacy format) is stale.
        QFile metadata(file.filePath());
        entry.committed = true; entry.used = modified;
        entry.current = metadata.open(QIODevice::ReadOnly) && metadata.read(40) == QByteArray(cacheMagic, 8) + revisionId();
    }
    qint64 total = 0;
    std::vector<const Entry*> order;
    for (const auto& [key, entry] : entries) { total += entry.bytes; order.push_back(&entry); }
    if (maxBytes < 0) {
        // Shrinks on nearly full disks; caches still in use are never removed.
        const QStorageInfo storage(directory.path());
        maxBytes = 20ll << 30;
        if (storage.isValid() && storage.bytesAvailable() >= 0) maxBytes = std::min(maxBytes, (storage.bytesAvailable() + total) / 4);
    }
    std::stable_sort(order.begin(), order.end(), [](auto a, auto b) { return a->used < b->used; });
    for (const auto* entry : order) {
        if (total <= maxBytes && (entry->current || now - entry->used <= staleAge)) continue;
        // Preparation and reuse hold the cache lock, so no instance can adopt it while it is removed.
        QLockFile lock(directory.filePath(entry->key + ".lock")); lock.setStaleLockTime(0);
        const auto pcm = directory.filePath(entry->key + ".pcm");
        if (!lock.tryLock(0) || cacheInUse(pcm) || (QFile::exists(pcm) && !QFile::remove(pcm))) continue;
        QFile::remove(directory.filePath(entry->key + ".meta"));
        total -= entry->bytes;
    }
}

std::shared_ptr<const AudioData> loadPcmWave(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto header = file.read(12);
    if (header.size() != 12 || header.first(4) != "RIFF" || header.mid(8, 4) != "WAVE") return {};
    const qint64 end = 8 + static_cast<qint64>(qFromLittleEndian<quint32>(header.constData() + 4));
    if (end < 12 || end > file.size()) return {};
    QByteArray format; qint64 offset = -1, bytes = 0;
    while (file.pos() + 8 <= end) {
        const auto chunk = file.read(8);
        if (chunk.size() != 8) return {};
        const qint64 size = qFromLittleEndian<quint32>(chunk.constData() + 4);
        const auto first = file.pos();
        if (size > end - first) return {};
        if (chunk.first(4) == "fmt ") {
            if (!format.isEmpty() || size < 16) return {};
            format = file.read(std::min(qint64{40}, size));
        } else if (chunk.first(4) == "data") {
            if (offset >= 0) return {};
            offset = first; bytes = size;
        }
        if (!file.seek(first + size + (size & 1))) return {};
    }
    if (format.size() < 16 || offset < 0 || bytes <= 0) return {};
    auto word = [&](int index) { return qFromLittleEndian<quint16>(format.constData() + index); };
    auto dword = [&](int index) { return qFromLittleEndian<quint32>(format.constData() + index); };
    int type = word(0); const int channels = word(2), bits = word(14);
    if (type == 0xfffe) {
        if (format.size() < 40 || word(16) < 22 || word(18) != bits) return {};
        const auto mask = dword(20);
        if (mask && mask != static_cast<quint32>(channels == 1 ? 4 : 3)) return {};
        static const char guidTail[] = {0, 0, 0, 0, 0x10, 0, char(0x80), 0, 0, char(0xaa), 0, 0x38, char(0x9b), 0x71};
        if (std::memcmp(format.constData() + 26, guidTail, 14)) return {};
        type = word(24);
    }
    if ((channels != 1 && channels != 2) || dword(4) != sampleRate ||
        !((type == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (type == 3 && bits == 32))) return {};
    const int stride = channels * (bits / 8);
    if (word(12) != stride || dword(8) != static_cast<quint32>(sampleRate * stride) || bytes % stride) return {};
    auto data = std::make_shared<AudioData>(); data->mono = channels == 1;
    auto storage = std::make_shared<WaveStorage>(path, offset, bytes / stride, channels, bits, type == 3);
    storage->prepare(*data); data->storage = std::move(storage);
    return data;
}
}
