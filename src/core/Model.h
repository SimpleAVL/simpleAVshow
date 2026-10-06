#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sav {
using Frame = std::int64_t;
inline constexpr int sampleRate = 48000;
// JSON stores times as doubles. Keep frame integers exactly representable,
// with ample headroom for timing arithmetic (rather than a show-duration cap).
inline constexpr Frame maxFrame = (Frame{1} << 53) - 1;
inline constexpr double maxSeconds = static_cast<double>(maxFrame / sampleRate);
inline Frame frames(double seconds) { return static_cast<Frame>(std::llround(seconds * sampleRate)); }
inline double seconds(Frame frame) { return static_cast<double>(frame) / sampleRate; }

struct Keyframe { Frame time = 0; double value = 1.; };
struct Envelope {
    std::vector<Keyframe> points;
    double at(Frame time, double fallback) const noexcept {
        if (points.empty()) return fallback;
        if (time <= points.front().time) return points.front().value;
        const auto right = std::upper_bound(points.begin(), points.end(), time,
            [](Frame t, const Keyframe& p) { return t < p.time; });
        if (right == points.end()) return points.back().value;
        const auto& a = *(right - 1);
        const double t = static_cast<double>(time - a.time) / (right->time - a.time);
        return a.value + t * (right->value - a.value);
    }
};
class AudioStorage {
public:
    virtual ~AudioStorage() = default;
    virtual Frame length() const noexcept = 0;
    // Worker-only: may perform disk I/O and throw on failure.
    virtual void read(Frame first, float* stereo, std::size_t count) const = 0;
};
struct AudioData {
    // Generated/test sources may stay resident. Imported sources use disk PCM.
    // Both representations are stereo, interleaved, 48 kHz.
    std::vector<float> samples;
    std::shared_ptr<const AudioStorage> storage;
    static constexpr Frame peakStride = 480;
    std::vector<float> peaks;
    bool mono = false;
    Frame length() const noexcept { return storage ? storage->length() : static_cast<Frame>(samples.size() / 2); }
    float peakAt(Frame frame) const noexcept {
        if (frame < 0 || frame >= length()) return 0;
        if (!peaks.empty()) return peaks[static_cast<std::size_t>(frame / peakStride)];
        return samples.empty() ? 0 : std::max(std::abs(samples[frame * 2]), std::abs(samples[frame * 2 + 1]));
    }
};
struct Track {
    std::string id, name;
    bool visual = false, async = false, muted = false;
    double gain = 1., pan = 0.;
    // Zero-based physical channels; -1 disconnects this leg.
    int left = 0, right = 1;
};
struct Clip {
    std::string id, assetId, text;
    int track = 0;
    Frame start = 0, duration = sampleRate * 10, offset = 0;
    bool loop = false;
    double gain = 1., pan = 0., x = 0., y = 0., scale = 1., opacity = 1.;
    Envelope volume, balance, positionX, positionY, size, alpha;
    std::shared_ptr<const AudioData> audio;
    Frame end() const noexcept { return start + duration; }
    bool contains(Frame time) const noexcept { return time >= start && time < end(); }
};
enum class MidiTiming { Arrival, Resume };
struct Marker {
    std::string id, name;
    Frame time = 0;
    bool hold = true;
    MidiTiming timing = MidiTiming::Arrival;
    std::vector<unsigned char> message;
};
struct Show {
    std::vector<Track> tracks;
    std::vector<Clip> clips;
    std::vector<Marker> markers;
    Frame duration = sampleRate * 60;
};
}
