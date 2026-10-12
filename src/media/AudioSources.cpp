#include "media/AudioSources.h"
#include "core/AudioBuffer.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryFile>
#include <QtEndian>
#include <cstring>
#include <stdexcept>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

namespace sav::detail {
namespace {
constexpr qint64 stereoBytes = 2 * sizeof(float);
constexpr qint64 metadataHeader = 8 + 8 + 1 + 32 + 32;
constexpr char cacheMagic[] = "SAVPCM01";
[[noreturn]] void fail(const QString& message) { throw std::runtime_error(message.toStdString()); }

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
QByteArray peaksHash(const std::vector<float>& peaks) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView(reinterpret_cast<const char*>(peaks.data()), static_cast<qsizetype>(peaks.size() * sizeof(float))));
    return hash.result();
}
void checkRange(Frame first, std::size_t count, Frame length) {
    if (first < 0 || first > length || count > static_cast<std::size_t>(length - first)) fail("Invalid audio source range");
}

class PcmStorage final : public AudioStorage {
public:
    PcmStorage(QString path, Frame length) : path_(std::move(path)), length_(length), modified_(QFileInfo(path_).lastModified()) {}
    Frame length() const noexcept override { return length_; }
    void read(Frame first, float* stereo, std::size_t count) const override {
        checkRange(first, count, length_);
        const QFileInfo info(path_);
        QFile file(path_);
        const auto bytes = static_cast<qint64>(count) * stereoBytes;
        if (info.size() != length_ * stereoBytes || info.lastModified() != modified_ ||
            !file.open(QIODevice::ReadOnly) || !file.seek(first * stereoBytes) ||
            file.read(reinterpret_cast<char*>(stereo), bytes) != bytes)
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
          size_(QFileInfo(path_).size()), modified_(QFileInfo(path_).lastModified()) {}
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
                fail("PCM audio source changed; reopen the project: " + path_);
        }
        decode(reinterpret_cast<const uchar*>(bytes.constData()) + (first - blockFirst) * stride, stereo, count);
    }
private:
    QByteArray readBytes(Frame first, std::size_t count) const {
        const QFileInfo info(path_);
        const qint64 stride = channels_ * (bits_ / 8);
        QFile file(path_);
        if (info.size() != size_ || info.lastModified() != modified_ ||
            !file.open(QIODevice::ReadOnly) || !file.seek(offset_ + first * stride))
            fail("PCM audio source changed or is unavailable: " + path_);
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
    QDateTime modified_;
    std::vector<QByteArray> blockHashes_;
};
}

QString audioCacheDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
        (QSysInfo::ByteOrder == QSysInfo::LittleEndian ? "/audio-pcm-v1-le" : "/audio-pcm-v1-be");
}

AudioCache::AudioCache(const QString& source) : source_(source), sourceHash_(fileHash(source)) {
    // Content identity also survives moved media and fresh package extraction paths.
    const auto revision = QByteArray("SAVPCM01:") + QByteArray::number(avformat_version()) + ':' +
        QByteArray::number(avcodec_version()) + ':' + QByteArray::number(swresample_version());
    const auto key = QString::fromLatin1(QCryptographicHash::hash(revision + sourceHash_, QCryptographicHash::Sha256).toHex());
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
    char magic[8], pcmHash[32], waveformHash[32]; qint64 length = 0; quint8 savedMono = 0;
    if (stream.readRawData(magic, 8) != 8 || std::memcmp(magic, cacheMagic, 8)) return {};
    stream >> length >> savedMono;
    if (length <= 0 || length > maxFrame || savedMono > 1 || static_cast<bool>(savedMono) != mono ||
        QFileInfo(pcm_).size() != length * stereoBytes) return {};
    const auto peakCount = (length + AudioData::peakStride - 1) / AudioData::peakStride;
    const auto peakBytes = peakCount * static_cast<qint64>(sizeof(float));
    if (metadata.size() != metadataHeader + peakBytes || stream.readRawData(pcmHash, 32) != 32 || stream.readRawData(waveformHash, 32) != 32) return {};
    auto data = std::make_shared<AudioData>(); data->mono = mono;
    data->peaks.resize(static_cast<std::size_t>(peakCount));
    if (metadata.read(reinterpret_cast<char*>(data->peaks.data()), peakBytes) != peakBytes ||
        peaksHash(data->peaks) != QByteArray(waveformHash, 32) ||
        !std::all_of(data->peaks.begin(), data->peaks.end(), [](float value) { return std::isfinite(value) && value >= 0; })) return {};
    try {
        if (fileHash(pcm_) != QByteArray(pcmHash, 32)) return {};
    } catch (const std::runtime_error&) { return {}; }
    data->storage = std::make_shared<PcmStorage>(pcm_, length);
    return data;
}

std::shared_ptr<const AudioStorage> AudioCache::publish(QTemporaryFile& file, const AudioData& data, Frame length) const {
    if (length <= 0 || length > maxFrame || file.size() != length * stereoBytes ||
        data.peaks.size() != static_cast<std::size_t>((length + AudioData::peakStride - 1) / AudioData::peakStride)) fail("Invalid prepared audio length");
    if (!file.flush()) fail("Cannot finish audio cache: " + file.errorString());
    if (fileHash(source_) != sourceHash_) fail("Audio source changed during preparation; import it again");
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
    QSaveFile metadata(metadata_);
    if (!metadata.open(QIODevice::WriteOnly)) fail("Cannot save audio cache metadata: " + metadata.errorString());
    QDataStream stream(&metadata); stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData(cacheMagic, 8); stream << static_cast<qint64>(length) << static_cast<quint8>(data.mono);
    stream.writeRawData(checksum.constData(), 32); stream.writeRawData(peaksHash(data.peaks).constData(), 32);
    const auto peakBytes = static_cast<qint64>(data.peaks.size() * sizeof(float));
    if (metadata.write(reinterpret_cast<const char*>(data.peaks.data()), peakBytes) != peakBytes ||
        stream.status() != QDataStream::Ok || !metadata.commit()) fail("Cannot finish audio cache metadata: " + metadata.errorString());
    return std::make_shared<PcmStorage>(pcm_, length);
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
