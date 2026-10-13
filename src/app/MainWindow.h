#pragma once
#include "app/Controller.h"
#include "app/Timeline.h"
#include "app/ProgramView.h"
#include <QMainWindow>
#include <QElapsedTimer>
#include <optional>
#include <set>

class QListWidget; class QLabel; class QPushButton; class QComboBox; class QDoubleSpinBox;
class QSpinBox; class QLineEdit; class QCheckBox; class QTextEdit; class QTableWidget;
class QStackedWidget; class QGroupBox; class QScrollArea; class QAction;

namespace sav {
class MidiMessageEditor;
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    Controller& controller() { return c_; }
    void openPath(const QString& path);
    // Offers the untitled-show sidecar left by a crash; call once after startup.
    void offerUntitledRecovery();
protected:
    void closeEvent(QCloseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    bool eventFilter(QObject* object, QEvent* event) override;
private:
    Controller c_;
    Timeline* timeline_ = nullptr;
    ProgramView* preview_ = nullptr;
    ProgramView* output_ = nullptr;
    QWidget* outputWindow_ = nullptr;
    QListWidget *library_ = nullptr, *cues_ = nullptr;
    QLabel *clock_ = nullptr, *state_ = nullptr, *next_ = nullptr, *health_ = nullptr, *hint_ = nullptr, *projectLabel_ = nullptr;
    QPushButton *play_ = nullptr, *showButton_ = nullptr, *outputsButton_ = nullptr, *openOutputButton_ = nullptr;
    QAction* outputsAction_ = nullptr;
    QAction* exportPackageAction_ = nullptr;
    QStackedWidget* inspector_ = nullptr;
    QScrollArea* inspectorScroll_ = nullptr;
    QLineEdit *trackName_ = nullptr, *markerName_ = nullptr;
    QDoubleSpinBox *trackGain_ = nullptr, *trackPan_ = nullptr, *clipStart_ = nullptr, *clipDuration_ = nullptr, *clipOffset_ = nullptr,
        *clipGain_ = nullptr, *clipPan_ = nullptr, *clipX_ = nullptr, *clipY_ = nullptr, *clipScale_ = nullptr, *clipOpacity_ = nullptr, *markerTime_ = nullptr;
    QSpinBox *trackLeft_ = nullptr, *trackRight_ = nullptr;
    QCheckBox *trackMute_ = nullptr, *trackAsync_ = nullptr, *clipLoop_ = nullptr, *markerHold_ = nullptr;
    QComboBox *curveProperty_ = nullptr, *midiTiming_ = nullptr, *trackRouting_ = nullptr;
    unsigned routingChannels_ = 0;
    MidiMessageEditor* midiEditor_ = nullptr;
    QPushButton* midiTest_ = nullptr;
    QLabel* midiDestination_ = nullptr;
    QTextEdit* clipText_ = nullptr;
    QTableWidget* curveTable_ = nullptr;
    QGroupBox *audioTransform_ = nullptr, *videoTransform_ = nullptr, *trackAudio_ = nullptr;
    bool refreshing_ = false, outputLost_ = false, inspectorDirty_ = false, automatic_ = false;
    // Fields changed since the inspector last showed the model; automatic commits apply only these.
    std::set<QWidget*> touched_;
    std::optional<Clip> clipboard_;
    QElapsedTimer escape_;
    bool use(QWidget* field) const { return !automatic_ || touched_.contains(field); }
    std::string inspectedClip_, inspectedMarker_;
    int inspectedTrack_ = -1;
    QString outputScreen_;
    void buildInspector();
    void refresh();
    void updatePlayback();
    void updateRoutingPresets();
    void updateInspector();
    void updateCurve();
    void applyTrack();
    void applyClip();
    void applyMarker(bool requireMidi = true);
    void commitInspector();
    void autoApply(QWidget* field);
    void applyCurve();
    void importMedia();
    void importPaths(const QStringList& paths);
    void splitClip();
    // Moves the selected clip or cue by ±1 frame (step ±1) or by step milliseconds.
    void nudge(int step);
    void jumpToCue(bool forward);
    // Finds offline sources by file name below a chosen folder; after opening, asks first.
    void relinkMedia(bool afterOpening = false);
    void placeAsset(QString id, int track, Frame time);
    void addText();
    void addMarker();
    void addTrack(bool visual, bool async);
    void deleteSelection();
    void projectSettings();
    void outputSettings();
    void openOutput(int screen);
    void screensChanged();
    void requestShowMode(bool enabled);
    void save(bool as = false);
    void exportPackage();
    void writePackage(const QString& path);
    bool checkDiscard();
    Clip* selectedClip();
    Marker* selectedMarker();
};
}
