#pragma once
#include "app/Project.h"
#include "media/AudioDevice.h"
#include "media/MidiOutput.h"
#include <QObject>
#include <QTimer>
#include <QSettings>
#include <QElapsedTimer>
#include <set>
#include <functional>
#include <map>
#include <thread>
#include <atomic>

namespace sav {
class Controller : public QObject {
    Q_OBJECT
public:
    explicit Controller(QObject* parent = nullptr);
    ~Controller() override;
    Project project;
    Transport transport;
    AudioDevice device;
    MidiOutput midi;
    bool showMode = false, dirty = false, busy = false;
    QString path, status = "Press Play to use your speakers. Choose show equipment in Outputs.", audioName;
    QString playbackProblem;
    Frame position() const { return transport.position(); }
    bool canEdit(bool report = true);
    bool edit(const std::function<void(Project&)>& mutation);
    void undo();
    void redo();
    void replace(Project next, QString projectPath = {});
    void toggle();
    void seek(Frame position);
    void stopAll();
    bool configureAudio(RtAudio::Api api, unsigned id, QString name, unsigned channels, unsigned buffer);
    bool configureMidi(int port);
    bool testMidi(const std::vector<unsigned char>& message);
    bool needsMidi() const;
    QString missingVideo() const;
    QString midiDestination() const { return midiName_; }
    void setShowMode(bool enabled);
    QImage compose();
    void save(QString target);
    void report(QString message);
    bool routesValid() const;
    unsigned lateFrames = 0;
signals:
    void changed();
    void tick();
    void message(QString message);
private:
    QTimer timer_, autosave_;
    RtAudio::Api api_ = RtAudio::UNSPECIFIED;
    unsigned deviceId_ = 0, channels_ = 2, buffer_ = 512;
    bool configured_ = false;
    QString midiName_;
    std::map<std::string, std::unique_ptr<VideoReader>> videos_;
    std::shared_ptr<const Show> armed_;
    std::thread midiThread_;
    std::atomic<bool> midiStop_{true};
    QElapsedTimer midiWatch_;
    std::set<std::string> videoErrors_;
    QImage lastPicture_;
    Frame lastComposed_ = -1;
    std::vector<Project> undo_, redo_;
    void arm(Frame position);
    void startMidiThread();
    void watchMidi();
    void stopMidiThread();
    bool prepareAudio();
    bool prepareMidi();
    bool openAudio(RtAudio::Api api, unsigned id, QString name, unsigned channels, unsigned buffer);
    bool openMidi(int port);
};
}
