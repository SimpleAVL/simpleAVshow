#include "media/Media.h"
#include "media/AudioSources.h"
#include <QFileInfo>
#include <QUuid>
#include <QImageReader>
#include <QTemporaryFile>
#include <atomic>
#include <limits>
#include <numeric>
#include <stdexcept>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace sav {
namespace {
std::string avError(int code) { char buffer[AV_ERROR_MAX_STRING_SIZE]; av_strerror(code, buffer, sizeof(buffer)); return buffer; }
void checked(int result, const char* action) { if (result < 0) throw std::runtime_error(std::string(action) + ": " + avError(result)); }
double mediaOrigin(const AVFormatContext* format) {
    if (format->start_time != AV_NOPTS_VALUE) return format->start_time / static_cast<double>(AV_TIME_BASE);
    double origin = std::numeric_limits<double>::infinity();
    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const auto* s = format->streams[i];
        if (s->start_time != AV_NOPTS_VALUE && (s->codecpar->codec_type == AVMEDIA_TYPE_AUDIO || s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO))
            origin = std::min(origin, s->start_time * av_q2d(s->time_base));
    }
    return std::isfinite(origin) ? origin : 0;
}
struct Input {
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    SwrContext* resampler = nullptr;
    SwsContext* scaler = nullptr;
    ~Input() {
        swr_free(&resampler); sws_freeContext(scaler);
        av_packet_free(&packet); av_frame_free(&frame);
        avcodec_free_context(&codec); avformat_close_input(&format);
    }
    void open(const QString& path) {
        const QByteArray encoded = path.toUtf8();
        checked(avformat_open_input(&format, encoded.constData(), nullptr, nullptr), "Open media");
        checked(avformat_find_stream_info(format, nullptr), "Read media information");
    }
    void decoder(int stream) {
        const auto* decoder = avcodec_find_decoder(format->streams[stream]->codecpar->codec_id);
        if (!decoder) throw std::runtime_error("Unsupported codec");
        codec = avcodec_alloc_context3(decoder);
        if (!codec) throw std::bad_alloc();
        checked(avcodec_parameters_to_context(codec, format->streams[stream]->codecpar), "Read codec parameters");
        codec->thread_count = 2;
        checked(avcodec_open2(codec, decoder, nullptr), "Open decoder");
    }
};
// Decodes a file's best audio stream to interleaved 48 kHz stereo float at show frames. The first
// decoded frame's timestamp places the source (a positive container offset becomes leading
// silence, a negative one is trimmed); samples then follow contiguously. Prepared caches and
// streamed sources both decode through this, so they place every sample identically.
class AudioDecoder {
public:
    // Seeks start this far early: codecs converge (MP3 bit reservoir, AAC/Opus overlap) and the
    // resampler fills its filter, so seeked output matches a decode from the start.
    static constexpr Frame preroll = sampleRate / 2;
    explicit AudioDecoder(QString path) : path_(std::move(path)) { open(); }
    bool mono() const noexcept { return mono_; }
    // The next decoded run, frames [first, first + samples.size() / 2). False once the source ends.
    bool next(Frame& first, std::vector<float>& samples) {
        samples.clear();
        while (!finished_) {
            if (receive()) {
                convert(input_->frame, first, samples); av_frame_unref(input_->frame);
            } else if (convert(nullptr, first, samples) <= 0) finished_ = true; // Drains the resampler's delay line.
            if (!samples.empty()) return true;
        }
        return false;
    }
    // Continue output from at or before frame, so the next runs cover it.
    void seek(Frame frame) {
        // A linear decode's first frames anchor timestamps to sample positions.
        if (!anchorTried_) { Frame first; std::vector<float> samples; while (!anchorTried_ && next(first, samples)) {} }
        const Frame output = std::max<Frame>(0, frame - start_ - preroll);
        // Resume on a resampling phase boundary, where output and input samples coincide exactly.
        target_ = output / stepOut_ * stepIn_; retries_ = 0;
        if (target_ == 0 || !anchored_) restart(); else seekInput(target_);
    }
private:
    QString path_;
    std::unique_ptr<Input> input_;
    int stream_ = -1, rate_ = 0, retries_ = 0;
    bool mono_ = false, started_ = false, aligned_ = false, finished_ = false, anchorTried_ = false, anchored_ = false;
    double origin_ = 0;
    std::int64_t firstPts_ = AV_NOPTS_VALUE; // Of the first decoded frame, in stream time base.
    // Counted input position minus the one a timestamp implies. Decoders trim encoder delay
    // (MP3, AAC, Opus priming) from the first frame without moving its timestamp, so later
    // timestamps are offset by the trimmed samples. Measured on the second frame of a linear decode.
    std::int64_t ptsOffset_ = 0;
    Frame start_ = 0;                        // Show frame of output sample 0.
    std::int64_t stepIn_ = 1, stepOut_ = 1;  // Input and output samples per resampling phase cycle.
    std::int64_t inputAt_ = 0, target_ = 0, produced_ = 0;
    std::vector<float> scratch_;
    void open() {
        input_ = std::make_unique<Input>(); input_->open(path_);
        stream_ = av_find_best_stream(input_->format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (stream_ < 0) throw std::runtime_error("File contains no audio");
        input_->decoder(stream_);
        if (input_->codec->ch_layout.nb_channels > 2)
            throw std::runtime_error("Import mono or stereo stems separately; multichannel source files are not supported yet");
        mono_ = input_->codec->ch_layout.nb_channels == 1; rate_ = input_->codec->sample_rate;
        if (rate_ <= 0) throw std::runtime_error("Audio has no sample rate");
        const auto common = std::gcd(rate_, sampleRate); stepIn_ = rate_ / common; stepOut_ = sampleRate / common;
        origin_ = mediaOrigin(input_->format);
        // Any one-channel layout (including front-left) is centre mono; the transport pans it.
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO, mono = AV_CHANNEL_LAYOUT_MONO;
        checked(swr_alloc_set_opts2(&input_->resampler, &stereo, AV_SAMPLE_FMT_FLT, sampleRate,
            mono_ ? &mono : &input_->codec->ch_layout, input_->codec->sample_fmt, rate_, 0, nullptr), "Configure resampling");
        checked(swr_init(input_->resampler), "Initialize resampling");
        // Video and other streams are never decoded here; skip their packets while demuxing.
        for (unsigned i = 0; i < input_->format->nb_streams; ++i) if (static_cast<int>(i) != stream_) input_->format->streams[i]->discard = AVDISCARD_ALL;
        started_ = aligned_ = finished_ = false;
    }
    // The next decoded frame into input_->frame; false once the decoder is drained.
    bool receive() {
        for (;;) {
            const int received = avcodec_receive_frame(input_->codec, input_->frame);
            if (received == 0) return true;
            if (received == AVERROR_EOF) return false;
            if (received != AVERROR(EAGAIN)) checked(received, "Decode audio");
            const int read = av_read_frame(input_->format, input_->packet);
            if (read == AVERROR_EOF) { checked(avcodec_send_packet(input_->codec, nullptr), "Flush audio decoder"); continue; }
            checked(read, "Read audio packets");
            const int sent = input_->packet->stream_index == stream_ ? avcodec_send_packet(input_->codec, input_->packet) : 0;
            av_packet_unref(input_->packet);
            checked(sent, "Feed audio decoder");
        }
    }
    void begin(std::int64_t pts) {
        started_ = true; firstPts_ = pts; inputAt_ = 0; start_ = 0;
        if (pts == AV_NOPTS_VALUE) return;
        const double startTime = pts * av_q2d(input_->format->streams[stream_]->time_base) - origin_;
        if (!std::isfinite(startTime) || std::abs(startTime) > maxSeconds) throw std::runtime_error("Invalid audio stream start time");
        start_ = frames(startTime);
    }
    // Decoding again from the start always matches; also the fallback when seeking cannot be trusted.
    void restart() { open(); }
    void seekInput(std::int64_t sample) {
        const auto base = input_->format->streams[stream_]->time_base;
        checked(av_seek_frame(input_->format, stream_, firstPts_ + av_rescale_q(sample - ptsOffset_, {1, rate_}, base), AVSEEK_FLAG_BACKWARD), "Seek audio");
        avcodec_flush_buffers(input_->codec); swr_close(input_->resampler);
        checked(swr_init(input_->resampler), "Initialize resampling");
        aligned_ = finished_ = false;
    }
    // Returns the resampler's output count (0 once a flush is drained); samples get the part at or after frame 0.
    int convert(AVFrame* frame, Frame& first, std::vector<float>& samples) {
        const std::uint8_t* planes[2]{};
        int count = 0;
        if (frame) {
            const auto base = input_->format->streams[stream_]->time_base;
            const auto pts = frame->best_effort_timestamp;
            const auto implied = [&] { return av_rescale_q_rnd(pts - firstPts_, base, {1, rate_}, AV_ROUND_NEAR_INF); };
            if (!started_) begin(pts);
            else if (aligned_ && !anchorTried_) {
                // Without timestamps there is nothing to seek by; seeks then decode from the start.
                anchorTried_ = true;
                if (pts != AV_NOPTS_VALUE && firstPts_ != AV_NOPTS_VALUE) { ptsOffset_ = inputAt_ - implied(); anchored_ = true; }
            }
            else if (!aligned_) {
                // The first frame after a seek; its timestamp says where the decoder landed.
                if (pts == AV_NOPTS_VALUE) { restart(); return 0; }
                inputAt_ = implied() + ptsOffset_;
                if (inputAt_ > target_) {
                    // Landed late (an inexact index): back off further, then decode from the start.
                    const auto earlier = target_ - 2 * (inputAt_ - target_) - rate_;
                    if (++retries_ <= 3 && earlier > 0) seekInput(earlier); else restart();
                    return 0;
                }
            }
            if (!aligned_) { aligned_ = true; produced_ = target_ / stepIn_ * stepOut_; }
            const auto skip = static_cast<int>(std::clamp<std::int64_t>(target_ - inputAt_, 0, frame->nb_samples));
            inputAt_ += frame->nb_samples; count = frame->nb_samples - skip;
            if (!count) return 0;
            // Input before the resume point is dropped so the resampler starts on its phase boundary.
            const int bytes = av_get_bytes_per_sample(static_cast<AVSampleFormat>(frame->format));
            const bool planar = av_sample_fmt_is_planar(static_cast<AVSampleFormat>(frame->format));
            const int channels = frame->ch_layout.nb_channels;
            for (int c = 0; c < (planar ? channels : 1); ++c) planes[c] = frame->extended_data[c] + static_cast<std::size_t>(skip) * bytes * (planar ? 1 : channels);
        } else if (!aligned_) return 0;
        const int capacity = static_cast<int>(av_rescale_rnd(swr_get_delay(input_->resampler, rate_) + count, sampleRate, rate_, AV_ROUND_UP));
        if (capacity <= 0) return 0;
        scratch_.resize(static_cast<std::size_t>(capacity) * 2);
        auto* dest = reinterpret_cast<std::uint8_t*>(scratch_.data());
        const int written = swr_convert(input_->resampler, &dest, capacity, frame ? planes : nullptr, count);
        checked(written, "Resample audio");
        // FFmpeg's default mono -> stereo downmix applies -3 dB. Undo it;
        // the transport applies constant-power mono pan exactly once.
        if (mono_) for (int i = 0; i < written * 2; ++i) scratch_[i] *= 1.41421356237f;
        const Frame at = start_ + produced_;
        if (written > maxFrame - at) throw std::runtime_error("Audio duration exceeds representable frame timing");
        produced_ += written;
        const auto skip = static_cast<std::size_t>(std::clamp<Frame>(-at, 0, written));
        first = at + static_cast<Frame>(skip);
        samples.assign(scratch_.begin() + static_cast<std::ptrdiff_t>(skip * 2), scratch_.begin() + written * 2);
        return written;
    }
};

// Reads decode the original file. The transport's read-ahead worker calls read, never the
// audio callback. A few decoders stay open so clips reading the same file at different
// places continue sequentially instead of seeking for every block.
class StreamStorage final : public AudioStorage {
public:
    StreamStorage(QString path, Frame length) : path_(std::move(path)), length_(length) {
        const QFileInfo info(path_); size_ = info.size(); modified_ = info.lastModified();
    }
    Frame length() const noexcept override { return length_; }
    bool decodes() const noexcept override { return true; }
    void read(Frame first, float* stereo, std::size_t count) const override {
        if (first < 0 || first > length_ || count > static_cast<std::size_t>(length_ - first)) throw std::runtime_error("Invalid audio source range");
        std::fill(stereo, stereo + count * 2, 0.f);
        if (!count) return;
        const QFileInfo info(path_);
        if (!info.exists()) throw std::runtime_error("Audio source is unavailable: " + path_.toStdString());
        if (info.size() != size_ || info.lastModified() != modified_) throw AudioSourceChanged("Audio source changed; reopen the project: " + path_.toStdString());
        const std::lock_guard lock(mutex_);
        Reader* reader = nullptr;
        try {
            pick(first, reader);
            const Frame end = first + static_cast<Frame>(count);
            for (Frame at = first; at < end;) {
                const Frame have = reader->first + static_cast<Frame>(reader->samples.size() / 2);
                if (have <= at) {
                    if (!reader->decoder.next(reader->first, reader->samples)) break;
                    continue;
                }
                if (reader->first > at) { at = std::min(end, reader->first); continue; } // Leading silence stays zero.
                const Frame stop = std::min(end, have);
                std::copy(reader->samples.begin() + (at - reader->first) * 2, reader->samples.begin() + (stop - reader->first) * 2, stereo + (at - first) * 2);
                at = stop;
            }
            reader->used = ++clock_;
        } catch (...) {
            // A failed decoder's position is unknown; the next read opens a fresh one.
            if (reader) std::erase_if(readers_, [reader](const auto& r) { return r.get() == reader; });
            throw;
        }
    }
private:
    static constexpr std::size_t maxReaders = 4;
    // Reading forward this far decodes through instead of seeking.
    static constexpr Frame continueWithin = 2 * sampleRate;
    struct Reader {
        explicit Reader(const QString& path) : decoder(path) {}
        AudioDecoder decoder;
        Frame first = 0;
        std::vector<float> samples;
        std::uint64_t used = 0;
    };
    QString path_;
    Frame length_;
    qint64 size_ = 0;
    QDateTime modified_;
    mutable std::mutex mutex_;
    mutable std::vector<std::unique_ptr<Reader>> readers_;
    mutable std::uint64_t clock_ = 0;
    // Sets reader before seeking it, so a failed seek discards that decoder.
    void pick(Frame frame, Reader*& reader) const {
        for (const auto& r : readers_) {
            const Frame have = r->first + static_cast<Frame>(r->samples.size() / 2);
            if (r->first <= frame && frame < have + continueWithin && (!reader || r->first > reader->first)) reader = r.get();
        }
        if (reader) return;
        if (readers_.size() < maxReaders) {
            readers_.push_back(std::make_unique<Reader>(path_)); reader = readers_.back().get();
            if (frame < continueWithin) return; // A fresh decoder starts at the beginning.
        } else reader = std::min_element(readers_.begin(), readers_.end(), [](const auto& a, const auto& b) { return a->used < b->used; })->get();
        reader->samples.clear(); reader->first = 0; reader->decoder.seek(frame);
    }
};

std::atomic<AudioPreparation> preparation{AudioPreparation::Stream};
void addPeaks(std::vector<float>& peaks, Frame first, const std::vector<float>& samples) {
    const auto count = static_cast<Frame>(samples.size() / 2);
    peaks.resize(static_cast<std::size_t>((first + count + AudioData::peakStride - 1) / AudioData::peakStride), 0.f);
    for (Frame i = 0; i < count; ++i) {
        auto& peak = peaks[static_cast<std::size_t>((first + i) / AudioData::peakStride)];
        peak = std::max(peak, std::max(std::abs(samples[i * 2]), std::abs(samples[i * 2 + 1])));
    }
}
std::shared_ptr<const AudioData> loadAudio(const QString& path) {
    AudioDecoder decoder(path);
    auto data = std::make_shared<AudioData>(); data->mono = decoder.mono();
    Frame first = 0; std::vector<float> samples;
    if (preparation.load() == AudioPreparation::Stream) {
        // One decode pass finds the exact length and waveform; playback decodes again on demand.
        detail::StreamSummary summary(path);
        auto known = summary.reuse(data->mono);
        if (!known) {
            known = detail::AudioSummary{0, data->mono, {}};
            while (decoder.next(first, samples)) { addPeaks(known->peaks, first, samples); known->length = first + static_cast<Frame>(samples.size() / 2); }
            if (known->length <= 0) throw std::runtime_error("Audio stream is empty");
            summary.save(*known);
        }
        data->peaks = std::move(known->peaks);
        data->storage = std::make_shared<StreamStorage>(path, known->length);
        return data;
    }
    detail::AudioCache prepared(path);
    if (auto reused = prepared.reuse(data->mono)) return reused;
    QTemporaryFile cache(prepared.temporaryTemplate());
    if (!cache.open()) throw std::runtime_error("Cannot create audio cache: " + cache.errorString().toStdString());
    Frame total = 0;
    while (decoder.next(first, samples)) {
        if (first > total) {
            // A sparse zero-filled prefix preserves embedded A/V offsets
            // without allocating leading silence in RAM.
            total = first;
            if (!cache.resize(total * 2 * static_cast<qint64>(sizeof(float))) || !cache.seek(total * 2 * static_cast<qint64>(sizeof(float))))
                throw std::runtime_error("Cannot prepare audio start offset: " + cache.errorString().toStdString());
        }
        const qint64 bytes = static_cast<qint64>(samples.size() * sizeof(float));
        if (cache.write(reinterpret_cast<const char*>(samples.data()), bytes) != bytes)
            throw std::runtime_error("Cannot write audio cache (check available disk space): " + cache.errorString().toStdString());
        addPeaks(data->peaks, first, samples);
        total += static_cast<Frame>(samples.size() / 2);
    }
    data->peaks.resize(static_cast<std::size_t>((total + AudioData::peakStride - 1) / AudioData::peakStride), 0.f);
    data->storage = prepared.publish(cache, *data, total);
    return data;
}
}

void setAudioPreparation(AudioPreparation value) noexcept { preparation.store(value); }
AudioPreparation audioPreparation() noexcept { return preparation.load(); }

MediaAsset loadMedia(const QString& path) {
    if (!QFileInfo(path).isFile()) throw std::runtime_error("Media file is missing: " + path.toStdString());
    MediaAsset asset;
    asset.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    asset.path = QFileInfo(path).absoluteFilePath(); asset.name = QFileInfo(path).fileName();
    QImageReader reader(path);
    if (reader.canRead() && !reader.supportsAnimation()) {
        // Avoid loading implausibly large images before allocating a project-sized render buffer.
        const QSize dimensions = reader.size();
        if (static_cast<qint64>(dimensions.width()) * dimensions.height() > 64000000)
            throw std::runtime_error("Image is larger than 64 megapixels");
        asset.still = reader.read();
        if (asset.still.isNull()) throw std::runtime_error(reader.errorString().toStdString());
        asset.image = true; asset.width = asset.still.width(); asset.height = asset.still.height(); asset.duration = 10;
        return asset;
    }
    if (auto pcm = detail::loadPcmWave(asset.path)) {
        asset.audio = std::move(pcm); asset.duration = seconds(asset.audio->length());
        return asset;
    }
    Input input; input.open(path);
    if (input.format->duration != AV_NOPTS_VALUE) asset.duration = input.format->duration / static_cast<double>(AV_TIME_BASE);
    const int video = av_find_best_stream(input.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video >= 0) {
        auto* stream = input.format->streams[video];
        asset.video = true; asset.width = stream->codecpar->width; asset.height = stream->codecpar->height;
        if (static_cast<qint64>(asset.width) * asset.height > 64000000) throw std::runtime_error("Video is larger than 64 megapixels");
        const auto rate = av_guess_frame_rate(input.format, stream, nullptr);
        if (rate.num && rate.den) asset.fps = av_q2d(rate);
    }
    if (av_find_best_stream(input.format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) >= 0) {
        if (asset.video && (!std::isfinite(asset.duration) || asset.duration <= 0 || asset.duration > maxSeconds))
            throw std::runtime_error("Media needs a finite, representable duration");
        asset.audio = loadAudio(asset.path);
        if (!asset.video) asset.duration = seconds(asset.audio->length());
    }
    if (!asset.video && !asset.audio) throw std::runtime_error("File contains no supported audio, video, or still image");
    if (!std::isfinite(asset.duration) || asset.duration <= 0 || asset.duration > maxSeconds) throw std::runtime_error("Media needs a finite, representable duration");
    return asset;
}

VideoReader::VideoReader(QString path, QSize maximum) : path_(std::move(path)), maximum_(maximum), worker_([this] { run(); }) {}
VideoReader::~VideoReader() {
    { std::lock_guard lock(mutex_); stop_ = true; }
    wake_.notify_one(); worker_.join();
}
QString VideoReader::error() const { std::lock_guard lock(mutex_); return error_; }
unsigned VideoReader::skipped() const { std::lock_guard lock(mutex_); return skipped_; }
void VideoReader::setLoop(double start, double end) {
    std::lock_guard lock(mutex_);
    if (start < 0) start = -1;
    if (start == loopStart_ && end == loopEnd_) return;
    loopStart_ = start; loopEnd_ = end; head_.clear(); headEof_ = false;
}
QImage VideoReader::imageAt(double time) {
    std::lock_guard lock(mutex_);
    target_ = std::max(0., time); wake_.notify_one();
    if (target_ < firstTime_) {
        QImage blank(1, 1, QImage::Format_RGBA8888); blank.fill(Qt::transparent); return blank;
    }
    auto pick = [&](const std::deque<FrameImage>& frames, bool complete) {
        QImage result;
        if (frames.empty() || (!complete && frames.back().time < target_)) return result;
        for (const auto& frame : frames) {
            if (frame.time <= target_ + .0001) result = frame.image;
            else break;
        }
        return result;
    };
    QImage result = pick(cache_, eof_);
    // The prerolled loop head serves the wrap while the worker moves it into the cache.
    if (result.isNull()) result = pick(head_, headEof_);
    if (result.isNull() && !error_.isEmpty()) result = shown_.isNull() && !cache_.empty() ? cache_.back().image : shown_;
    if (!result.isNull()) shown_ = result;
    return result;
}
void VideoReader::run() {
    struct Stopped {};
    try {
        Input input; input.open(path_);
        const int index = av_find_best_stream(input.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        checked(index, "Find video stream"); input.decoder(index);
        auto* stream = input.format->streams[index];
        const double start = mediaOrigin(input.format);
        {
            std::lock_guard lock(mutex_);
            if (stream->start_time != AV_NOPTS_VALUE) firstTime_ = stream->start_time * av_q2d(stream->time_base) - start;
        }
        const AVRational frameRate = av_guess_frame_rate(input.format, stream, nullptr);
        const double rate = frameRate.num && frameRate.den ? av_q2d(frameRate) : 24;
        const double lookahead = std::min(.25, 12. / std::max(1., rate));
        unsigned failures = 0;
        // Corrupt packets and transient read/seek errors are skipped; only a long run of them is fatal.
        auto failed = [&](int code, const char* action, bool backoff) {
            std::unique_lock lock(mutex_); ++skipped_;
            if (++failures > 50) throw std::runtime_error(std::string(action) + ": " + avError(code));
            if (backoff) wake_.wait_for(lock, std::chrono::milliseconds(10), [&] { return stop_; });
            if (stop_) throw Stopped{};
        };
        auto seekTo = [&](double time) {
            const auto timestamp = static_cast<std::int64_t>((time + start) / av_q2d(stream->time_base));
            int result = av_seek_frame(input.format, index, timestamp, AVSEEK_FLAG_BACKWARD);
            if (result < 0) result = av_seek_frame(input.format, index, timestamp, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
            // Last resort: decode forward from the beginning.
            if (result < 0) result = av_seek_frame(input.format, index, stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
            if (result < 0) failed(result, "Seek video", false);
            avcodec_flush_buffers(input.codec);
        };
        // Decodes and converts the next picture; false at end of stream.
        auto next = [&](double previous, FrameImage& out) {
            for (;;) {
                { std::lock_guard lock(mutex_); if (stop_) throw Stopped{}; }
                int result = avcodec_receive_frame(input.codec, input.frame);
                if (result == AVERROR_EOF) return false;
                if (result == AVERROR(EAGAIN)) {
                    while ((result = av_read_frame(input.format, input.packet)) >= 0 && input.packet->stream_index != index) av_packet_unref(input.packet);
                    if (result == AVERROR_EOF) { if (avcodec_send_packet(input.codec, nullptr) < 0) return false; }
                    else if (result < 0) failed(result, "Read video", true);
                    else {
                        result = avcodec_send_packet(input.codec, input.packet); av_packet_unref(input.packet);
                        if (result < 0) failed(result, "Feed video decoder", false);
                    }
                    continue;
                }
                if (result < 0) { failed(result, "Decode video", false); continue; }
                const auto timestamp = input.frame->best_effort_timestamp;
                out.time = timestamp == AV_NOPTS_VALUE ? std::max(0., previous + 1. / rate) : timestamp * av_q2d(stream->time_base) - start;
                QSize dimensions(input.frame->width, input.frame->height);
                if (dimensions.width() > maximum_.width() || dimensions.height() > maximum_.height()) dimensions.scale(maximum_, Qt::KeepAspectRatio);
                out.image = QImage(dimensions, QImage::Format_RGBA8888);
                input.scaler = sws_getCachedContext(input.scaler, input.frame->width, input.frame->height,
                    static_cast<AVPixelFormat>(input.frame->format), out.image.width(), out.image.height(), AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                unsigned char* dest[] = {out.image.bits()}; int strides[] = {static_cast<int>(out.image.bytesPerLine())};
                // The slice height counts source rows, even when the output is downscaled.
                result = out.image.isNull() || !input.scaler ? AVERROR(EINVAL) : sws_scale(input.scaler, input.frame->data, input.frame->linesize, 0, input.frame->height, dest, strides);
                av_frame_unref(input.frame);
                if (result < 0) { failed(result, "Convert video pixels", false); continue; }
                failures = 0; return true;
            }
        };
        double last = -1, serviced = -1, loopStart = -1; bool eof = false, primed = false;
        for (;;) {
            double target, loopEnd;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return stop_ || target_ != serviced; });
                if (stop_) return;
                target = target_; serviced = target; loopEnd = loopEnd_;
                if (loopStart_ != loopStart) { loopStart = loopStart_; if (primed) last = -1; primed = false; }
                if (primed) {
                    // The decoder sits after the loop head while the cached tail plays.
                    if (!cache_.empty() && target >= cache_.front().time && (eof_ || target <= cache_.back().time)) continue;
                    primed = false;
                    if (!head_.empty() && target >= head_.front().time && target <= head_.back().time + 1.) {
                        cache_ = head_; eof_ = eof = headEof_; last = head_.back().time;
                    } else last = -1;
                }
            }
            if (target < 0) continue;
            bool seek = last < 0 || target > last + 1.;
            { std::lock_guard lock(mutex_); if (!cache_.empty() && target < cache_.front().time) seek = true; }
            if (seek) {
                seekTo(target); eof = false; last = -1;
                std::lock_guard lock(mutex_); cache_.clear(); eof_ = false;
            }
            // Near a loop end, decode the whole tail so the loop head can be prerolled before the wrap.
            const bool tail = loopStart >= 0 && target >= loopStart && target >= loopEnd - 15. / rate;
            auto wanted = [&] {
                if (eof) return false;
                if (last < target + lookahead) return true;
                std::lock_guard lock(mutex_); return tail && last < loopEnd && cache_.size() < 17;
            };
            while (wanted()) {
                FrameImage frame;
                if (!next(last, frame)) { eof = true; std::lock_guard lock(mutex_); eof_ = true; break; }
                last = frame.time;
                std::lock_guard lock(mutex_);
                cache_.push_back(std::move(frame));
                // Retain the frame preceding the target and a small lookahead; bound memory.
                while (cache_.size() > 2 && cache_[1].time <= target) cache_.pop_front();
                while (cache_.size() > 18) cache_.pop_front();
            }
            if (!tail || (!eof && last < loopEnd)) continue;
            seekTo(loopStart);
            std::deque<FrameImage> head; double position = -1; bool headEof = false;
            while (position < loopStart + lookahead && head.size() < 13) {
                FrameImage frame;
                if (!next(position, frame)) { headEof = true; break; }
                position = frame.time; head.push_back(std::move(frame));
                while (head.size() > 1 && head[1].time <= loopStart) head.pop_front();
            }
            std::lock_guard lock(mutex_);
            if (loopStart_ == loopStart) { head_ = std::move(head); headEof_ = headEof; }
            primed = true; eof = headEof; last = position;
        }
    } catch (const Stopped&) {
    } catch (const std::exception& error) {
        std::lock_guard lock(mutex_); error_ = QString::fromUtf8(error.what());
    }
}
}
