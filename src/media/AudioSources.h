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
std::shared_ptr<const AudioData> loadPcmWave(const QString& path);

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
