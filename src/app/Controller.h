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
    bool showMode = false, dirty = false, busy = false;
    QString path, status = "Press Play to use your speakers. Choose show equipment in Outputs.", audioName;
    QString playbackProblem;
    // While disarmed after a failed arm, the position Play will arm at (seeks move it).
    Frame position() const { return armFailed_ ? pendingPosition_ : transport.position(); }
    // The operator's next cue: later than the playhead, or at it unless that hold is already holding.
    const Marker* nextCue() const;
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
    // Offline sources and vanished videos used on the timeline.
    QStringList missingMedia() const;
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
    QStringList preflight();
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
    void save(QString target);
    // Saving an opened package or generated media copies files; MainWindow runs that part on a worker.
    bool savingCopiesMedia() const;
    static QString mediaDirectory(const QString& target);
    void commitSave(Project candidate, QString target);
    void report(QString message);
    // Sidecar for crash recovery: next to the show, or in app data while untitled.
    QString autosavePath() const;
    static QString untitledAutosavePath();
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
    QString midiProblem_; // The playback problem MIDI last reported; a reconnect clears only that.
    // The last arm failed: nothing is armed, and Play arms at pendingPosition_ (updated by seeks).
    // armProblem_ is the playback problem it reported; a later successful arm clears only that.
    bool armFailed_ = false;
    bool pausedLive_ = false; // Paused by the operator during playback, not stopped or sought since.
    // Files the show needs that are offline, and those it needs present now (videos, streamed audio).
    void mediaToCheck(QStringList& offline, QStringList& needed) const;
    // Shared with the checking thread, which may outlive the controller.
    struct MediaCheck { std::mutex mutex; QStringList missing; std::atomic<bool> running{false}; };
    std::shared_ptr<MediaCheck> mediaCheck_ = std::make_shared<MediaCheck>();
    QStringList missingNow_; // What the last media check during a show found missing.
    QString missingWarning_; // Its warning, kept with it.
    QStringList lastMissingMedia(const QStringList& offline, const QStringList& needed) const;
    void checkMediaInBackground(const QStringList& needed, bool wait = false);
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
    std::atomic<bool> midiStop_{true};
    QElapsedTimer midiWatch_, stall_;
    std::uint64_t heartbeat_ = 0;
    std::set<std::string> videoErrors_;
    QImage lastPicture_;
    Frame lastComposed_ = -1;
    std::vector<Frame> lastClocks_;
    std::deque<Project> undo_, redo_;
    bool arm(Frame position); // False when preparing playback failed; see armFailed_.
    void armOrThrow(Frame position);
    void startMidiThread();
    void watchMidi();
    void stopMidiThread();
    bool prepareAudio();
    bool prepareMidi();
    bool openAudio(RtAudio::Api api, unsigned id, QString name, unsigned channels, unsigned buffer);
    bool openMidi(int port);
};
}
