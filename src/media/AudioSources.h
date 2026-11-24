#pragma once
#include "core/Model.h"
#include <QByteArray>
#include <QString>
#include <memory>

class QLockFile;
class QTemporaryFile;

namespace sav::detail {
// Import/worker helpers only; none of these functions run in the callback.
QString audioCacheDirectory();
// Least recently used caches are removed until the directory fits the budget;
// automatic is min(20 GiB, a quarter of free space plus the cache). Caches being
// prepared or used by any running instance are kept. Caches from another decoder
// revision or FFmpeg build are removed once unused for 7 days.
constexpr qint64 automaticAudioCacheBudget = -1;
void trimAudioCache(qint64 maxBytes = automaticAudioCacheBudget);
std::shared_ptr<const AudioData> loadPcmWave(const QString& path);
// O(1) path rebind for an identical copied WAV file: reuses verified block hashes and peaks without rescanning disk.
std::shared_ptr<const AudioData> rebindPcmWavePath(const std::shared_ptr<const AudioData>& existing, const QString& newPath);

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
