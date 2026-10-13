// Independent CoreAudio input collector. It never loads a show or opens an output.
// Launch as its own macOS app so audio permission belongs to this collector, not a host editor.
#include <RtAudio.h>
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QVBoxLayout>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <signal.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <array>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <cerrno>
#include <memory>
#include <functional>

void requestMicrophonePermission(std::function<void(bool)> completion);

struct Collector {
    struct Block { std::vector<float> samples; unsigned frames; };
    std::mutex mutex;
    std::condition_variable available;
    std::deque<Block> queue;
    bool done = false;
    std::atomic<unsigned> inputOverflows{0}, queueOverflows{0};
    std::atomic<qint64> frames{0};
    std::atomic<double> firstSample{0};
    std::atomic<bool> writeFailed{false};
    std::array<std::atomic<float>,16> peaks{};
    static int callback(void*, void* input, unsigned count, double, RtAudioStreamStatus status, void* context) {
        auto& self = *static_cast<Collector*>(context);
        if (!self.firstSample.load()) self.firstSample.store(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count()-count/48000.);
        if (status) self.inputOverflows.fetch_add(1);
        Block block{{},count};
        const auto* data = static_cast<const float*>(input);
        for (unsigned channel=0;channel<16;++channel) {
            float peak=0;
            for (unsigned frame=0;frame<count;++frame) peak=std::max(peak,std::abs(data[frame*16+channel]));
            self.peaks[channel].store(peak);
        }
        block.samples.assign(data,data+count*16);
        {
            std::lock_guard lock(self.mutex);
            if (self.queue.size() >= 4000) { self.queueOverflows.fetch_add(1); return 0; }
            self.queue.push_back(std::move(block));
        }
        self.available.notify_one(); return 0;
    }
};

int main(int argc, char** argv) {
    QApplication app(argc,argv);
    app.setApplicationName("Lighthouse Loopback Recorder");
    QCommandLineParser parser; parser.addHelpOption();
    parser.addOption({"output","Evidence directory","path"});
    parser.addOption({"pipe","Named pipe receiving unmodified float32 16-channel input","path"});
    parser.addOption({"device","Required input name","name","BlackHole 16ch"}); parser.process(app);
    const QString out = QDir(parser.value("output")).absolutePath();
    QDir().mkpath(out);
    QWidget window;window.setWindowTitle(app.applicationName());window.resize(520,900);
    window.setStyleSheet("QWidget { background:#192331;color:#dee7f3;font-size:13px; } QProgressBar {border:1px solid #3c5064;height:22px;text-align:center;} QProgressBar::chunk {background:#268d79;} QPlainTextEdit {background:#101a25;}");
    auto* layout=new QVBoxLayout(&window);
    auto* heading=new QLabel("INDEPENDENT BLACKHOLE INPUT\n48 kHz · 16 channels · live received samples");layout->addWidget(heading);
    std::array<QProgressBar*,16> meters{};
    const QStringList names={"Score L","Score R","Guide","Click","Sea L","Sea R","Effects L","Effects R"};
    for (int i=0;i<16;++i) { meters[i]=new QProgressBar;meters[i]->setRange(0,1000);meters[i]->setValue(0);layout->addWidget(meters[i]); }
    layout->addWidget(new QLabel("MIDI RECEIVED · Lighthouse MIDI Logger"));
    auto* midiLog=new QPlainTextEdit;midiLog->setReadOnly(true);midiLog->setMaximumBlockCount(200);layout->addWidget(midiLog,1);
    auto* status=new QLabel("Preparing independent BlackHole input…");status->setWordWrap(true);layout->addWidget(status);
    window.show();
    Collector capture;
    std::unique_ptr<RtAudio> audio;
    std::thread writer;
    auto writeJson = [&](const QString& name,QJsonObject object) {
        QFile file(out+"/"+name); if (!file.open(QIODevice::WriteOnly)) return false;
        return file.write(QJsonDocument(object).toJson(QJsonDocument::Indented)) >= 0;
    };
    QJsonObject info;
    qint64 logOffset=0;unsigned midiMessages=0;
    QTimer displays;displays.setInterval(100);
    QObject::connect(&displays,&QTimer::timeout,[&] {
        for (int i=0;i<16;++i) {
            const float peak=capture.peaks[i].load();meters[i]->setValue(std::min(1000,static_cast<int>(peak*1000)));
            meters[i]->setFormat(QString("%1 · %2 · %3 dBFS").arg(i+1,2,10,QChar('0')).arg(i<8?names[i]:"Unused").arg(peak>0?QString::number(20*std::log10(peak),'f',1):"−∞"));
        }
        QFile log(out+"/midi-received.jsonl");
        if (log.open(QIODevice::ReadOnly)) {
            log.seek(logOffset);
            while (!log.atEnd()) {
                const auto line=log.readLine();if (!line.endsWith('\n')) break;
                logOffset=log.pos();const auto record=QJsonDocument::fromJson(line).object();
                if (!record.isEmpty()) {++midiMessages;midiLog->appendPlainText(QString("%1  %2").arg(record["wall_seconds"].toDouble(),7,'f',2).arg(record["hex"].toString()));}
            }
        }
        status->setText(QString("Received %1 audio frames · %2 MIDI messages\nInput overruns: %3 · Queue drops: %4")
            .arg(capture.frames.load()).arg(midiMessages).arg(capture.inputOverflows.load()).arg(capture.queueOverflows.load()));
    });displays.start();
    QTimer stop;stop.setInterval(100);
    QObject::connect(&stop,&QTimer::timeout,[&] {
        if (capture.writeFailed.load() || QFile::exists(out+"/stop-native-capture")) app.quit();
    });
    auto startCapture = [&] {
        try {
            audio=std::make_unique<RtAudio>(RtAudio::MACOSX_CORE);
            unsigned selected = 0;bool found = false;
            for (auto id:audio->getDeviceIds()) {
                auto d=audio->getDeviceInfo(id);
                const auto name=QString::fromStdString(d.name);
                if ((name==parser.value("device") || name.endsWith(": "+parser.value("device"))) && d.inputChannels>=16) {
                    selected=id;found=true;info={{"name",name},{"channels",16},{"sample_rate",48000}};break;
                }
            }
            if (!found) throw std::runtime_error("Required 16-channel BlackHole input is unavailable");
            // Python's pipe reader is already waiting; this process provides only received input samples.
            const int fd=::open(QFile::encodeName(parser.value("pipe")).constData(),O_WRONLY);
            if (fd<0) throw std::runtime_error("Cannot open the input-sample pipe");
            ::signal(SIGPIPE,SIG_IGN);
            writer=std::thread([&,fd] {
                for (;;) {
                    Collector::Block block;
                    {
                        std::unique_lock lock(capture.mutex);
                        capture.available.wait(lock,[&] { return capture.done || !capture.queue.empty(); });
                        if (capture.queue.empty() && capture.done) break;
                        block=std::move(capture.queue.front());capture.queue.pop_front();
                    }
                    auto* bytes=reinterpret_cast<const char*>(block.samples.data());std::size_t remaining=block.samples.size()*sizeof(float);
                    while (remaining) {
                        const auto written=::write(fd,bytes,remaining);
                        if (written<0 && errno==EINTR) continue;
                        if (written<=0) {capture.writeFailed.store(true);::close(fd);return;}
                        bytes+=written;remaining-=written;
                    }
                    capture.frames.fetch_add(block.frames);
                }
                ::close(fd);
            });
            RtAudio::StreamParameters input;input.deviceId=selected;input.nChannels=16;input.firstChannel=0;
            unsigned buffer=512;
            if (audio->openStream(nullptr,&input,RTAUDIO_FLOAT32,48000,&buffer,Collector::callback,&capture)!=RTAUDIO_NO_ERROR)
                throw std::runtime_error(audio->getErrorText());
            if (audio->startStream()!=RTAUDIO_NO_ERROR) throw std::runtime_error(audio->getErrorText());
            info["buffer_frames"]=static_cast<int>(buffer);
            if (!writeJson("native-capture-ready.json",info)) throw std::runtime_error("Cannot write native input readiness");
            stop.start();
        } catch (const std::exception& e) {
            writeJson("native-capture-error.json",{{"error",QString::fromUtf8(e.what())}});app.exit(1);
        }
    };
    QTimer::singleShot(0,&app,[&] {
        status->setText("Please allow microphone access for independent BlackHole input recording.");
        requestMicrophonePermission([&](bool granted) {
            if (granted) startCapture();
            else {writeJson("native-capture-error.json",{{"error","Microphone permission was not granted to the independent recorder"}});app.exit(1);}
        });
    });
    const int result=app.exec();
    if (audio && audio->isStreamRunning()) audio->stopStream();
    if (audio && audio->isStreamOpen()) audio->closeStream();
    {std::lock_guard lock(capture.mutex);capture.done=true;}capture.available.notify_one();
    if (writer.joinable()) writer.join();
    writeJson("native-capture-summary.json",{{"backend","independent native CoreAudio input app"},{"input_device",info},
        {"audio_start_monotonic",capture.firstSample.load()},{"frames",capture.frames.load()},
        {"input_overflows",static_cast<int>(capture.inputOverflows.load())},
        {"queue_overflows",static_cast<int>(capture.queueOverflows.load())},{"write_failed",capture.writeFailed.load()}});
    return capture.writeFailed.load()?1:result;
}
