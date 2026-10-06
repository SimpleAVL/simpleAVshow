#pragma once
#include "core/Model.h"
#include <QImage>
#include <QString>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace sav {
struct MediaAsset {
    QString id, path, name;
    bool image = false, video = false;
    double duration = 0, fps = 24;
    int width = 0, height = 0;
    QImage still;
    std::shared_ptr<const AudioData> audio;
};
MediaAsset loadMedia(const QString& path);

// One worker per video clip. No decoding or disk access on the audio thread.
class VideoReader {
public:
    explicit VideoReader(QString path, QSize maximum = QSize(1920, 1080));
    ~VideoReader();
    QImage imageAt(double time);
    QString error() const;
private:
    struct FrameImage { double time; QImage image; };
    QString path_, error_;
    QSize maximum_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<FrameImage> cache_;
    double target_ = -1;
    double firstTime_ = 0;
    bool stop_ = false;
    bool eof_ = false;
    std::thread worker_;
    void run();
};
}
