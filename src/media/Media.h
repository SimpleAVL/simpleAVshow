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
// Corrupt packets and transient read/seek failures are skipped and counted; only
// unopenable media or a long run of consecutive failures sets error(). After an
// error, imageAt keeps returning the last good frame.
class VideoReader {
public:
    explicit VideoReader(QString path, QSize maximum = QSize(1920, 1080));
    ~VideoReader();
    QImage imageAt(double time);
    // Looping clips wrap from end back to start; the worker prerolls the frames
    // after start before the wrap. A negative start disables looping. Idempotent.
    void setLoop(double start, double end);
    QString error() const;
    unsigned skipped() const;
private:
    struct FrameImage { double time; QImage image; };
    QString path_, error_;
    QSize maximum_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<FrameImage> cache_, head_;
    QImage shown_;
    double target_ = -1;
    double firstTime_ = 0;
    double loopStart_ = -1, loopEnd_ = 0;
    unsigned skipped_ = 0;
    bool stop_ = false;
    bool eof_ = false, headEof_ = false;
    std::thread worker_;
    void run();
};
}
