#include "media/Media.h"
#include <QFileInfo>
#include <QUuid>
#include <QImageReader>
#include <QTemporaryFile>
#include <QDir>
#include <limits>
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
class PcmStorage final : public AudioStorage {
public:
    PcmStorage(QString path, Frame length) : path_(std::move(path)), length_(length) {}
    ~PcmStorage() override { QFile::remove(path_); }
    Frame length() const noexcept override { return length_; }
    void read(Frame first, float* stereo, std::size_t count) const override {
        if (first < 0 || first > length_ || count > static_cast<std::size_t>(length_ - first)) throw std::runtime_error("Invalid audio cache range");
        QFile file(path_);
        const qint64 bytes = static_cast<qint64>(count) * 2 * sizeof(float);
        if (!file.open(QIODevice::ReadOnly) || !file.seek(first * 2 * sizeof(float)) ||
            file.read(reinterpret_cast<char*>(stereo), bytes) != bytes)
            throw std::runtime_error("Cannot read prepared audio cache: " + file.errorString().toStdString());
    }
private:
    QString path_;
    Frame length_;
};
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
}

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
    const int audio = av_find_best_stream(input.format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audio >= 0) {
        input.decoder(audio);
        if (input.codec->ch_layout.nb_channels > 2)
            throw std::runtime_error("Import mono or stereo stems separately; multichannel source files are not supported yet");
        auto data = std::make_shared<AudioData>(); data->mono = input.codec->ch_layout.nb_channels == 1;
        QTemporaryFile cache(QDir::tempPath() + "/simpleavshow-pcm-XXXXXX");
        if (!cache.open()) throw std::runtime_error("Cannot create audio cache: " + cache.errorString().toStdString());
        Frame total = 0;
        std::vector<float> scratch;
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        checked(swr_alloc_set_opts2(&input.resampler, &stereo, AV_SAMPLE_FMT_FLT, sampleRate,
            &input.codec->ch_layout, input.codec->sample_fmt, input.codec->sample_rate, 0, nullptr), "Configure resampling");
        checked(swr_init(input.resampler), "Initialize resampling");
        bool firstFrame = true;
        const double origin = mediaOrigin(input.format);
        Frame trimAtStart = 0;
        auto convert = [&](AVFrame* frame) {
            if (firstFrame && frame) {
                firstFrame = false;
                const auto pts = frame->best_effort_timestamp;
                if (pts != AV_NOPTS_VALUE) {
                    const double startTime = pts * av_q2d(input.format->streams[audio]->time_base) - origin;
                    if (!std::isfinite(startTime) || std::abs(startTime) > maxSeconds) throw std::runtime_error("Invalid audio stream start time");
                    const Frame start = frames(startTime);
                    if (start > 0) {
                        // A sparse zero-filled prefix preserves embedded A/V offsets
                        // without allocating leading silence in RAM.
                        total = start;
                        if (!cache.resize(total * 2 * sizeof(float)) || !cache.seek(total * 2 * sizeof(float)))
                            throw std::runtime_error("Cannot prepare audio start offset: " + cache.errorString().toStdString());
                        data->peaks.resize(static_cast<std::size_t>((total + AudioData::peakStride - 1) / AudioData::peakStride), 0.f);
                    } else trimAtStart = -start;
                }
            }
            const int samples = frame ? frame->nb_samples : 0;
            const int capacity = static_cast<int>(av_rescale_rnd(swr_get_delay(input.resampler, input.codec->sample_rate) + samples,
                sampleRate, input.codec->sample_rate, AV_ROUND_UP));
            if (capacity <= 0) return 0;
            scratch.resize(static_cast<std::size_t>(capacity) * 2);
            auto* dest = reinterpret_cast<unsigned char*>(scratch.data());
            const int written = swr_convert(input.resampler, &dest, capacity, frame ? const_cast<const unsigned char**>(frame->extended_data) : nullptr, samples);
            checked(written, "Resample audio");
            // FFmpeg's default mono -> stereo downmix applies -3 dB. Undo it;
            // the transport applies constant-power mono pan exactly once.
            if (data->mono) for (int i = 0; i < written * 2; ++i) scratch[i] *= 1.41421356237f;
            const Frame skip = std::min(trimAtStart, static_cast<Frame>(written));
            trimAtStart -= skip;
            const Frame kept = written - skip;
            if (kept > maxFrame - total) throw std::runtime_error("Audio duration exceeds representable frame timing");
            const qint64 bytes = kept * 2 * sizeof(float);
            if (bytes && cache.write(reinterpret_cast<const char*>(scratch.data() + skip * 2), bytes) != bytes)
                throw std::runtime_error("Cannot write audio cache (check available disk space): " + cache.errorString().toStdString());
            data->peaks.resize(static_cast<std::size_t>((total + kept + AudioData::peakStride - 1) / AudioData::peakStride), 0.f);
            for (Frame i = 0; i < kept; ++i) {
                auto& peak = data->peaks[static_cast<std::size_t>((total + i) / AudioData::peakStride)];
                peak = std::max(peak, std::max(std::abs(scratch[(skip + i) * 2]), std::abs(scratch[(skip + i) * 2 + 1])));
            }
            total += kept;
            return written;
        };
        auto receive = [&] {
            for (;;) {
                int result = avcodec_receive_frame(input.codec, input.frame);
                if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
                checked(result, "Decode audio"); convert(input.frame); av_frame_unref(input.frame);
            }
        };
        int readResult = 0;
        while ((readResult = av_read_frame(input.format, input.packet)) >= 0) {
            if (input.packet->stream_index == audio) {
                checked(avcodec_send_packet(input.codec, input.packet), "Feed audio decoder"); receive();
            }
            av_packet_unref(input.packet);
        }
        if (readResult != AVERROR_EOF) checked(readResult, "Read audio packets");
        checked(avcodec_send_packet(input.codec, nullptr), "Flush audio decoder"); receive();
        while (convert(nullptr) > 0) {}
        if (!cache.flush()) throw std::runtime_error("Cannot finish audio cache: " + cache.errorString().toStdString());
        data->storage = std::make_shared<PcmStorage>(cache.fileName(), total);
        cache.setAutoRemove(false); cache.close(); // Shared storage removes it after the last snapshot releases it.
        asset.audio = data;
        if (!asset.video) asset.duration = seconds(data->length());
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
QImage VideoReader::imageAt(double time) {
    std::lock_guard lock(mutex_);
    target_ = std::max(0., time); wake_.notify_one();
    if (target_ < firstTime_) {
        QImage blank(1, 1, QImage::Format_RGBA8888); blank.fill(Qt::transparent); return blank;
    }
    if (cache_.empty() || (!eof_ && cache_.back().time < target_)) return {};
    QImage result;
    for (const auto& frame : cache_) {
        if (frame.time <= target_ + .0001) result = frame.image;
        else break;
    }
    return result;
}
void VideoReader::run() {
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
        double last = -1, serviced = -1; bool eof = false;
        for (;;) {
            double target;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return stop_ || target_ != serviced; });
                if (stop_) return;
                target = target_; serviced = target;
            }
            if (target < 0) continue;
            bool seek = last < 0 || target > last + 1.;
            { std::lock_guard lock(mutex_); if (!cache_.empty() && target < cache_.front().time) seek = true; }
            if (seek) {
                const auto timestamp = static_cast<std::int64_t>((target + start) / av_q2d(stream->time_base));
                checked(av_seek_frame(input.format, index, timestamp, AVSEEK_FLAG_BACKWARD), "Seek video");
                avcodec_flush_buffers(input.codec); eof = false; last = -1;
                std::lock_guard lock(mutex_); cache_.clear(); eof_ = false;
            }
            while (!eof && last < target + lookahead) {
                { std::lock_guard lock(mutex_); if (stop_) return; }
                int result = avcodec_receive_frame(input.codec, input.frame);
                if (result == AVERROR(EAGAIN)) {
                    for (;;) {
                        result = av_read_frame(input.format, input.packet);
                        if (result < 0) break;
                        if (input.packet->stream_index == index) break;
                        av_packet_unref(input.packet);
                    }
                    if (result == AVERROR_EOF) checked(avcodec_send_packet(input.codec, nullptr), "Flush video");
                    else {
                        checked(result, "Read video");
                        checked(avcodec_send_packet(input.codec, input.packet), "Feed video decoder");
                        av_packet_unref(input.packet);
                    }
                    continue;
                }
                if (result == AVERROR_EOF) { eof = true; std::lock_guard lock(mutex_); eof_ = true; break; }
                checked(result, "Decode video");
                const auto timestamp = input.frame->best_effort_timestamp;
                last = timestamp == AV_NOPTS_VALUE ? std::max(0., last + 1. / 24) : timestamp * av_q2d(stream->time_base) - start;
                QSize dimensions(input.frame->width, input.frame->height);
                if (dimensions.width() > maximum_.width() || dimensions.height() > maximum_.height()) dimensions.scale(maximum_, Qt::KeepAspectRatio);
                QImage image(dimensions, QImage::Format_RGBA8888);
                input.scaler = sws_getCachedContext(input.scaler, input.frame->width, input.frame->height,
                    static_cast<AVPixelFormat>(input.frame->format), image.width(), image.height(), AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                if (!input.scaler) throw std::runtime_error("Cannot convert video pixels");
                unsigned char* dest[] = {image.bits()}; int strides[] = {static_cast<int>(image.bytesPerLine())};
                // The slice height counts source rows, even when the output is downscaled.
                checked(sws_scale(input.scaler, input.frame->data, input.frame->linesize, 0, input.frame->height, dest, strides), "Convert video pixels");
                av_frame_unref(input.frame);
                std::lock_guard lock(mutex_);
                cache_.push_back({last, image});
                // Retain the frame preceding the target and a small lookahead; bound memory.
                while (cache_.size() > 2 && cache_[1].time <= target) cache_.pop_front();
                while (cache_.size() > 18) cache_.pop_front();
            }
        }
    } catch (const std::exception& error) {
        std::lock_guard lock(mutex_); error_ = QString::fromUtf8(error.what());
    }
}
}
