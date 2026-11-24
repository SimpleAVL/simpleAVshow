#pragma once
#include "app/Project.h"
#include "media/AudioDevice.h"
#include "media/MidiOutput.h"
#include <QObject>
#include <QTimer>
#include <QSettings>
#include <QElapsedTimer>
#include <deque>
#include <set>
#include <functional>
#include <map>
#include <mutex>
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
    bool showMode = false, dirty = false, busy = false, preflightLock = false;
    QString path, status = "Press Play to use your speakers. Choose show equipment in Outputs.", audioName;
    QString playbackProblem;
    Frame position() const { return transport.position(); }
    // The operator's next cue: later than the playhead, or at it unless that hold is already holding.
    const Marker* nextCue() const;
    bool canEdit(bool report = true);
    // Exclusive busy lease so operator dialogs and async MCP file jobs cannot step on each other.
    quint64 acquireBusy();
    void releaseBusy(quint64 lease);
    quint64 projectRevision() const { return projectRevision_; }
    quint64 showEpoch() const { return showEpoch_; }
    bool edit(const std::function<void(Project&)>& mutation);
    void undo();
    void redo();
    std::size_t undoCount() const { return undo_.size(); }
    std::size_t redoCount() const { return redo_.size(); }
    void replace(Project next, QString projectPath = {}, bool markDirty = false);
    bool toggle();
    bool play();
    bool pause();
    bool seek(Frame position);
    bool stopAll();
    // Reflects queued transport commands immediately before the next audio callback publishes them.
    State effectiveState() const;
    Frame effectivePosition() const;
    bool playInFlight() const { return lastPlaySeq_ > transport.commandsApplied(); }
    bool configureAudio(RtAudio::Api api, unsigned id, QString name, unsigned channels, unsigned buffer);
    bool configureAudioByName(RtAudio::Api api, const QString& wantedName, unsigned channels, unsigned buffer);
    RtAudio::Api activeAudioApi() const { return api_; }
    bool audioConfigured() const { return configured_; }
    bool configureMidi(int port);
    bool configureMidiByName(const QString& portName);
    bool testMidi(const std::vector<unsigned char>& message, const QString& owner = {});
    void cancelPendingTestMidi(const QString& owner = {});
    bool needsMidi() const;
    // Offline sources and vanished videos used on the timeline.
    QStringList missingMedia(bool probeFilesystem = true) const;
    // Problems worth fixing before a performance; empty when the show is ready.
    QStringList preflight(bool probeHardware = true);
    QString midiDestination() const { return midiName_; }
    void setShowMode(bool enabled);
    QImage compose();
    // Side-effect-free composition at an arbitrary show time (safe during playback and Show mode).
    // Pre-decoded video frames keyed by clip ID may be supplied from a worker thread so the GUI thread stays non-blocking.
    static QImage composeProjectAt(const Project& project, Frame position, QSize maximum = QSize(), const QMap<std::string, QImage>* videoFrames = nullptr);
    QImage composeAt(Frame position, QSize maximum = QSize(), const QMap<std::string, QImage>* videoFrames = nullptr) const {
        return composeProjectAt(project, position, maximum, videoFrames);
    }
    bool save(QString target);
    // Saving an opened package or generated media copies files; MainWindow runs that part on a worker.
    bool savingCopiesMedia() const;
    static QString mediaDirectory(const QString& target);
    bool commitSave(Project candidate, QString target);
    void report(QString message);
    // Sidecar for crash recovery: next to the show, or in app data while untitled.
    QString autosavePath() const;
    static QString untitledAutosavePath();
    void discardAutosave();
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
    std::atomic<bool> midiStop_{true}, showModeAtomic_{false};
    std::atomic<std::uint64_t> testMidiGen_{1};
    struct PendingTestMidi {
        std::vector<unsigned char> bytes;
        std::uint64_t epoch = 0;
        std::uint64_t gen = 0;
        QString owner;
    };
    std::mutex testMidiMutex_;
    std::deque<PendingTestMidi> testMidiQueue_;
    QElapsedTimer midiWatch_, stall_;
    std::uint64_t heartbeat_ = 0;
    std::set<std::string> videoErrors_;
    QImage lastPicture_;
    Frame lastComposed_ = -1;
    std::vector<Frame> lastClocks_;
    std::deque<Project> undo_, redo_;
    quint64 busyLease_ = 0, nextBusyLease_ = 1, projectRevision_ = 0, showEpoch_ = 0;
    std::uint64_t commandsPosted_ = 0, lastPlaySeq_ = 0;
    State predictedState_ = State::Stopped;
    Frame predictedPosition_ = 0;
    bool postTransportCommand(Command cmd);
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
