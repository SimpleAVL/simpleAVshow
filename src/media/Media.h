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
    // Missing or unreadable when the show opened; clips keep their place until the source is relinked.
    bool offline = false;
    QString problem;
    double duration = 0, fps = 24;
    int width = 0, height = 0;
    QImage still;
    std::shared_ptr<const AudioData> audio;
};
// How compressed audio (anything but direct 48 kHz PCM WAV) is made playable. Stream imports
// quickly and decodes the original file during playback; Prepared decodes once into a PCM disk
// cache, so playback only reads samples. Both place every sample at the same show frame.
enum class AudioPreparation { Stream, Prepared };
void setAudioPreparation(AudioPreparation preparation) noexcept;
AudioPreparation audioPreparation() noexcept;
// Prepares audio for the current preference, or for one captured when a batch began.
MediaAsset loadMedia(const QString& path);
MediaAsset loadMedia(const QString& path, AudioPreparation preparation);
// Closes streamed-audio decoders unused for a few seconds, wherever their sources are held (an armed
// show, undo history), so files are not kept open indefinitely; a later read reopens and seeks.
void closeIdleStreams();

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
