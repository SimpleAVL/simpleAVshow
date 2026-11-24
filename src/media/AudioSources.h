#pragma once
#include "core/Model.h"
#include <QByteArray>
#include <QString>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

class QLockFile;
class QTemporaryFile;

namespace sav::detail {
// Import/worker helpers only; none of these functions run in the callback.
QString audioCacheDirectory();
// The file system's identity of the file at path (device and inode, or volume and file index), which
// changes when a save replaces the file rather than rewriting it; zero if it cannot be read.
using FileIdentity = std::pair<quint64, quint64>;
FileIdentity fileIdentity(const QString& path);
// Size, modification time and identity: a file that still has all three is the one that was read.
struct FileVersion {
    qint64 size = -1, modified = 0; FileIdentity identity;
    bool operator==(const FileVersion&) const = default;
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
struct SeekPoint { std::int64_t pts = 0, sample = 0; };
struct AudioSummary { Frame length = 0; bool mono = false; std::vector<float> peaks; std::vector<SeekPoint> seekPoints; };
class StreamSummary {
public:
    // Hashes the source; construct it before opening the source to decode, so a replacement in
    // between is caught by save rather than stored under the replacement's key.
    explicit StreamSummary(const QString& source);
    // The source as hashed; streamed reads compare against it.
    const FileVersion& version() const noexcept { return version_; }
    std::optional<AudioSummary> reuse(bool mono) const;
    // Throws if the source changed while it was decoded; saving itself is best effort.
    void save(const AudioSummary& summary) const;
private:
    QString source_, path_;
    QByteArray stamp_;
    FileVersion version_;
};

class AudioCache {
public:
    explicit AudioCache(const QString& source);
    ~AudioCache();
    std::shared_ptr<const AudioData> reuse(bool mono) const;
    QString temporaryTemplate() const;
    std::shared_ptr<const AudioStorage> publish(QTemporaryFile& file, const AudioData& data, Frame length) const;
private:
    QString source_, pcm_, metadata_;
    QByteArray sourceHash_;
    std::unique_ptr<QLockFile> lock_;
};
}
