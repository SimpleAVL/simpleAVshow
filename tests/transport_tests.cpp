#include "core/Transport.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <functional>
#include <chrono>
#include <cstdlib>
#include <new>
#include <thread>

thread_local bool checkingCallback = false;
thread_local unsigned callbackAllocations = 0;
void* operator new(std::size_t size) {
    if (checkingCallback) ++callbackAllocations;
    if (auto* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* p) noexcept { ::operator delete(p); }

using namespace sav;
namespace {
void expect(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void near(float actual, double expected, const char* message) { if (std::abs(actual - expected) > 1e-5) throw std::runtime_error(message); }
std::shared_ptr<AudioData> audio(std::initializer_list<float> mono) {
    auto data = std::make_shared<AudioData>(); for (auto value : mono) { data->samples.push_back(value); data->samples.push_back(value); } return data;
}
std::shared_ptr<Show> show(bool async = true) {
    auto p = std::make_shared<Show>(); p->duration = 20;
    p->tracks.push_back({"a", "Main", false, async});
    Clip c; c.id = "clip"; c.duration = 20; c.audio = audio({.1f, .2f, .3f, .4f, .5f, .6f, .7f, .8f});
    p->clips.push_back(c); return p;
}
class PatternStorage : public AudioStorage {
public:
    explicit PatternStorage(Frame length) : length_(length) {}
    Frame length() const noexcept override { return length_; }
    static float value(Frame frame) { return static_cast<float>(frame % 97) / 200.f; }
    void read(Frame first, float* out, std::size_t count) const override {
        if (checkingCallback) callbackReads.store(true);
        reads.fetch_add(1);
        if (changed.load()) throw AudioSourceChanged("Test source changed");
        if (fail.load()) throw std::runtime_error("Test cache failure");
        std::this_thread::sleep_for(std::chrono::milliseconds(delay.load()));
        for (std::size_t i = 0; i < count; ++i) out[i * 2] = out[i * 2 + 1] = value(first + static_cast<Frame>(i));
    }
    mutable std::atomic<bool> callbackReads{false};
    mutable std::atomic<unsigned> reads{0};
    std::atomic<bool> fail{false}, changed{false};
    std::atomic<int> delay{0};
private:
    Frame length_;
};
std::shared_ptr<AudioData> diskAudio(const std::shared_ptr<PatternStorage>& storage) {
    auto data = std::make_shared<AudioData>(); data->storage = storage; return data;
}
void checkedRender(Transport& t, float* out, unsigned count, unsigned channels) {
    checkingCallback = true; t.render(out, count, channels); checkingCallback = false;
    expect(callbackAllocations == 0, "callback must never allocate");
}
void play(Transport& t) { expect(t.command({Action::Play}), "play command accepted"); }
void render(Transport& t, unsigned count, std::vector<float>& output, unsigned channels = 2) { output.resize(count * channels); t.render(output.data(), count, channels); }
}
int main() {
    int passed = 0;
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"async clip beginning exactly at a hold starts on arrival", [] {
            auto p = show(); p->markers.push_back({"hold", "Hold", 0, true}); Transport t; t.configure(p); play(t);
            std::vector<float> out; render(t, 3, out); expect(t.state() == State::Held && t.position() == 0, "held at zero"); near(out[0], .1, "first sample plays at arrival"); near(out[4], .3, "ambient continues at zero-time hold");
        }},
        {"hold freezes show time, not async source or automation value", [] {
            auto p = show(); p->markers.push_back({"hold", "Hold", 3, true}); p->clips[0].volume.points = {{3, 1}, {5, 0}};
            Transport t; t.configure(p); play(t); std::vector<float> out; render(t, 5, out);
            expect(t.position() == 3 && t.state() == State::Held, "exact marker boundary");
            near(out[6], .4, "async continues at hold"); near(out[8], .5, "automation held at marker gain");
            play(t); render(t, 3, out); near(out[0], .6, "resume preserves source"); near(out[2], .35, "fade follows resumed show time"); near(out[4], 0, "fade reaches zero");
        }},
        {"normal audio stops at hold, resumes at the same sample", [] {
            auto p = show(false); p->markers.push_back({"hold", "Hold", 3, true});
            Transport t; t.configure(p); play(t); std::vector<float> out; render(t, 8, out);
            near(out[4], .3, "last sample before hold"); for (int i = 6; i < 16; ++i) near(out[i], 0, "normal audio silent while held");
            play(t); render(t, 1, out); near(out[0], .4, "normal audio resumes from hold position");
        }},
        {"manual pause preserves async source", [] {
            Transport t; t.configure(show()); play(t); std::vector<float> out; render(t, 2, out);
            t.command({Action::Pause}); render(t, 2, out); expect(t.position() == 2, "manual pause freezes show"); near(out[0], .3, "async keeps running");
            play(t); render(t, 1, out); near(out[0], .5, "resume continues async source");
        }},
        {"seek inside active async clip preserves its source, outside stops it", [] {
            auto p = show(); p->clips[0].duration = 10;
            Transport t; t.configure(p); play(t); std::vector<float> out; render(t, 2, out);
            t.command({Action::Seek, 6}); render(t, 1, out); near(out[0], .3, "source retained on interior seek"); expect(t.position() == 7, "show seeks independently");
            t.command({Action::Seek, 12}); render(t, 2, out); near(out[0], 0, "exterior seek stops async source"); expect(t.asyncVoices() == 0, "no lingering async voice");
            t.command({Action::Seek, 1}); render(t, 1, out); near(out[0], .2, "inactive clip starts at seek position");
        }},
        {"seek to exclusive clip end stops paused async voice", [] {
            auto p = show(); p->clips[0].duration = 6; Transport t; t.configure(p); play(t);
            std::vector<float> out; render(t, 1, out); t.command({Action::Pause}); t.command({Action::Seek, 6}); render(t, 2, out);
            near(out[0], 0, "end is outside clip"); expect(t.asyncVoices() == 0, "paused seek stops voice");
        }},
        {"looping and non-looping ambience survive an arbitrarily long hold", [] {
            auto p = show(); p->markers.push_back({"hold", "Hold", 1, true}); p->clips[0].loop = true;
            Transport t; t.configure(p); play(t); std::vector<float> out; render(t, 100, out);
            near(out[198], .4, "loop wraps through source on hold"); expect(t.position() == 1, "show remains held"); expect(t.asyncVoices() == 1, "loop still active");
            p = show(); p->markers.push_back({"hold", "Hold", 1, true}); t.configure(p); play(t); render(t, 100, out);
            near(out[198], 0, "one-shot ends"); expect(t.asyncVoices() == 0, "one-shot is finished"); play(t); render(t, 1, out); near(out[0], 0, "one-shot does not restart on resume");
        }},
        {"arrival and resume MIDI fire once; seeks do not send", [] {
            auto p = show(false); p->markers = {{"arrive", "Arrival", 2, true, MidiTiming::Arrival, {0x90, 60, 100}},
                {"resume", "Resume", 5, true, MidiTiming::Resume, {0xc0, 4}}};
            Transport t; t.configure(p); play(t); std::vector<float> out; render(t, 20, out);
            MidiEvent event; expect(t.takeMidi(event) && event.marker == 0 && event.time == 2, "arrival fires"); expect(!t.takeMidi(event), "arrival only once");
            play(t); render(t, 20, out); expect(t.position() == 5, "next hold reached"); expect(!t.takeMidi(event), "resume message waits");
            play(t); render(t, 1, out); expect(t.takeMidi(event) && event.marker == 1 && event.time == 5, "resume message fires"); expect(!t.takeMidi(event), "resume only once");
            t.command({Action::Pause}); t.command({Action::Seek, 2}); render(t, 0, out); expect(!t.takeMidi(event), "seek sends no MIDI");
            play(t); render(t, 1, out); expect(t.takeMidi(event) && event.marker == 0, "landing on a marker arms it for play");
        }},
        {"multiple same-time holds are consumed together", [] {
            auto p = show(); p->markers = {{"a", "A", 2, true}, {"b", "B", 2, true, MidiTiming::Resume, {0xc0, 2}}};
            Transport t; t.configure(p); play(t); std::vector<float> out; render(t, 4, out); play(t); render(t, 1, out);
            expect(t.position() == 3 && t.state() == State::Playing, "resume does not hit same marker again"); MidiEvent e; expect(t.takeMidi(e) && e.marker == 1, "same-time resume MIDI fires");
        }},
        {"multichannel routes are synchronous and remain isolated", [] {
            auto p = show(false); p->tracks[0].left = 0; p->tracks[0].right = 1;
            p->tracks.push_back({"guide", "Guide", false, false, false, 1, 0, 2, -1}); p->tracks.push_back({"click", "Click", false, false, false, 1, 0, 3, -1});
            auto guide = p->clips[0]; guide.track = 1; guide.audio = audio({.2f}); p->clips.push_back(guide);
            auto click = p->clips[0]; click.track = 2; click.audio = audio({.3f}); p->clips.push_back(click);
            Transport t; t.configure(p); play(t); std::vector<float> out; render(t, 1, out, 6);
            near(out[0], .1, "main left"); near(out[1], .1, "main right"); near(out[2], .2, "guide only on channel 3"); near(out[3], .3, "click only on channel 4"); near(out[4], 0, "unused channel silent"); near(out[5], 0, "unused channel silent");
        }},
        {"Stop All stops ambience without resetting show time", [] {
            Transport t; t.configure(show()); play(t); std::vector<float> out; render(t, 2, out); t.command({Action::StopAll}); render(t, 4, out);
            expect(t.position() == 2 && t.state() == State::Stopped && t.asyncVoices() == 0, "hard stop state"); for (auto sample : out) near(sample, 0, "hard stop silence");
        }},
        {"show end stops all voices", [] {
            auto p = show(); p->duration = 4; p->clips[0].duration = 4; p->clips[0].loop = true;
            Transport t; t.configure(p); play(t); std::vector<float> out; render(t, 12, out);
            expect(t.state() == State::Ended && t.position() == 4, "show end boundary"); near(out[8], 0, "async silent at project end"); expect(t.asyncVoices() == 0, "project end clears voices");
        }},
        {"mixing clamps overload and reports clipping", [] {
            auto p = show(false); p->clips[0].gain = 4;
            Transport t; t.configure(p); play(t); std::vector<float> out; render(t, 5, out);
            near(out[8], 1, "output is bounded"); expect(t.clipped(), "overload visible to UI");
        }},
        {"final hold can resume into end of show", [] {
            auto p = show(); p->duration = 4; p->clips[0].duration = 4; p->markers.push_back({"final", "Final", 4, true, MidiTiming::Resume, {0xfc}});
            Transport t; t.configure(p); play(t); std::vector<float> out; render(t, 10, out); expect(t.state() == State::Held && t.position() == 4, "final hold reached");
            play(t); render(t, 1, out); expect(t.state() == State::Ended && t.asyncVoices() == 0, "resume ends the show"); MidiEvent e; expect(t.takeMidi(e), "final resume MIDI delivered");
        }},
        {"mono constant-power pan and a disconnected leg", [] {
            auto p = show(false); auto source = std::make_shared<AudioData>(*p->clips[0].audio); source->mono = true; p->clips[0].audio = source;
            p->tracks[0].pan = -1; p->tracks[0].right = -1;
            Transport t; t.configure(p); play(t); std::vector<float> out; render(t, 1, out, 4);
            near(out[0], .1, "mono hard left full gain"); near(out[1], 0, "disconnected leg"); near(out[2], 0, "unrouted channel"); near(out[3], 0, "unrouted channel");
        }},
        {"muting an async track does not restart its source", [] {
            auto p = show(); p->tracks[0].muted = true;
            Transport t; t.configure(p); play(t); std::vector<float> out; render(t, 3, out); expect(t.asyncVoices() == 1, "muted source still advances"); near(out[0], 0, "mute is silent");
        }},
        {"mixing overload remains bounded on a held async track", [] {
            auto p = show(); p->clips[0].gain = 4; p->markers.push_back({"hold", "Hold", 3, true});
            Transport t; t.configure(p); play(t); std::vector<float> out; render(t, 5, out);
            expect(t.state() == State::Held && t.position() == 3, "overload does not disturb hold timing");
            near(out[8], 1, "output is bounded"); expect(t.clipped(), "overload visible to UI");
        }},
        {"large unsorted timelines schedule boundaries without fixed capacities", [] {
            auto p = show(false); p->clips.clear(); p->duration = frames(48 * 3600);
            for (int i = 700; i >= 0; --i) {
                Clip c; c.track = 0; c.start = i * 2; c.duration = 1; c.audio = audio({.1f}); p->clips.push_back(c);
                p->markers.push_back({"m", "Cue", i * 2, false, MidiTiming::Arrival, {0xfa}});
            }
            Transport t; t.configure(p); play(t); std::vector<float> out(2804);
            checkedRender(t, out.data(), 1402, 2);
            for (int i = 0; i < 701; ++i) { near(out[i * 4], .1, "scheduled clip onset"); near(out[i * 4 + 2], 0, "exclusive clip end"); }
            MidiEvent event; int count = 0;
            while (t.takeMidi(event)) { expect(event.time == count * 2, "unsorted markers delivered in time order"); ++count; }
            expect(count == 701 && !t.eventOverflow(), "large cue group delivered");
            t.command({Action::Seek, 200}); checkedRender(t, out.data(), 1, 2); near(out[0], .1, "seek rebuilds active set");
            p = show(false); p->markers.assign(2000, {"m", "Cue", 0, false, MidiTiming::Arrival, {0xfa}});
            t.configure(p); play(t); checkedRender(t, out.data(), 1, 2); count = 0;
            while (t.takeMidi(event)) ++count;
            expect(count == 2000 && !t.eventOverflow(), "MIDI capacity scales to simultaneous project cues");
        }},
        {"disk read-ahead rotates safely with no callback reads or allocations", [] {
            auto storage = std::make_shared<PatternStorage>(frames(48 * 3600));
            auto p = show(false); p->duration = storage->length(); p->clips[0].duration = p->duration; p->clips[0].audio = diskAudio(storage);
            Transport t; t.configure(p); play(t); std::vector<float> out(1024 * 2);
            for (int block = 0; block < 128; ++block) {
                checkedRender(t, out.data(), 1024, 2);
                for (int i = 0; i < 1024; ++i) near(out[i * 2], PatternStorage::value(block * 1024 + i), "disk audio matches exact source frame");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            expect(t.bufferWaits() == 0, "read-ahead keeps up with callback blocks");
            expect(!storage->callbackReads.load(), "disk access never runs on callback thread");
            expect(storage->reads.load() < 24, "disk reads occur per block rather than sample");
        }},
        {"cache miss waits without advancing any track and recovers at exact seek frame", [] {
            auto storage = std::make_shared<PatternStorage>(frames(48 * 3600));
            auto p = show(false); p->duration = storage->length(); p->clips[0].duration = p->duration; p->clips[0].audio = diskAudio(storage);
            auto companion = p->clips[0]; companion.audio = audio({.1f, .2f, .3f, .4f}); companion.loop = true; p->clips.push_back(companion);
            Transport t; t.configure(p); play(t); float out[2]; checkedRender(t, out, 1, 2);
            storage->delay.store(30);
            const Frame destination = frames(30 * 3600) + 123;
            t.command({Action::Seek, destination}); checkedRender(t, out, 1, 2);
            expect(t.position() == destination && t.bufferWaits() == 1, "seek miss freezes sample clock"); near(out[0], 0, "all tracks wait together");
            for (int i = 0; i < 1000 && t.position() == destination; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1)); checkedRender(t, out, 1, 2);
            }
            expect(t.position() == destination + 1, "buffering recovers");
            near(out[0], PatternStorage::value(destination) + .4, "recovery skips no samples on either track");
            expect(!storage->callbackReads.load(), "seek never reads disk on callback");
        }},
        {"disk async loop keeps offset through hold, pause, seek, and Stop All", [] {
            auto storage = std::make_shared<PatternStorage>(17003);
            auto p = show(); p->clips[0].audio = diskAudio(storage); p->clips[0].offset = 8131; p->clips[0].loop = true;
            p->markers.push_back({"hold", "Hold", 0, true}); Transport t; t.configure(p); play(t);
            std::vector<float> out(40000 * 2); checkedRender(t, out.data(), 40000, 2);
            for (Frame i = 0; i < 40000; ++i) near(out[i * 2], PatternStorage::value(8131 + i % (17003 - 8131)), "loop wraps to source in");
            expect(t.state() == State::Held && t.position() == 0 && t.asyncVoices() == 1, "async hold continues on disk");
            t.command({Action::Pause}); checkedRender(t, out.data(), 1, 2);
            t.command({Action::Seek, 10}); checkedRender(t, out.data(), 1, 2);
            near(out[0], PatternStorage::value(8131 + 40001 % (17003 - 8131)), "interior seek retains async cursor");
            t.command({Action::StopAll}); checkedRender(t, out.data(), 1, 2); near(out[0], 0, "disk voice stops");
            expect(!storage->callbackReads.load() && !t.bufferWaits(), "short loop is fully prebuffered");
        }},
        {"unreadable disk cache is reported outside the callback", [] {
            auto storage = std::make_shared<PatternStorage>(frames(10)); storage->fail.store(true);
            auto p = show(false); p->clips[0].audio = diskAudio(storage);
            Transport t; t.configure(p); expect(t.audioReadFailed(), "pre-roll reports storage failure");
            play(t); float out[2]; checkedRender(t, out, 1, 2); near(out[0], 0, "failure does not emit garbage");
            expect(t.position() == 0 && !storage->callbackReads.load(), "failure never advances or accesses disk on callback");
        }},
        {"disk clip replays without a miss after seeking back before it", [] {
            auto storage = std::make_shared<PatternStorage>(frames(60));
            auto p = show(false); p->duration = frames(120); p->clips[0].start = 1000; p->clips[0].duration = storage->length(); p->clips[0].audio = diskAudio(storage);
            Transport t; t.configure(p); play(t); std::vector<float> out(512 * 2);
            for (int block = 0; block < 100; ++block) { checkedRender(t, out.data(), 512, 2); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            const auto waits = t.bufferWaits();
            t.command({Action::Seek, 0}); checkedRender(t, out.data(), 1, 2);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            checkedRender(t, out.data(), 512, 2); checkedRender(t, out.data(), 512, 2);
            expect(t.bufferWaits() == waits, "seek back re-reads the clip entry before it is needed");
            near(out[(1000 - 513) * 2], PatternStorage::value(0), "replay starts at the source-in frame");
            expect(!storage->callbackReads.load(), "retargeting never reads disk on callback");
        }},
        {"re-arming keeps resident read-ahead for unchanged disk clips", [] {
            auto storage = std::make_shared<PatternStorage>(frames(60));
            auto p = show(false); p->duration = frames(120); p->clips[0].duration = storage->length(); p->clips[0].audio = diskAudio(storage);
            Transport t; t.configure(p); const auto reads = storage->reads.load(); expect(reads > 0, "first arm prebuffers");
            auto edited = std::make_shared<Show>(*p); edited->markers.push_back({"m", "Cue", frames(90), true});
            t.configure(edited); expect(storage->reads.load() == reads, "unchanged clip performs no new reads");
            play(t); std::vector<float> out(64 * 2); checkedRender(t, out.data(), 64, 2);
            for (int i = 0; i < 64; ++i) near(out[i * 2], PatternStorage::value(i), "reused read-ahead plays the entry");
            expect(t.bufferWaits() == 0 && !storage->callbackReads.load(), "reuse never misses");
            edited = std::make_shared<Show>(*edited); edited->clips[0].offset = 10; t.configure(edited);
            expect(storage->reads.load() > reads, "changed offset prebuffers again");
            play(t); checkedRender(t, out.data(), 64, 2); near(out[0], PatternStorage::value(10), "new offset plays");
            const auto moved = storage->reads.load(); t.configure(edited, frames(30));
            expect(storage->reads.load() > moved, "a non-resident entry prebuffers"); expect(t.bufferWaits() == 0, "re-arm resets wait count");
        }},
        {"transient read failures retry, then recover without latching", [] {
            auto storage = std::make_shared<PatternStorage>(frames(48 * 3600));
            auto p = show(false); p->duration = storage->length(); p->clips[0].duration = p->duration; p->clips[0].audio = diskAudio(storage);
            Transport t; t.configure(p); play(t); float out[2]; checkedRender(t, out, 1, 2);
            storage->fail.store(true);
            const Frame destination = frames(20 * 3600);
            t.command({Action::Seek, destination});
            for (int i = 0; i < 100; ++i) { checkedRender(t, out, 1, 2); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
            expect(t.position() == destination && t.readRetries() > 0 && !t.audioReadFailed(), "a locked file waits and retries");
            storage->fail.store(false);
            for (int i = 0; i < 1000 && t.position() == destination; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); checkedRender(t, out, 1, 2); }
            expect(t.position() == destination + 1 && !t.audioReadFailed(), "playback resumes once the file is readable");
            near(out[0], PatternStorage::value(destination), "recovery resumes at the exact frame");
        }},
        {"persistent read failures latch after the retry window; changed sources latch at once", [] {
            for (const bool sourceChanged : {false, true}) {
                auto storage = std::make_shared<PatternStorage>(frames(48 * 3600));
                auto p = show(false); p->duration = storage->length(); p->clips[0].duration = p->duration; p->clips[0].audio = diskAudio(storage);
                Transport t; t.configure(p); play(t); float out[2]; checkedRender(t, out, 1, 2);
                (sourceChanged ? storage->changed : storage->fail).store(true);
                t.command({Action::Seek, frames(20 * 3600)}); checkedRender(t, out, 1, 2);
                const auto started = std::chrono::steady_clock::now();
                while (!t.audioReadFailed() && std::chrono::steady_clock::now() - started < std::chrono::seconds(4))
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                const auto elapsed = std::chrono::steady_clock::now() - started;
                expect(t.audioReadFailed(), "unrecoverable reads are reported");
                expect(sourceChanged ? elapsed < std::chrono::milliseconds(500) : elapsed >= AudioBuffer::retryWindow - std::chrono::milliseconds(100),
                    sourceChanged ? "changed source fails without retrying" : "transient failures retry for the full window");
            }
        }},
        {"a single connected leg carries the whole source", [] {
            auto p = show(false); p->tracks[0].left = 1; p->tracks[0].right = -1; p->tracks[0].pan = .7;
            auto stereo = std::make_shared<AudioData>(); stereo->samples = {.2f, .4f}; p->clips[0].audio = stereo; p->clips[0].loop = true;
            Transport t; t.configure(p); play(t); float out[2]; checkedRender(t, out, 1, 2);
            near(out[0], 0, "disconnected leg stays silent"); near(out[1], .3, "stereo sums to mono at full level, ignoring pan");
            auto mono = audio({.5f}); mono->mono = true; p->clips[0].audio = mono; p->tracks[0].left = -1; p->tracks[0].right = 0;
            t.configure(p); play(t); checkedRender(t, out, 1, 2); near(out[0], .5, "mono source keeps full level on one leg");
        }},
        {"non-finite samples are silenced and flagged", [] {
            auto p = show(false); p->clips[0].audio = audio({std::nanf(""), .25f});
            auto companion = p->clips[0]; companion.audio = audio({.25f}); companion.loop = true; p->clips.push_back(companion);
            Transport t; t.configure(p); play(t); float out[4]; checkedRender(t, out, 2, 2);
            near(out[0], 0, "NaN never reaches the device"); expect(t.clipped(), "NaN latches the overload indicator");
            near(out[2], .5, "following samples mix normally");
        }},
        {"cached envelope segments and pan gains match per-sample evaluation", [] {
            auto p = show(); p->duration = 4000; p->clips[0].duration = 4000; p->clips[0].loop = true;
            auto source = audio({.5f}); source->mono = true; p->clips[0].audio = source;
            p->clips[0].volume.points = {{100, 0}, {1100, 1}, {1101, .2}, {1150, .25}, {3000, .75}};
            p->clips[0].balance.points = {{0, -1}, {2000, 1}, {2500, -.5}};
            const auto expected = [&](Frame local) {
                const double pan = std::clamp(p->clips[0].balance.at(local, 0), -1., 1.), gain = .5 * p->clips[0].volume.at(local, 1);
                return std::pair{gain * std::cos((pan + 1.) * 0.7853981633974483), gain * std::sin((pan + 1.) * 0.7853981633974483)};
            };
            const auto check = [&](const std::vector<float>& out, Frame from, Frame count) {
                for (Frame f = 0; f < count; ++f) {
                    const auto [left, right] = expected(from + f);
                    expect(std::abs(out[f * 2] - left) < 1e-4 && std::abs(out[f * 2 + 1] - right) < 1e-4, "automation matches Envelope::at");
                }
            };
            Transport t; t.configure(p); play(t); std::vector<float> out(4000 * 2);
            checkedRender(t, out.data(), 3990, 2); check(out, 0, 3990);
            t.command({Action::Seek, 1090}); checkedRender(t, out.data(), 200, 2); check(out, 1090, 200);
        }},
        {"envelope interpolation follows exact frame position", [] {
            Envelope e{{{10, .2}, {20, .8}}}; near(e.at(0, 1), .2, "before first key"); near(e.at(15, 1), .5, "linear middle"); near(e.at(30, 1), .8, "after last key");
        }}
    };
    for (const auto& [name, test] : tests) {
        try { test(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { std::cerr << "FAIL " << name << ": " << e.what() << '\n'; return 1; }
    }
    std::cout << passed << " transport scenarios passed\n";
}
