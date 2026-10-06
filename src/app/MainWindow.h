#pragma once
#include "app/Controller.h"
#include "app/Timeline.h"
#include "app/ProgramView.h"
#include <QMainWindow>

class QListWidget; class QLabel; class QPushButton; class QComboBox; class QDoubleSpinBox;
class QSpinBox; class QLineEdit; class QCheckBox; class QTextEdit; class QTableWidget;
class QStackedWidget; class QGroupBox; class QScrollArea; class QAction;

namespace sav {
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    Controller& controller() { return c_; }
    void openPath(const QString& path);
protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* object, QEvent* event) override;
private:
    Controller c_;
    Timeline* timeline_ = nullptr;
    ProgramView* preview_ = nullptr;
    ProgramView* output_ = nullptr;
    QWidget* outputWindow_ = nullptr;
    QListWidget *library_ = nullptr, *cues_ = nullptr;
    QLabel *clock_ = nullptr, *state_ = nullptr, *health_ = nullptr, *hint_ = nullptr, *projectLabel_ = nullptr;
    QPushButton *play_ = nullptr, *showButton_ = nullptr, *outputsButton_ = nullptr, *openOutputButton_ = nullptr;
    QAction* outputsAction_ = nullptr;
    QStackedWidget* inspector_ = nullptr;
    QScrollArea* inspectorScroll_ = nullptr;
    QLineEdit *trackName_ = nullptr, *markerName_ = nullptr;
    QDoubleSpinBox *trackGain_ = nullptr, *trackPan_ = nullptr, *clipStart_ = nullptr, *clipDuration_ = nullptr, *clipOffset_ = nullptr,
        *clipGain_ = nullptr, *clipPan_ = nullptr, *clipX_ = nullptr, *clipY_ = nullptr, *clipScale_ = nullptr, *clipOpacity_ = nullptr, *markerTime_ = nullptr;
    QSpinBox *trackLeft_ = nullptr, *trackRight_ = nullptr, *midiChannel_ = nullptr, *midiData1_ = nullptr, *midiData2_ = nullptr;
    QCheckBox *trackMute_ = nullptr, *trackAsync_ = nullptr, *clipLoop_ = nullptr, *markerHold_ = nullptr;
    QComboBox *curveProperty_ = nullptr, *midiType_ = nullptr, *midiTiming_ = nullptr;
    QLineEdit* midiRaw_ = nullptr;
    QTextEdit* clipText_ = nullptr;
    QTableWidget* curveTable_ = nullptr;
    QGroupBox *audioTransform_ = nullptr, *videoTransform_ = nullptr, *trackAudio_ = nullptr;
    bool refreshing_ = false;
    void buildInspector();
    void refresh();
    void updatePlayback();
    void updateInspector();
    void updateCurve();
    void applyTrack();
    void applyClip();
    void applyMarker();
    void applyCurve();
    void importMedia();
    void placeAsset(QString id, int track, Frame time);
    void addText();
    void addMarker();
    void addTrack(bool visual, bool async);
    void deleteSelection();
    void projectSettings();
    void outputSettings();
    void openOutput(int screen);
    void save(bool as = false);
    bool checkDiscard();
    Clip* selectedClip();
    Marker* selectedMarker();
};
}
