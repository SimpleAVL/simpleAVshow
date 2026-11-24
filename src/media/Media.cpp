#include "media/Media.h"
#include "media/AudioSources.h"
#include <QFileInfo>
#include <QUuid>
#include <QImageReader>
#include <QTemporaryFile>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
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
// Local files only: no format may reach the network on its own (an .sdp or HLS playlist would).
AVDictionary* localOnly() { AVDictionary* options = nullptr; av_dict_set(&options, "protocol_whitelist", "file", 0); return options; }
// A media file opened by the app for FFmpeg rather than through FFmpeg's file protocol. On Windows that
// protocol's C runtime open denies deleting or renaming the file while it is open. Streamed audio stays
// open for the whole show, and streaming is the mode for building one, where an editor re-exporting a
// stem (often by renaming a new file over it) must not fail with "file in use"; so this opens with read,
// write and delete sharing, and a replaced or removed file surfaces as a changed or missing source.
// Performances use prepared caches, which do not read the original. Other systems use POSIX calls.
class SourceFile {
public:
    explicit SourceFile(const QString& path) {
#ifdef Q_OS_WIN
        handle_ = CreateFileW(detail::windowsPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) throw std::runtime_error("Open media: cannot open " + path.toStdString());
#else
        fd_ = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC);
        if (fd_ < 0) throw std::runtime_error("Open media: " + std::string(std::strerror(errno)));
#endif
        constexpr int size = 64 * 1024;
        auto* buffer = static_cast<unsigned char*>(av_malloc(size));
        if (buffer) io_ = avio_alloc_context(buffer, size, 0, this, &SourceFile::read, nullptr, &SourceFile::seek);
        if (!io_) { av_free(buffer); close(); throw std::bad_alloc(); }
    }
    ~SourceFile() { av_freep(&io_->buffer); avio_context_free(&io_); close(); }
    SourceFile(const SourceFile&) = delete;
    SourceFile& operator=(const SourceFile&) = delete;
    AVIOContext* io() const noexcept { return io_; }
    // Identity and size of the file actually open, whatever is at its path by now, as far as the handle
    // can tell (some network servers report no identity).
    struct Opened { std::optional<detail::FileIdentity> identity; std::optional<qint64> size; };
    Opened opened() const {
#ifdef Q_OS_WIN
        BY_HANDLE_FILE_INFORMATION info{};
        if (GetFileInformationByHandle(handle_, &info))
            return {detail::FileIdentity{info.dwVolumeSerialNumber, (quint64{info.nFileIndexHigh} << 32) | info.nFileIndexLow},
                static_cast<qint64>((quint64{info.nFileSizeHigh} << 32) | info.nFileSizeLow)};
        LARGE_INTEGER size;
        if (GetFileSizeEx(handle_, &size)) return {std::nullopt, size.QuadPart};
        return {};
#else
        struct stat info{};
        if (::fstat(fd_, &info)) return {};
        return {detail::FileIdentity{static_cast<quint64>(info.st_dev), static_cast<quint64>(info.st_ino)}, static_cast<qint64>(info.st_size)};
#endif
    }
private:
#ifdef Q_OS_WIN
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    void close() noexcept { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
#else
    int fd_ = -1;
    void close() noexcept { if (fd_ >= 0) ::close(fd_); }
#endif
    AVIOContext* io_ = nullptr;
    static int read(void* opaque, std::uint8_t* buffer, int size) {
        const auto* self = static_cast<const SourceFile*>(opaque);
#ifdef Q_OS_WIN
        DWORD got = 0;
        if (!ReadFile(self->handle_, buffer, static_cast<DWORD>(size), &got, nullptr)) return AVERROR(EIO);
#else
        const auto got = ::read(self->fd_, buffer, static_cast<std::size_t>(size));
        if (got < 0) return AVERROR(errno);
#endif
        return got ? static_cast<int>(got) : AVERROR_EOF;
    }
    static std::int64_t seek(void* opaque, std::int64_t offset, int whence) {
        const auto* self = static_cast<const SourceFile*>(opaque);
        whence &= ~AVSEEK_FORCE;
#ifdef Q_OS_WIN
        if (whence == AVSEEK_SIZE) { LARGE_INTEGER size; return GetFileSizeEx(self->handle_, &size) ? size.QuadPart : AVERROR(EIO); }
        LARGE_INTEGER to, at; to.QuadPart = offset;
        const DWORD method = whence == SEEK_SET ? FILE_BEGIN : whence == SEEK_CUR ? FILE_CURRENT : FILE_END;
        return SetFilePointerEx(self->handle_, to, &at, method) ? at.QuadPart : AVERROR(EIO);
#else
        if (whence == AVSEEK_SIZE) { struct stat info{}; return ::fstat(self->fd_, &info) ? AVERROR(errno) : static_cast<std::int64_t>(info.st_size); }
        const auto at = ::lseek(self->fd_, offset, whence);
        return at < 0 ? AVERROR(errno) : static_cast<std::int64_t>(at);
#endif
    }
};
// The demuxer for path, from its bytes and name alone. Formats that play other files (playlists,
// manifests, split recordings, scripts) depend on files that change, move and vanish independently,
// so neither caches keyed by this file's bytes nor change detection could follow them. Opening runs
// a format's header reader (which would already fetch segments or evaluate a script), so they are
// refused here, and the format found is forced on open.
// io: an open file to probe, rewound afterwards so it can be opened as detected (no second open of path).
const AVInputFormat* identify(const QString& path, AVIOContext* io) {
    const AVInputFormat* detected = nullptr;
    checked(av_probe_input_buffer2(io, &detected, path.toUtf8().constData(), nullptr, 0, 0), "Identify media");
    for (const auto& name : QByteArray(detected->name).split(','))
        if (name == "concat" || name == "hls" || name == "dash" || name == "imf" || name == "webm_dash_manifest" ||
            name == "mlv" || name == "sdp" || name == "avisynth" || name == "vapoursynth")
            throw std::runtime_error("Playlists, manifests and split recordings that refer to other files cannot be imported; import the media files themselves");
    return detected;
}
const AVInputFormat* identify(const QString& path) { const SourceFile probe(path); return identify(path, probe.io()); }
struct Input {
    std::unique_ptr<SourceFile> file; // Outlives format, which reads through it.
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
    // Sharing::Lock keeps FFmpeg's own file protocol, which on Windows stops the file from being deleted
    // or renamed while open: playing videos stay protected during a show, as before streaming existed.
    enum class Sharing { AllowReplace, Lock };
    // expected: the file as imported; the file actually opened is checked against it before anything
    // reads it, so a replacement reports as changed rather than as whatever parsing it would fail with.
    void open(const QString& path, Sharing sharing = Sharing::AllowReplace, const std::optional<detail::FileVersion>& expected = {}) {
        const AVInputFormat* detected = nullptr;
        if (sharing == Sharing::AllowReplace) {
            file = std::make_unique<SourceFile>(path);
            // Whatever the handle can tell is compared; identity only where both sides know it.
            if (expected) {
                const auto now = file->opened();
                if ((now.size && *now.size != expected->size) ||
                    (now.identity && *now.identity != detail::FileIdentity{} && expected->identity != detail::FileIdentity{} && *now.identity != expected->identity))
                    throw AudioSourceChanged("Audio source changed; reopen the project: " + path.toStdString());
            }
            detected = identify(path, file->io());
            format = avformat_alloc_context();
            if (!format) throw std::bad_alloc();
            format->pb = file->io();
        } else detected = identify(path);
        AVDictionary* options = localOnly();
        const int opened = avformat_open_input(&format, path.toUtf8().constData(), detected, &options); av_dict_free(&options);
        checked(opened, "Open media");
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
    using SeekPoints = std::shared_ptr<const std::vector<detail::SeekPoint>>;
    // expected: the file as imported. Every open (including a restart) checks the file it actually
    // opened against it, so a replacement between a path check and the open cannot be decoded.
    explicit AudioDecoder(QString path, SeekPoints points = {}, std::optional<detail::FileVersion> expected = {})
        : path_(std::move(path)), points_(std::move(points)), expected_(std::move(expected)) { open(); }
    bool mono() const noexcept { return mono_; }
    QByteArray format() const { return input_->format->iformat->name; }
    // Record seek points during the coming linear decode (the import pass).
    void record() { recording_ = true; }
    std::vector<detail::SeekPoint> recorded() && { return ordered_ ? std::move(recorded_) : std::vector<detail::SeekPoint>{}; }
    // The next decoded run, frames [first, first + samples.size() / 2). False once the source ends.
    bool next(Frame& first, std::vector<float>& samples) {
        samples.clear();
        while (!finished_) {
            try {
                if (receive()) {
                    convert(input_->frame, first, samples); av_frame_unref(input_->frame);
                } else if (landing_) { restart(); continue; } // Ended before the landing point.
                else if (convert(nullptr, first, samples) <= 0) finished_ = true; // Drains the resampler's delay line.
            } catch (const std::exception&) {
                // Warm-up after a seek can start on data that only decodes in sequence (a packet split
                // across a program-stream boundary); decoding from the start always works.
                if (!landing_) throw;
                restart(); continue;
            }
            if (!samples.empty()) return true;
        }
        return false;
    }
    // Continue output from at or before frame, so the next runs cover it.
    void seek(Frame frame) {
        if (!started_) {
            // The first frame's timestamp places the source.
            if (receive()) { begin(input_->frame->best_effort_timestamp); av_frame_unref(input_->frame); }
            finished_ = false;
        }
        const Frame output = std::max<Frame>(0, frame - start_ - preroll);
        // Resume on a resampling phase boundary, where output and input samples coincide exactly.
        target_ = output / stepOut_ * stepIn_;
        // Land on the last seek point at or before the target. Decoding starts one point earlier,
        // so the decoder has warmed up (some codecs output nothing for the first packet after a
        // seek) by the time it reaches the landing packet, whose exact sample the import recorded.
        // Without two such points, decode from the start.
        if (!points_ || target_ == 0) { restart(); return; }
        const auto after = std::upper_bound(points_->begin(), points_->end(), target_,
            [](std::int64_t sample, const detail::SeekPoint& p) { return sample < p.sample; });
        if (after - points_->begin() < 2) { restart(); return; }
        if (av_seek_frame(input_->format, stream_, (after - 2)->pts, AVSEEK_FLAG_BACKWARD) < 0) { restart(); return; }
        landing_ = *(after - 1);
        reset();
    }
private:
    QString path_;
    SeekPoints points_;
    std::optional<detail::FileVersion> expected_;
    std::vector<detail::SeekPoint> recorded_;
    std::optional<detail::SeekPoint> landing_; // The seek point a seek aims at.
    std::deque<std::pair<std::int64_t, std::int64_t>> sentAt_; // Recent packets' (timestamp, byte offset).
    std::int64_t sentFrom(std::int64_t pts) const {
        for (auto it = sentAt_.rbegin(); it != sentAt_.rend(); ++it) if (it->first == pts) return it->second;
        return -1;
    }
    std::int64_t nextPoint_ = 0, lastPts_ = AV_NOPTS_VALUE;
    bool recording_ = false, ordered_ = true;
    std::unique_ptr<Input> input_;
    int stream_ = -1, rate_ = 0, channels_ = 0, format_ = AV_SAMPLE_FMT_NONE;
    bool mono_ = false, started_ = false, aligned_ = false, finished_ = false;
    double origin_ = 0;
    Frame start_ = 0;                        // Show frame of output sample 0.
    std::int64_t stepIn_ = 1, stepOut_ = 1;  // Input and output samples per resampling phase cycle.
    std::int64_t inputAt_ = 0, target_ = 0, produced_ = 0;
    std::vector<float> scratch_;
    void open() {
        input_ = std::make_unique<Input>(); input_->open(path_, Input::Sharing::AllowReplace, expected_);
        stream_ = av_find_best_stream(input_->format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (stream_ < 0) throw std::runtime_error("File contains no audio");
        input_->decoder(stream_);
        if (input_->codec->ch_layout.nb_channels > 2)
            throw std::runtime_error("Import mono or stereo stems separately; multichannel source files are not supported yet");
        channels_ = input_->codec->ch_layout.nb_channels; mono_ = channels_ == 1; rate_ = input_->codec->sample_rate; format_ = input_->codec->sample_fmt;
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
        started_ = aligned_ = finished_ = false; landing_.reset(); sentAt_.clear();
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
            if (input_->packet->stream_index == stream_) {
                // Which byte offset each recent timestamp came from, for recording and checking seek points.
                sentAt_.emplace_back(input_->packet->pts, input_->packet->pos);
                if (sentAt_.size() > 64) sentAt_.pop_front();
            }
            const int sent = input_->packet->stream_index == stream_ ? avcodec_send_packet(input_->codec, input_->packet) : 0;
            av_packet_unref(input_->packet);
            checked(sent, "Feed audio decoder");
        }
    }
    void begin(std::int64_t pts) {
        started_ = true; inputAt_ = 0; start_ = 0;
        if (pts == AV_NOPTS_VALUE) return;
        const double startTime = pts * av_q2d(input_->format->streams[stream_]->time_base) - origin_;
        if (!std::isfinite(startTime) || std::abs(startTime) > maxSeconds) throw std::runtime_error("Invalid audio stream start time");
        start_ = frames(startTime);
    }
    // Decoding again from the start always matches; also the fallback when seeking cannot be trusted.
    void restart() { target_ = 0; open(); }
    void reset() {
        avcodec_flush_buffers(input_->codec); swr_close(input_->resampler);
        checked(swr_init(input_->resampler), "Initialize resampling");
        aligned_ = finished_ = recording_ = false; sentAt_.clear();
    }
    // Returns the resampler's output count (0 once a flush is drained); samples get the part at or after frame 0.
    int convert(AVFrame* frame, Frame& first, std::vector<float>& samples) {
        const std::uint8_t* planes[2]{};
        int count = 0;
        if (frame) {
            const auto pts = frame->best_effort_timestamp;
            // The resampler and the plane offsets below assume the stream's opening format throughout.
            if (frame->ch_layout.nb_channels != channels_ || frame->format != format_)
                throw std::runtime_error("Audio format changes partway through the file; convert it to one format before importing");
            if (!started_) begin(pts);
            else if (!aligned_) {
                // After a seek: frames before the landing point only warm the decoder up.
                if (!landing_ || pts == AV_NOPTS_VALUE || pts < landing_->pts) return 0;
                // The landing packet never decoded, or a computed timestamp named another packet.
                if (pts > landing_->pts || (landing_->pos >= 0 && sentFrom(pts) != landing_->pos)) { restart(); return 0; }
                inputAt_ = landing_->sample; landing_.reset();
            }
            if (!aligned_) { aligned_ = true; produced_ = target_ / stepIn_ * stepOut_; }
            if (recording_) {
                // Seek points need increasing timestamps; a stream without them seeks by decoding from the start.
                ordered_ = ordered_ && pts != AV_NOPTS_VALUE && (lastPts_ == AV_NOPTS_VALUE || pts > lastPts_); lastPts_ = pts;
                if (ordered_ && inputAt_ >= nextPoint_) { recorded_.push_back({pts, inputAt_, sentFrom(pts)}); nextPoint_ = inputAt_ + rate_; }
            }
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
    // version: the file as the import hashed it, before decoding; anything else at the path is a change.
    StreamStorage(QString path, Frame length, AudioDecoder::SeekPoints points, detail::FileVersion version)
        : path_(std::move(path)), length_(length), points_(std::move(points)), version_(version) {
        const std::lock_guard lock(registry().mutex); registry().storages.push_back(this); id_ = ++registry().clock;
    }
    ~StreamStorage() override { const std::lock_guard lock(registry().mutex); std::erase(registry().storages, this); }
    Frame length() const noexcept override { return length_; }
    bool decodes() const noexcept override { return true; }
    bool readsSource() const noexcept override { return true; }
    void read(Frame first, float* stereo, std::size_t count) const override {
        if (first < 0 || first > length_ || count > static_cast<std::size_t>(length_ - first)) throw std::runtime_error("Invalid audio source range");
        std::fill(stereo, stereo + count * 2, 0.f);
        if (!count) return;
        const std::lock_guard lock(mutex_); ++reads_;
        // A missing or changed source releases its open decoders first, so their handles do not keep a
        // deleted file's name reserved (Windows) or pin an old copy while the operator restores it.
        // A file deleted while open on Windows keeps its name until handles close; it is not present, so
        // unavailable too: retried, not reported as changed. An identity the file system does not report
        // right now is not a change (sameAs).
        // One lookup: values from a failed one (a share dropping now) would look like a change.
        const QFileInfo info(path_);
        if (!info.exists() || !info.lastModified().isValid() || !detail::present(path_)) {
            readers_.clear(); throw std::runtime_error("Audio source is unavailable: " + path_.toStdString());
        }
        // File identity also catches a replacement saved with the same size and modification time;
        // an in-place rewrite that keeps both is not detected.
        const detail::FileVersion current{info.size(), info.lastModified().toMSecsSinceEpoch(), detail::fileIdentity(path_)};
        if (!current.sameAs(version_)) { readers_.clear(); throw AudioSourceChanged("Audio source changed; reopen the project: " + path_.toStdString()); }
        Reader* reader = nullptr;
        try {
            pick(first, reader);
            const Frame end = first + static_cast<Frame>(count);
            for (Frame at = first; at < end;) {
                const Frame have = reader->first + static_cast<Frame>(reader->samples.size() / 2);
                if (have <= at) {
                    // The import measured this length; ending early means the source no longer decodes the same.
                    if (!reader->decoder.next(reader->first, reader->samples)) throw std::runtime_error("Audio source ended early: " + path_.toStdString());
                    continue;
                }
                if (reader->first > at) { at = std::min(end, reader->first); continue; } // Leading silence stays zero.
                const Frame stop = std::min(end, have);
                std::copy(reader->samples.begin() + (at - reader->first) * 2, reader->samples.begin() + (stop - reader->first) * 2, stereo + (at - first) * 2);
                at = stop;
            }
            reader->used = ++registry().clock; reader->usedAt = std::chrono::steady_clock::now();
        } catch (...) {
            // A failed decoder's position is unknown; the next read opens a fresh one.
            if (reader) std::erase_if(readers_, [reader](const auto& r) { return r.get() == reader; });
            throw;
        }
    }
private:
    static constexpr std::size_t maxReaders = 4;
    // Each open decoder holds a file descriptor, and every armed clip prebuffers through one, so a
    // show with hundreds of sources would exhaust the process's descriptors (256 by default on
    // macOS). Open decoders are capped across all streamed sources: the least recently used idle
    // one closes first and reopens with a seek (milliseconds, inside the read-ahead) when needed.
    static constexpr std::size_t openBudget = 64;
    struct Registry {
        std::mutex mutex;
        std::vector<const StreamStorage*> storages;
        std::atomic<std::size_t> open{0};
        std::atomic<std::uint64_t> clock{0};
    };
    // Never destroyed: a background sweep may still hold it while the process exits.
    static Registry& registry() { static auto* instance = new Registry; return *instance; }
public:
    // Sources reading right now are skipped (their locks are only tried), and so is any decoder read
    // within idleAfter: a playing clip reads every block, so only idle decoders close.
    // wait: finish before returning (tests); otherwise presence checks and closing run on a worker.
    static void closeIdle(bool wait) {
        constexpr auto idleAfter = std::chrono::seconds(3);
        const auto now = std::chrono::steady_clock::now();
        // One sweep at a time, and the slot comes first: readers are only taken by the sweep that will close
        // them off this thread. A busy sweep (a stalled share) skips this tick; tests wait for it.
        static std::atomic<bool> sweeping{false};
        while (sweeping.exchange(true)) { if (!wait) return; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        struct Open { const StreamStorage* storage; std::uint64_t id, reads; QString path; };
        std::vector<Open> open;
        std::vector<std::unique_ptr<Reader>> retired; // Closed without locks held: closing can stall on a share.
        {
            auto& r = registry(); const std::lock_guard lock(r.mutex);
            for (const auto* storage : r.storages) {
                const std::unique_lock other(storage->mutex_, std::try_to_lock); if (!other) continue;
                auto& readers = storage->readers_;
                const auto idle = std::stable_partition(readers.begin(), readers.end(), [&](const auto& reader) { return now - reader->usedAt < idleAfter; });
                std::move(idle, readers.end(), std::back_inserter(retired)); readers.erase(idle, readers.end());
                if (!readers.empty()) open.push_back({storage, storage->id_, storage->reads_, storage->path_});
            }
        }
        // A source deleted while open (FAT/exFAT on Windows) keeps its name until its handles close:
        // release them so the file can be restored or replaced under that name. Checks and closes can
        // stall (network shares), so they run on a detached thread, one sweep at a time, without locks
        // that reads need; the app can quit without waiting for one.
        if (open.empty() && retired.empty()) { sweeping = false; return; }
        auto sweep = [open = std::move(open), retired = std::move(retired)]() mutable {
            retired.clear();
            std::erase_if(open, [](const Open& entry) { return detail::present(entry.path); });
            for (const auto& entry : open) {
                std::vector<std::unique_ptr<Reader>> gone;
                {
                    auto& r = registry(); const std::lock_guard lock(r.mutex);
                    // Only the same registration, not read since: otherwise the file came back.
                    const auto* storage = entry.storage;
                    if (std::find(r.storages.begin(), r.storages.end(), storage) == r.storages.end() || storage->id_ != entry.id) continue;
                    const std::unique_lock other(storage->mutex_, std::try_to_lock);
                    if (other && storage->reads_ == entry.reads) gone = std::move(storage->readers_);
                }
            }
            sweeping = false;
        };
        if (!wait) try { std::thread(std::move(sweep)).detach(); return; } catch (const std::system_error&) {}
        sweep(); // Tests, or no thread available: close here rather than keep the slot or throw into the timer.
    }
private:
    // Reading forward this far decodes through instead of seeking.
    static constexpr Frame continueWithin = 2 * sampleRate;
    struct Reader {
        // Takes over the budget slot makeRoom reserved.
        Reader(const QString& path, AudioDecoder::SeekPoints points, const detail::FileVersion& version) : decoder(path, std::move(points), version) {}
        ~Reader() { --registry().open; }
        Reader(const Reader&) = delete; Reader& operator=(const Reader&) = delete;
        AudioDecoder decoder;
        Frame first = 0;
        std::vector<float> samples;
        std::uint64_t used = 0;
        std::chrono::steady_clock::time_point usedAt = std::chrono::steady_clock::now();
    };
    QString path_;
    Frame length_;
    AudioDecoder::SeekPoints points_;
    detail::FileVersion version_;
    mutable std::mutex mutex_;
    mutable std::vector<std::unique_ptr<Reader>> readers_;
    mutable std::uint64_t reads_ = 0; // Under mutex_: lets the sweep tell whether a read happened since it looked.
    std::uint64_t id_ = 0; // Unique per registration, unlike the address.
    // Sets reader before seeking it, so a failed seek discards that decoder.
    void pick(Frame frame, Reader*& reader) const {
        for (const auto& r : readers_) {
            const Frame have = r->first + static_cast<Frame>(r->samples.size() / 2);
            if (r->first <= frame && frame < have + continueWithin && (!reader || r->first > reader->first)) reader = r.get();
        }
        if (reader) return;
        if (readers_.size() < maxReaders) {
            makeRoom();
            std::unique_ptr<Reader> fresh;
            try { fresh = std::make_unique<Reader>(path_, points_, version_); }
            catch (...) { --registry().open; throw; } // The reserved slot was never taken.
            readers_.push_back(std::move(fresh)); reader = readers_.back().get();
            if (frame < continueWithin) return; // A fresh decoder starts at the beginning.
        } else reader = std::min_element(readers_.begin(), readers_.end(), [](const auto& a, const auto& b) { return a->used < b->used; })->get();
        reader->samples.clear(); reader->first = 0; reader->decoder.seek(frame);
    }
    // Called holding mutex_: closes least recently used decoders, here or in sources not reading
    // right now (their locks are only tried, so two sources making room cannot deadlock), until a
    // new one fits the budget, then reserves its slot before the registry lock is released, so
    // concurrent opens cannot claim the same one. If every other source is busy, the budget is
    // exceeded for now.
    void makeRoom() const {
        auto& r = registry();
        const std::lock_guard lock(r.mutex);
        while (r.open.load() >= openBudget) {
            const StreamStorage* owner = nullptr; std::size_t index = 0;
            std::vector<std::unique_lock<std::mutex>> held;
            for (const auto* storage : r.storages) {
                if (storage != this) { std::unique_lock other(storage->mutex_, std::try_to_lock); if (!other) continue; held.push_back(std::move(other)); }
                for (std::size_t i = 0; i < storage->readers_.size(); ++i)
                    if (!owner || storage->readers_[i]->used < owner->readers_[index]->used) { owner = storage; index = i; }
            }
            if (!owner) break;
            owner->readers_.erase(owner->readers_.begin() + static_cast<std::ptrdiff_t>(index));
        }
        ++r.open;
    }
};

std::atomic<AudioPreparation> preference{AudioPreparation::Stream};
void addPeaks(std::vector<float>& peaks, Frame first, const std::vector<float>& samples) {
    const auto count = static_cast<Frame>(samples.size() / 2);
    peaks.resize(static_cast<std::size_t>((first + count + AudioData::peakStride - 1) / AudioData::peakStride), 0.f);
    for (Frame i = 0; i < count; ++i) {
        auto& peak = peaks[static_cast<std::size_t>((first + i) / AudioData::peakStride)];
        peak = std::max(peak, std::max(std::abs(samples[i * 2]), std::abs(samples[i * 2 + 1])));
    }
}
std::shared_ptr<const AudioData> loadAudio(const QString& path, AudioPreparation preparation) {
    // The source is hashed before the decoder opens it: a replacement in between changes the stamp,
    // which saving the summary or publishing the cache checks, instead of filing the old decode
    // under the replacement's content key.
    const bool streaming = preparation == AudioPreparation::Stream;
    const QByteArray format = identify(path)->name;
    std::optional<detail::StreamSummary> summary; std::optional<detail::AudioCache> prepared;
    if (streaming) summary.emplace(path, format); else prepared.emplace(path, format);
    // The decoder opens the file as hashed (or fails as changed) and must pick the demuxer the key names.
    AudioDecoder decoder(path, {}, streaming ? summary->version() : prepared->version());
    if (decoder.format() != format) throw std::runtime_error("Audio source changed during preparation; import it again");
    auto data = std::make_shared<AudioData>(); data->mono = decoder.mono();
    Frame first = 0; std::vector<float> samples;
    if (streaming) {
        // One decode pass finds the exact length and waveform; playback decodes again on demand.
        auto known = summary->reuse(data->mono);
        if (known) summary->verify();
        else {
            known = detail::AudioSummary{0, data->mono, {}, {}};
            decoder.record();
            while (decoder.next(first, samples)) { addPeaks(known->peaks, first, samples); known->length = first + static_cast<Frame>(samples.size() / 2); }
            if (known->length <= 0) throw std::runtime_error("Audio stream is empty");
            known->seekPoints = std::move(decoder).recorded();
            summary->save(*known);
        }
        data->peaks = std::move(known->peaks);
        data->storage = std::make_shared<StreamStorage>(path, known->length,
            std::make_shared<const std::vector<detail::SeekPoint>>(std::move(known->seekPoints)), summary->version());
        return data;
    }
    if (auto reused = prepared->reuse(data->mono)) { prepared->verify(); return reused; }
    QTemporaryFile cache(prepared->temporaryTemplate());
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
    data->storage = prepared->publish(cache, *data, total);
    return data;
}
}

void closeIdleStreams(bool wait) { StreamStorage::closeIdle(wait); }
void setAudioPreparation(AudioPreparation value) noexcept { preference.store(value); }
AudioPreparation audioPreparation() noexcept { return preference.load(); }

MediaAsset loadMedia(const QString& path) { return loadMedia(path, audioPreparation()); }
MediaAsset loadMedia(const QString& path, AudioPreparation preparation) {
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
        asset.audio = loadAudio(asset.path, preparation);
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
        Input input; input.open(path_, Input::Sharing::Lock);
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
