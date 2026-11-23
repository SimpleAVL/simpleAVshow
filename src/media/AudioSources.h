#pragma once
#include "core/Model.h"
#include <QByteArray>
#include <QString>
#include <memory>
#include <optional>
#include <vector>

class QLockFile;
class QTemporaryFile;

namespace sav::detail {
// Import/worker helpers only; none of these functions run in the callback.
QString audioCacheDirectory();
// Least recently used caches are removed until the directory fits the budget;
// automatic is min(20 GiB, a quarter of free space plus the cache). Caches being
// prepared or used by any running instance are kept. Caches from another decoder
// revision or FFmpeg build are removed once unused for 7 days. Waveform sidecars of
// streamed and direct sources only save work, so they are trimmed like unused caches.
constexpr qint64 automaticAudioCacheBudget = -1;
void trimAudioCache(qint64 maxBytes = automaticAudioCacheBudget);
std::shared_ptr<const AudioData> loadPcmWave(const QString& path);

// The decoded length and waveform of a streamed source. Keyed by content and decoder
// revision like the prepared cache, so reopening or moving the file skips the decode pass.
struct AudioSummary { Frame length = 0; bool mono = false; std::vector<float> peaks; };
class StreamSummary {
public:
    explicit StreamSummary(const QString& source);
    std::optional<AudioSummary> reuse(bool mono) const;
    // Throws if the source changed while it was decoded; saving itself is best effort.
    void save(const AudioSummary& summary) const;
private:
    QString source_, path_;
    QByteArray stamp_;
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
