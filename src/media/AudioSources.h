#pragma once
#include "core/Model.h"
#include <QByteArray>
#include <QString>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class QLockFile;
class QTemporaryFile;

namespace sav::detail {
// Import/worker helpers only; none of these functions run in the callback.
QString audioCacheDirectory();
// The file system's identity of the file at path (device and inode, or volume and file index), which
// changes when a save replaces the file rather than rewriting it; zero if it cannot be read.
#ifdef Q_OS_WIN
// path for Win32 calls: native separators, with the \\?\ (or \\?\UNC\) prefix that lifts the
// 260-character limit when the path needs it.
std::wstring windowsPath(const QString& path);
#endif
// Volume and file number; 0 where unknown. A share may report them only some of the time, and some
// volumes have no stable identifier (their device number changes on every mount).
using FileIdentity = std::pair<quint64, quint64>;
FileIdentity fileIdentity(const QString& path);
// Whether two identities can belong to the same file: unknown parts match anything.
inline bool sameIdentity(const FileIdentity& a, const FileIdentity& b) {
    return (!a.first || !b.first || a.first == b.first) && (!a.second || !b.second || a.second == b.second);
}
// Whether one side knows a part the other does not, so a match says less than usual.
inline bool identityUncertain(const FileIdentity& a, const FileIdentity& b) {
    return !a.first != !b.first || !a.second != !b.second;
}
#ifndef Q_OS_WIN
// The stable identifier of the volume holding an open file (macOS: the file system UUID; 0 if none).
quint64 volumeId(int descriptor);
#endif
// Whether a file is at path. A file deleted while another handle has it open keeps its name until that
// handle closes wherever deletion is not immediate (Windows on FAT/exFAT drives, older Windows, network
// shares): it is not present. Other open failures (a share briefly unreachable) count as present, so
// they are retried by reads instead of being reported as missing media.
bool present(const QString& path);
// Both from one file-system lookup.
struct Probe { bool present = false; FileIdentity identity; };
Probe probe(const QString& path);
// Size, modification time and identity: a file that still has all three is the one that was read.
struct FileVersion {
    qint64 size = -1, modified = 0; FileIdentity identity;
    bool operator==(const FileVersion&) const = default;
    // The same file as other: size and time match, and identity too wherever both sides know it
    // (a share may report it only some of the time).
    bool sameAs(const FileVersion& other) const {
        return size == other.size && modified == other.modified && sameIdentity(identity, other.identity);
    }
};
FileVersion fileVersion(const QString& path);
// Least recently used caches are removed until the directory fits the budget;
// automatic is min(20 GiB, a quarter of free space plus the cache). Caches being
// prepared or used by any running instance are kept. Caches from another decoder
// revision or FFmpeg build are removed once unused for 7 days. Waveform sidecars of
// streamed and direct sources only save work, so they are trimmed like unused caches.
constexpr qint64 automaticAudioCacheBudget = -1;
void trimAudioCache(qint64 maxBytes = automaticAudioCacheBudget);
// Bytes in the audio cache directory: prepared caches plus waveform sidecars.
qint64 audioCacheBytes();
std::shared_ptr<const AudioData> loadPcmWave(const QString& path);

// The decoded length and waveform of a streamed source. Keyed by content and decoder
// revision like the prepared cache, so reopening or moving the file skips the decode pass.
// An input sample position by packet timestamp, recorded about once a second while decoding a
// stream whose timestamps are coarser than one sample (Matroska/WebM's milliseconds), where a
// timestamp alone cannot say which sample a seek landed on.
// pos: the packet's byte offset (-1 if the container has none). Byte-seeked containers can compute a
// timestamp after a seek that a linear decode gave another packet (G.722 in WAV), so a landing must
// match the offset too.
struct SeekPoint { std::int64_t pts = 0, sample = 0, pos = -1; };
struct AudioSummary { Frame length = 0; bool mono = false; std::vector<float> peaks; std::vector<SeekPoint> seekPoints; };
class StreamSummary {
public:
    // Hashes the source; construct it before opening the source to decode, so a replacement in
    // between is caught by save rather than stored under the replacement's key.
    // format: the demuxer chosen for the file. Raw formats are chosen by extension, so identical
    // bytes can decode differently, and the format is part of the key.
    StreamSummary(const QString& source, const QByteArray& format);
    // The source as hashed; streamed reads compare against it.
    const FileVersion& version() const noexcept { return version_; }
    std::optional<AudioSummary> reuse(bool mono) const;
    // Throws if the source changed since construction; check before accepting a reused summary.
    void verify() const;
    // Verifies, then saves (best effort).
    void save(const AudioSummary& summary) const;
private:
    QString source_, path_;
    QByteArray stamp_, sourceHash_;
    bool recent_ = false;
    void contentUnchanged() const;
    FileVersion version_;
};

class AudioCache {
public:
    // Hashes the source; like StreamSummary, construct it before opening the source to decode.
    AudioCache(const QString& source, const QByteArray& format); // format as for StreamSummary.
    ~AudioCache();
    std::shared_ptr<const AudioData> reuse(bool mono) const;
    // Throws if the source changed since construction; check before accepting a reused cache.
    void verify() const;
    QString temporaryTemplate() const;
    const FileVersion& version() const noexcept { return version_; } // As StreamSummary::version.
    std::shared_ptr<const AudioStorage> publish(QTemporaryFile& file, const AudioData& data, Frame length) const;
private:
    QString source_, pcm_, metadata_;
    QByteArray stamp_, sourceHash_; // The stamp is taken before hashing.
    FileVersion version_;
    std::unique_ptr<QLockFile> lock_;
};
}
