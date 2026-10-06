#include "core/Transport.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <functional>

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
