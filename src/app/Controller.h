#pragma once
#include "app/Project.h"
#include "media/AudioDevice.h"
#include "media/MidiOutput.h"
#include <QObject>
#include <QTimer>
#include <QSet>
#include <QSettings>
#include <QElapsedTimer>
#include <deque>
#include <set>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <atomic>
#include <mutex>

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
    // While disarmed after a failed arm, the position Play will arm at (seeks move it).
    Frame position() const { return armFailed_ ? pendingPosition_ : transport.position(); }
    // The operator's next cue: later than the playhead, or at it unless that hold is already holding.
    const Marker* nextCue() const;
    bool canEdit(bool report = true);
    // Exclusive busy lease so operator dialogs and async MCP file jobs cannot step on each other.
    quint64 acquireBusy();
    void releaseBusy(quint64 lease);
    quint64 projectRevision() const { return projectRevision_; }
    quint64 historyEpoch() const { return historyEpoch_; }
    quint64 showEpoch() const { return showEpoch_; }
    bool edit(const std::function<void(Project&)>& mutation);
    void undo();
    void redo();
    std::size_t undoCount() const { return undo_.size(); }
    std::size_t redoCount() const { return redo_.size(); }
    void replace(Project next, QString projectPath = {}, bool markDirty = false);
    bool toggle();
    bool play(CommandSource source = CommandSource::Operator);
    bool pause(CommandSource source = CommandSource::Operator);
    bool seek(Frame position, CommandSource source = CommandSource::Operator);
    bool stopAll(CommandSource source = CommandSource::Operator);
    // Reflects still-authorized queued transport commands immediately before the next audio callback publishes them.
    State effectiveState() const;
    Frame effectivePosition() const;
    bool playInFlight() const;
    bool configureAudio(RtAudio::Api api, unsigned id, QString name, unsigned channels, unsigned buffer);
    bool configureAudioByName(RtAudio::Api api, const QString& wantedName, unsigned channels, unsigned buffer);
    RtAudio::Api activeAudioApi() const { return api_; }
    bool audioConfigured() const { return configured_; }
    bool configureMidi(int port);
    bool configureMidiByName(const QString& portName);
    bool testMidi(const std::vector<unsigned char>& message, const QString& owner = {},
                  std::shared_ptr<std::atomic<bool>> ownerCancelled = {});
    void cancelPendingTestMidi(const QString& owner = {});
    bool needsMidi() const;
    // Offline sources and vanished videos used on the timeline.
    QStringList missingMedia(bool probeFilesystem = true) const;
    // The missing files the last background check found, among those the show still uses; no disk access.
    QStringList lastMissingMedia() const;
    // The warning for what the last check during a show found missing; empty when nothing is.
    QString missingMediaWarning() const;
    // Starts a background check of the show's media unless one is running; wait finishes it (tests).
    void checkMediaInBackground(bool wait = false);
    // Whether an asset needs relinking: offline since opening, or its file is gone though playback
    // needs it (a video, or audio read from its original rather than a prepared cache).
    static bool needsRelink(const MediaAsset& asset);
    // Problems worth fixing before a performance; empty when the show is ready.
    QStringList preflight(bool probeHardware = true);
    // Playback preferences for this machine, remembered across sessions.
    void setLateAudio(LateAudio policy);
    void setAudioPreparation(AudioPreparation preparation);
    // File-backed audio sources of the show and its undo/redo history.
    QStringList audioSources() const;
    // Whether any file-backed audio in the show or its history is prepared other than as preparation says.
    bool audioDiffersFrom(AudioPreparation preparation) const;
    // Swaps reloaded audio, by source path, into the show and its history (after a preparation change)
    // without an undo step. Rejected changes nothing (editing locked, or the show cannot use it);
    // ArmFailed kept the new audio but could not re-arm playback (reported; Play retries).
    enum class AudioSwap { Rejected, Applied, ArmFailed };
    AudioSwap replaceAudio(const QMap<QString, std::shared_ptr<const AudioData>>& audio);
    QString midiDestination() const { return midiName_; }
    void setShowMode(bool enabled);
    QImage compose();
    // Side-effect-free composition at an arbitrary show time (safe during playback and Show mode).
    // Pre-decoded video frames keyed by clip ID may be supplied from a worker thread so the GUI thread stays non-blocking.
    static QImage composeProjectAt(const Project& project, Frame position, QSize maximum = QSize(), const QMap<std::string, QImage>* videoFrames = nullptr);
    QImage composeAt(Frame position, QSize maximum = QSize(), const QMap<std::string, QImage>* videoFrames = nullptr) const {
        return composeProjectAt(project, position, maximum, videoFrames);
    }
    bool save(QString target, bool allowOverwrite = true);
    // Saving an opened package or generated media copies files; MainWindow runs that part on a worker.
    bool savingCopiesMedia() const;
    static QString mediaDirectory(const QString& target);
    bool commitSave(Project candidate, QString target, bool allowOverwrite = true);
    void report(QString message);
    // Sidecar for crash recovery: next to the show, or in app data while untitled.
    QString autosavePath() const;
    static QString untitledAutosavePath();
    const QString& recoveredAutosavePath() const { return recoveredAutosavePath_; }
    void setRecoveredAutosavePath(QString sidecarPath) { recoveredAutosavePath_ = std::move(sidecarPath); }
    void discardAutosave();
    bool routesValid() const;
    bool armFailed() const { return armFailed_; }
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
    QString recoveredAutosavePath_;
    QString midiProblem_; // The playback problem MIDI last reported; a reconnect clears only that.
    // The last arm failed: nothing is armed, and Play arms at pendingPosition_ (updated by seeks).
    // armProblem_ is the playback problem it reported; a later successful arm clears only that.
    bool armFailed_ = false;
    bool pausedLive_ = false; // Paused by the operator during playback, not stopped or sought since.
    // Files the show needs that are offline, and those it needs present now (videos, streamed audio).
    void mediaToCheck(QStringList& offline, QStringList& needed) const;
    // Shared with the checking thread, which may outlive the controller.
    struct MediaCheck { std::mutex mutex; QStringList missing; bool running = false; }; // Both under mutex.
    std::shared_ptr<MediaCheck> mediaCheck_ = std::make_shared<MediaCheck>();
    QStringList missingNow_; // What the last media check during a show found missing.
    QString missingWarning_; // Its warning, kept with it.
    QStringList lastMissingMedia(const QStringList& offline, const QStringList& needed) const;
    // Reads the latest result and, unless one is running, starts the next check, under one lock.
    QStringList lastMissingMediaAndCheck(const QStringList& offline, const QStringList& needed);
    static void startMediaCheck(const std::shared_ptr<MediaCheck>& check, const QStringList& needed, bool wait);
    std::set<std::string> recovering_; // Reopened after their file came back; their last frame stands in.
    bool recoveringDrawn_ = false; // The last composed picture polled a recovering clip.
    QSet<QString> missingSeen_; // Files seen missing since the open video readers started; kept through Stop All.
    void promoteVideoWarning();
    // Closes a video reader on a thread of its own: its worker may be blocked reading a share.
    static void closeVideo(std::unique_ptr<VideoReader> reader);
    bool wasLive_ = false; // A live show was running at the last tick.
    std::map<std::string, QImage> clipFrames_; // Each video clip's last picture.
    std::map<std::string, QString> videoProblems_; // The warning each failed video clip reported.
    // Forgets what earlier checks found (another show, an edit, a stop) and any check still running.
    void resetMediaState();
    QString reportedProblem_; // The missing-media warning last shown, cleared once the files are back.
    static QString mediaWarning(const QStringList& missing);
    // Drops a clip's video reader; it closes on a thread of its own.
    void retireVideo(const std::string& clip);
    bool wasHeld_ = false; // Held since the last media check, without leaving the hold.
    QString armProblem_;
    Frame pendingPosition_ = 0;
    std::map<std::string, std::unique_ptr<VideoReader>> videos_;
    std::shared_ptr<const Show> armed_;
    std::thread midiThread_;
    std::atomic<bool> midiStop_{true}, showModeAtomic_{false}, testMidiInFlight_{false};
    std::atomic<std::uint64_t> testMidiGen_{1};
    struct PendingTestMidi {
        std::vector<unsigned char> bytes;
        std::uint64_t epoch = 0;
        std::uint64_t gen = 0;
        QString owner;
        std::shared_ptr<std::atomic<bool>> ownerCancelled;
    };
    std::mutex testMidiMutex_;
    std::shared_ptr<std::atomic<bool>> inFlightTestCancel_;
    QString inFlightTestOwner_;
    std::deque<PendingTestMidi> testMidiQueue_;
    QElapsedTimer midiWatch_, stall_;
    std::uint64_t heartbeat_ = 0;
    std::set<std::string> videoErrors_;
    QImage lastPicture_;
    Frame lastComposed_ = -1;
    std::vector<Frame> lastClocks_;
    std::deque<Project> undo_, redo_;
    quint64 busyLease_ = 0, nextBusyLease_ = 1, projectRevision_ = 0, historyEpoch_ = 0, showEpoch_ = 0;
    struct PendingTransportCommand {
        std::uint64_t seq = 0;
        Command command;
    };
    std::uint64_t commandsPosted_ = 0, operatorStopSeq_ = 0;
    std::deque<PendingTransportCommand> pendingCommands_;
    std::pair<State, Frame> predictTransport(bool* hasPendingPlay = nullptr) const;
    bool postTransportCommand(Command cmd);
    // False when preparing playback failed; see armFailed_. Without a position, arms at the effective one.
    bool arm(std::optional<Frame> position = std::nullopt);
    void armOrThrow(std::optional<Frame> position);
    void startMidiThread();
    void watchMidi();
    void stopMidiThread();
    bool prepareAudio();
    bool prepareMidi();
    bool openAudio(RtAudio::Api api, unsigned id, QString name, unsigned channels, unsigned buffer);
    bool openMidi(int port);
};
}
