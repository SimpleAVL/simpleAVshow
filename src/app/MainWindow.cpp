#include "app/MainWindow.h"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressDialog>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QShortcut>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <QDrag>
#include <QWindow>

namespace sav {
namespace {
class LibraryList : public QListWidget {
public:
    using QListWidget::QListWidget;
protected:
    void startDrag(Qt::DropActions) override {
        if (!currentItem()) return;
        auto* data = new QMimeData;
        data->setData("application/x-simpleavshow-asset", currentItem()->data(Qt::UserRole).toString().toUtf8());
        auto* drag = new QDrag(this); drag->setMimeData(data); drag->exec(Qt::CopyAction);
    }
};
QDoubleSpinBox* realSpin(double min, double max, double step, QWidget* parent = nullptr) {
    auto* box = new QDoubleSpinBox(parent); box->setRange(min, max); box->setDecimals(3); box->setSingleStep(step); box->setKeyboardTracking(false); return box;
}
QSpinBox* intSpin(int min, int max) { auto* box = new QSpinBox; box->setRange(min, max); box->setKeyboardTracking(false); return box; }
QPushButton* button(QString text, QLayout* layout = nullptr) {
    auto* b = new QPushButton(text); b->setCursor(Qt::PointingHandCursor); b->setFocusPolicy(Qt::NoFocus);
    if (layout) layout->addWidget(b); return b;
}
QLabel* section(QString text) { auto* l = new QLabel(text); l->setObjectName("section"); return l; }
struct ProjectResult { Project project; QString error; };
struct MediaResult { std::vector<MediaAsset> assets; QStringList errors; };
QString midiHex(const std::vector<unsigned char>& bytes) {
    QStringList list; for (auto b : bytes) list << QString("%1").arg(b, 2, 16, QChar('0')).toUpper(); return list.join(' ');
}
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("SimpleAVShow"); resize(1440, 960); setMinimumSize(1100, 720);
    setStyleSheet(R"(
      QMainWindow, QDialog { background: #111720; color: #dee7f3; }
      QWidget { color: #dee7f3; font-family: 'Inter', 'Segoe UI', 'Arial'; font-size: 12px; }
      QLabel#section { color: #8499b5; font-size: 10px; font-weight: 600; letter-spacing: 1px; margin: 7px 0; }
      QPushButton, QToolButton { background: #263448; border: 1px solid #34455d; border-radius: 5px; padding: 7px 12px; }
      QPushButton:hover, QToolButton:hover { background: #354963; }
      QPushButton:checked { background: #8a6131; border-color: #e0b864; }
      QPushButton:disabled { color: #607188; background: #1a2432; }
      QPushButton#primary { background: #5d84cf; border-color: #739aee; color: #fff; font-weight: 600; }
      QPushButton#danger, QPushButton#stopForOutputSettings { background: #3b2630; border-color: #67414e; color: #e3adb3; }
      QLineEdit, QSpinBox, QDoubleSpinBox, QTextEdit, QComboBox { background: #16202c; border: 1px solid #35445b; border-radius: 4px; padding: 5px; selection-background-color: #486ca5; }
      QComboBox:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled { color: #607188; background: #1a2432; border-color: #2d3d51; }
      QComboBox::drop-down { border: none; width: 22px; }
      QListWidget, QTableWidget { background: #141d28; border: none; outline: none; }
      QListWidget::item { padding: 9px 10px; border-bottom: 1px solid #233043; }
      QListWidget::item:selected { background: #2a4263; }
      QHeaderView::section { background: #202d3d; color: #b0c1d6; border: none; padding: 6px; }
      QGroupBox { border: 1px solid #2d3d51; border-radius: 5px; margin-top: 14px; padding-top: 9px; }
      QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 4px; color: #94aaca; }
      QScrollArea { border: none; background: #192331; }
      QTabWidget::pane { border: none; }
      QTabBar::tab { background: #182332; padding: 10px 14px; color: #91a5bf; }
      QTabBar::tab:selected { color: #e2ecfa; background: #24344b; border-bottom: 2px solid #729aec; }
      QSplitter::handle { background: #2a3647; }
      QMenuBar { background: #17212e; padding: 3px; }
      QMenuBar::item:selected, QMenu::item:selected { background: #334d70; }
      QMenu { background: #1c293a; border: 1px solid #3d506b; }
      QStatusBar { background: #192432; color: #96abc5; }
      QScrollBar:horizontal { height: 10px; background: #151e2b; }
      QScrollBar:vertical { width: 10px; background: #151e2b; }
      QScrollBar::handle { background: #3b506c; border-radius: 4px; min-width: 20px; min-height: 20px; }
      QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
      QCheckBox { spacing: 7px; }
    )");
    auto* file = menuBar()->addMenu("File");
    file->addAction("New show", QKeySequence::New, this, [this] { if (c_.canEdit() && checkDiscard()) c_.replace(newProject()); });
    file->addAction("Open…", QKeySequence::Open, this, [this] {
        if (!c_.canEdit() || !checkDiscard()) return;
        const auto path = QFileDialog::getOpenFileName(this, "Open show", {}, "SimpleAVShow (*.savshow *.savshow.autosave)"); if (!path.isEmpty()) openPath(path);
    });
    file->addAction("Save", QKeySequence::Save, this, [this] { save(); });
    file->addAction("Save as…", QKeySequence::SaveAs, this, [this] { save(true); });
    file->addSeparator(); file->addAction("Open demonstration", this, [this] { if (c_.canEdit() && checkDiscard()) { c_.replace(demoProject()); c_.dirty = true; refresh(); } });
    file->addSeparator(); file->addAction("Import media…", QKeySequence("Ctrl+I"), this, &MainWindow::importMedia);
    auto* edit = menuBar()->addMenu("Edit");
    edit->addAction("Undo", QKeySequence::Undo, &c_, &Controller::undo);
    edit->addAction("Redo", QKeySequence::Redo, &c_, &Controller::redo);
    edit->addAction("Delete selected clip / cue", this, &MainWindow::deleteSelection);
    auto* project = menuBar()->addMenu("Project");
    project->setToolTipsVisible(true);
    project->addAction("Project settings…", this, &MainWindow::projectSettings);
    outputsAction_ = project->addAction("Outputs…", this, &MainWindow::outputSettings);
    outputsAction_->setObjectName("outputSettingsAction");
    auto* tracks = project->addMenu("Add track");
    tracks->addAction("Picture layer", this, [this] { addTrack(true, false); });
    tracks->addAction("Audio", this, [this] { addTrack(false, false); });
    tracks->addAction("Async audio", this, [this] { addTrack(false, true); });
    project->addAction("Delete selected track…", this, [this] {
        if (!c_.canEdit()) return;
        const int index = timeline_->selectedTrack;
        if (QMessageBox::question(this, "Delete track", "Delete this track and all its clips?") != QMessageBox::Yes) return;
        c_.edit([=](Project& p) { std::erase_if(p.show.clips, [=](const Clip& clip) { return clip.track == index; });
            p.show.tracks.erase(p.show.tracks.begin() + index); for (auto& clip : p.show.clips) if (clip.track > index) --clip.track; });
    });
    auto* help = menuBar()->addMenu("Help");
    help->addAction("Keyboard and playback rules", this, [this] {
        QMessageBox::information(this, "Operating SimpleAVShow",
            "Space: play / pause / resume a hold (outside text fields).\nEscape: Stop All, including async audio.\nDelete: delete the selected clip or marker.\n\n"
            "Import media, then drag it from Media onto a compatible track.\nDrag a clip to move; drag its edges to trim. Click the ruler to seek.\nUse the inspector to edit routing, cues, transformations, and automation.\n\n"
            "Holds and manual pauses leave running async audio playing.\nA seek inside an active async clip preserves its source position.\nA seek outside that clip stops it. Seeking never sends MIDI.\n\n"
            "Guide and Click outputs start disconnected. Route them in the track inspector.\nShow mode locks editing and timeline seeks. Use Stop All before editing.\n"
            "At the end of the project, playback and async audio stop.\n\nThis is a development build. Validate it on the Windows show machine before a performance.");
    });
    auto* root = new QWidget; auto* vertical = new QVBoxLayout(root); vertical->setContentsMargins(0, 0, 0, 0); vertical->setSpacing(0);
    auto* top = new QWidget; top->setStyleSheet("background: #1b2737;"); auto* bar = new QHBoxLayout(top); bar->setContentsMargins(18, 11, 18, 11);
    auto* brand = new QLabel("SIMPLE<span style='color:#7ea6eb'>AV</span>SHOW"); brand->setStyleSheet("font-size: 17px; font-weight: 600;"); bar->addWidget(brand);
    projectLabel_ = new QLabel; projectLabel_->setStyleSheet("color: #9fb1ca; margin-left: 18px;"); bar->addWidget(projectLabel_); bar->addStretch();
    outputsButton_ = button("Outputs", bar); outputsButton_->setObjectName("outputSettingsButton");
    connect(outputsButton_, &QPushButton::clicked, this, &MainWindow::outputSettings);
    showButton_ = button("Show mode", bar); showButton_->setCheckable(true);
    connect(showButton_, &QPushButton::clicked, this, [this](bool checked) { c_.setShowMode(checked); });
    vertical->addWidget(top);
    auto* split = new QSplitter(Qt::Vertical); vertical->addWidget(split, 1);
    auto* upper = new QSplitter(Qt::Horizontal); split->addWidget(upper);
    auto* libraryPane = new QWidget; auto* libraryLayout = new QVBoxLayout(libraryPane); libraryLayout->setContentsMargins(12, 8, 12, 8);
    auto* tabs = new QTabWidget; libraryLayout->addWidget(tabs, 1);
    library_ = new LibraryList; library_->setDragEnabled(true); tabs->addTab(library_, "Media");
    cues_ = new QListWidget; tabs->addTab(cues_, "Cues");
    connect(library_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        placeAsset(item->data(Qt::UserRole).toString(), timeline_->selectedTrack, c_.position());
    });
    connect(cues_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) { timeline_->selectMarker(item->data(Qt::UserRole).toString()); });
    connect(cues_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        for (const auto& m : c_.project.show.markers) if (m.id == item->data(Qt::UserRole).toString().toStdString()) c_.seek(m.time);
    });
    auto* import = button("+ Import media", libraryLayout); connect(import, &QPushButton::clicked, this, &MainWindow::importMedia);
    auto* libraryHint = new QLabel("Drag media onto a track.\nDouble-click to place at the playhead."); libraryHint->setStyleSheet("color: #7890b0; font-size: 10px;"); libraryLayout->addWidget(libraryHint);
    libraryPane->setMinimumWidth(210); upper->addWidget(libraryPane);
    auto* previewPane = new QWidget; auto* previewLayout = new QVBoxLayout(previewPane); previewLayout->setContentsMargins(18, 8, 18, 14);
    auto* previewTop = new QHBoxLayout; previewTop->addWidget(section("PROGRAM")); previewTop->addStretch();
    openOutputButton_ = button("Open output", previewTop); openOutputButton_->setObjectName("openOutputSettingsButton");
    connect(openOutputButton_, &QPushButton::clicked, this, &MainWindow::outputSettings); previewLayout->addLayout(previewTop);
    preview_ = new ProgramView; previewLayout->addWidget(preview_, 1);
    hint_ = new QLabel("Build a show or open the demonstration from File"); hint_->setWordWrap(true); hint_->setStyleSheet("color: #8fa8c9;"); previewLayout->addWidget(hint_);
    upper->addWidget(previewPane);
    buildInspector(); upper->addWidget(inspectorScroll_); upper->setSizes({250, 780, 320});
    auto* lower = new QWidget; auto* lowerLayout = new QVBoxLayout(lower); lowerLayout->setContentsMargins(0, 0, 0, 0); lowerLayout->setSpacing(0);
    auto* tools = new QWidget; tools->setStyleSheet("background: #202d3e;"); auto* toolLayout = new QHBoxLayout(tools); toolLayout->setContentsMargins(14, 8, 14, 8);
    toolLayout->addWidget(section("TIMELINE"));
    auto* hold = button("+ Hold / MIDI cue", toolLayout); connect(hold, &QPushButton::clicked, this, &MainWindow::addMarker);
    auto* text = button("+ Text", toolLayout); connect(text, &QPushButton::clicked, this, &MainWindow::addText);
    auto* snap = button("Snap", toolLayout); snap->setCheckable(true); snap->setChecked(true);
    toolLayout->addStretch(); toolLayout->addWidget(new QLabel("Zoom"));
    auto* zoom = new QSlider(Qt::Horizontal); zoom->setRange(4, 160); zoom->setValue(22); zoom->setFixedWidth(130); toolLayout->addWidget(zoom);
    lowerLayout->addWidget(tools);
    timeline_ = new Timeline(c_); lowerLayout->addWidget(timeline_, 1); split->addWidget(lower); split->setSizes({430, 420});
    connect(zoom, &QSlider::valueChanged, timeline_, &Timeline::setZoom);
    connect(snap, &QPushButton::clicked, this, [this](bool checked) { timeline_->snapping = checked; });
    connect(timeline_, &Timeline::selectionChanged, this, &MainWindow::updateInspector);
    connect(timeline_, &Timeline::assetDropped, this, &MainWindow::placeAsset);
    auto* transportBar = new QWidget; transportBar->setStyleSheet("background: #1b2737;"); auto* controls = new QHBoxLayout(transportBar); controls->setContentsMargins(18, 12, 18, 12);
    play_ = button("▶  Play", controls); play_->setObjectName("primary"); play_->setMinimumWidth(145); connect(play_, &QPushButton::clicked, &c_, &Controller::toggle);
    auto* stop = button("■  Stop All", controls); stop->setObjectName("danger"); connect(stop, &QPushButton::clicked, &c_, &Controller::stopAll);
    clock_ = new QLabel; clock_->setStyleSheet("font-family: 'Menlo', 'Consolas'; font-size: 23px; letter-spacing: 2px; padding: 0 15px;"); controls->addWidget(clock_);
    state_ = new QLabel; state_->setMinimumWidth(85); controls->addWidget(state_); controls->addStretch();
    health_ = new QLabel; health_->setStyleSheet("color: #92a8c6;"); controls->addWidget(health_); vertical->addWidget(transportBar);
    setCentralWidget(root); statusBar()->setSizeGripEnabled(false);
    connect(&c_, &Controller::changed, this, &MainWindow::refresh);
    connect(&c_, &Controller::tick, this, &MainWindow::updatePlayback);
    connect(&c_, &Controller::message, this, [this](QString message) { statusBar()->showMessage(message); });
    qApp->installEventFilter(this); refresh();
}

void MainWindow::buildInspector() {
    auto* wrapper = new QWidget; auto* layout = new QVBoxLayout(wrapper); layout->setContentsMargins(14, 10, 14, 12); layout->addWidget(section("INSPECTOR"));
    inspector_ = new QStackedWidget; layout->addWidget(inspector_, 1);
    // Track panel.
    auto* trackPage = new QWidget; auto* trackLayout = new QVBoxLayout(trackPage); trackLayout->setContentsMargins(0, 0, 0, 0);
    auto* trackForm = new QFormLayout; trackName_ = new QLineEdit; trackMute_ = new QCheckBox("Mute / hide track");
    trackForm->addRow("Name", trackName_); trackForm->addRow(trackMute_); trackLayout->addLayout(trackForm);
    trackAudio_ = new QGroupBox("Audio and routing"); auto* audio = new QFormLayout(trackAudio_);
    trackAsync_ = new QCheckBox("Async · continues through holds"); audio->addRow(trackAsync_);
    trackGain_ = realSpin(0, 4, .05); trackPan_ = realSpin(-1, 1, .05); trackLeft_ = intSpin(0, 64); trackRight_ = intSpin(0, 64);
    trackLeft_->setSpecialValueText("Off"); trackRight_->setSpecialValueText("Off");
    audio->addRow("Gain", trackGain_); audio->addRow("Pan", trackPan_); audio->addRow("Left → output", trackLeft_); audio->addRow("Right → output", trackRight_);
    auto* routeHint = new QLabel("Outputs are numbered from 1.\nFor mono guide/click, connect one leg.\nUse Outputs to open enough channels."); routeHint->setWordWrap(true); routeHint->setStyleSheet("color: #88a0be; font-size: 10px;"); audio->addRow(routeHint);
    trackLayout->addWidget(trackAudio_); auto* applyTrackButton = button("Apply track", trackLayout); connect(applyTrackButton, &QPushButton::clicked, this, &MainWindow::applyTrack); trackLayout->addStretch(); inspector_->addWidget(trackPage);
    // Clip panel.
    auto* clipPage = new QWidget; auto* clipLayout = new QVBoxLayout(clipPage); clipLayout->setContentsMargins(0, 0, 0, 0);
    auto* clipForm = new QFormLayout; clipStart_ = realSpin(0, 86400, 1); clipDuration_ = realSpin(.001, 86400, 1); clipOffset_ = realSpin(0, 86400, 1);
    clipLoop_ = new QCheckBox("Loop source"); clipForm->addRow("Start (s)", clipStart_); clipForm->addRow("Length (s)", clipDuration_); clipForm->addRow("Source in (s)", clipOffset_); clipForm->addRow(clipLoop_); clipLayout->addLayout(clipForm);
    clipText_ = new QTextEdit; clipText_->setMaximumHeight(100); clipText_->setPlaceholderText("Text on the program output"); clipLayout->addWidget(clipText_);
    audioTransform_ = new QGroupBox("Audio"); auto* audioForm = new QFormLayout(audioTransform_); clipGain_ = realSpin(0, 4, .05); clipPan_ = realSpin(-1, 1, .05);
    audioForm->addRow("Gain", clipGain_); audioForm->addRow("Pan", clipPan_); clipLayout->addWidget(audioTransform_);
    videoTransform_ = new QGroupBox("Picture"); auto* videoForm = new QFormLayout(videoTransform_);
    clipX_ = realSpin(-2, 2, .05); clipY_ = realSpin(-2, 2, .05); clipScale_ = realSpin(.01, 8, .05); clipOpacity_ = realSpin(0, 1, .05);
    videoForm->addRow("X (frame widths)", clipX_); videoForm->addRow("Y (frame heights)", clipY_); videoForm->addRow("Scale", clipScale_); videoForm->addRow("Opacity", clipOpacity_); clipLayout->addWidget(videoTransform_);
    auto* applyClipButton = button("Apply clip", clipLayout); connect(applyClipButton, &QPushButton::clicked, this, &MainWindow::applyClip);
    auto* automation = new QGroupBox("Automation · follows show time"); auto* automationLayout = new QVBoxLayout(automation);
    curveProperty_ = new QComboBox; curveProperty_->addItems({"Volume", "Pan", "Position X", "Position Y", "Scale", "Opacity"}); automationLayout->addWidget(curveProperty_);
    curveTable_ = new QTableWidget(0, 2); curveTable_->setHorizontalHeaderLabels({"Clip time (s)", "Value"}); curveTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    curveTable_->verticalHeader()->hide(); curveTable_->setMinimumHeight(150); automationLayout->addWidget(curveTable_);
    auto* pointButtons = new QHBoxLayout; auto* addPoint = button("+ At playhead", pointButtons); auto* removePoint = button("− Point", pointButtons); automationLayout->addLayout(pointButtons);
    auto* fadeButtons = new QHBoxLayout; auto* fadeIn = button("Fade in", fadeButtons); auto* fadeOut = button("After hold", fadeButtons); automationLayout->addLayout(fadeButtons);
    auto* applyCurveButton = button("Apply curve", automationLayout);
    auto* curveHint = new QLabel("Times are relative to clip start.\nLinear interpolation; held while paused.\nAfter hold: fade out over 4 seconds."); curveHint->setWordWrap(true); curveHint->setStyleSheet("color: #88a0be; font-size: 10px;"); automationLayout->addWidget(curveHint);
    clipLayout->addWidget(automation); clipLayout->addStretch(); inspector_->addWidget(clipPage);
    connect(curveProperty_, &QComboBox::currentIndexChanged, this, [this] { if (!refreshing_) updateCurve(); });
    connect(addPoint, &QPushButton::clicked, this, [this] {
        auto* clip = selectedClip(); if (!clip || !c_.canEdit()) return;
        const Frame local = std::clamp(c_.position() - clip->start, Frame{0}, clip->duration);
        const double value = envelope(*clip, curveProperty_->currentIndex())->at(local, propertyValue(*clip, curveProperty_->currentIndex()));
        const int row = curveTable_->rowCount(); curveTable_->insertRow(row);
        curveTable_->setItem(row, 0, new QTableWidgetItem(QString::number(seconds(local), 'f', 3))); curveTable_->setItem(row, 1, new QTableWidgetItem(QString::number(value, 'f', 3)));
    });
    connect(removePoint, &QPushButton::clicked, this, [this] { if (c_.canEdit() && curveTable_->currentRow() >= 0) curveTable_->removeRow(curveTable_->currentRow()); });
    connect(applyCurveButton, &QPushButton::clicked, this, &MainWindow::applyCurve);
    connect(fadeIn, &QPushButton::clicked, this, [this] {
        auto* clip = selectedClip(); if (!clip || !c_.canEdit()) return; const auto id = clip->id;
        const int property = c_.project.show.tracks[clip->track].visual ? 5 : 0;
        c_.edit([=](Project& p) { for (auto& c : p.show.clips) if (c.id == id) envelope(c, property)->points = {{0, 0}, {std::min(c.duration, frames(2)), propertyValue(c, property)}}; });
        curveProperty_->setCurrentIndex(property); updateCurve();
    });
    connect(fadeOut, &QPushButton::clicked, this, [this] {
        auto* clip = selectedClip(); if (!clip || !c_.canEdit()) return;
        Frame time = clip->duration;
        for (const auto& m : c_.project.show.markers) if (m.hold && m.time >= std::max(c_.position(), clip->start) && m.time < clip->end()) time = std::min(time, m.time - clip->start);
        if (time >= clip->duration) { c_.report("No following hold marker inside this clip"); return; }
        const auto id = clip->id; const int property = c_.project.show.tracks[clip->track].visual ? 5 : 0;
        c_.edit([=](Project& p) { for (auto& c : p.show.clips) if (c.id == id) {
            auto* e = envelope(c, property); const double value = e->at(time, propertyValue(c, property));
            std::erase_if(e->points, [=](const Keyframe& point) { return point.time >= time; });
            e->points.push_back({time, value}); e->points.push_back({std::min(c.duration, time + frames(4)), 0});
        }});
        curveProperty_->setCurrentIndex(property); updateCurve();
    });
    // Cue panel.
    auto* cuePage = new QWidget; auto* cueLayout = new QVBoxLayout(cuePage); cueLayout->setContentsMargins(0, 0, 0, 0);
    auto* cueForm = new QFormLayout; markerName_ = new QLineEdit; markerTime_ = realSpin(0, 86400, 1); markerHold_ = new QCheckBox("Hold · Space to continue");
    cueForm->addRow("Name", markerName_); cueForm->addRow("Time (s)", markerTime_); cueForm->addRow(markerHold_); cueLayout->addLayout(cueForm);
    auto* midiBox = new QGroupBox("MIDI output"); auto* midiForm = new QFormLayout(midiBox);
    midiTiming_ = new QComboBox; midiTiming_->addItems({"On arrival", "On resume"}); midiType_ = new QComboBox;
    midiType_->addItems({"None", "Note On", "Note Off", "Control Change", "Program Change", "Poly pressure", "Channel pressure", "Pitch bend", "Raw MIDI / SysEx"});
    midiChannel_ = intSpin(1, 16); midiData1_ = intSpin(0, 127); midiData2_ = intSpin(0, 127); midiRaw_ = new QLineEdit;
    midiRaw_->setPlaceholderText("F0 7D 01 F7"); midiForm->addRow("When", midiTiming_); midiForm->addRow("Message", midiType_); midiForm->addRow("Channel", midiChannel_);
    midiForm->addRow("Data 1 / low 7 bits", midiData1_); midiForm->addRow("Data 2 / high 7 bits", midiData2_); midiForm->addRow("Hex bytes", midiRaw_);
    auto* midiHint = new QLabel("Raw messages accept MIDI 1.0 bytes,\nincluding SysEx and system messages.\nPrograms are numbered 0–127."); midiHint->setWordWrap(true); midiHint->setStyleSheet("color: #88a0be; font-size: 10px;"); midiForm->addRow(midiHint);
    cueLayout->addWidget(midiBox); auto* applyCueButton = button("Apply cue", cueLayout); connect(applyCueButton, &QPushButton::clicked, this, &MainWindow::applyMarker);
    connect(midiType_, &QComboBox::currentIndexChanged, this, [this](int type) {
        midiChannel_->setEnabled(type > 0 && type < 8); midiData1_->setEnabled(type > 0 && type < 8);
        midiData2_->setEnabled(type > 0 && type < 8 && type != 4 && type != 6); midiRaw_->setEnabled(type == 8);
    });
    cueLayout->addStretch(); inspector_->addWidget(cuePage);
    inspectorScroll_ = new QScrollArea; inspectorScroll_->setWidgetResizable(true); inspectorScroll_->setWidget(wrapper); inspectorScroll_->setMinimumWidth(280);
}

Clip* MainWindow::selectedClip() { for (auto& clip : c_.project.show.clips) if (clip.id == timeline_->selectedClip.toStdString()) return &clip; return nullptr; }
Marker* MainWindow::selectedMarker() { for (auto& marker : c_.project.show.markers) if (marker.id == timeline_->selectedMarker.toStdString()) return &marker; return nullptr; }
void MainWindow::refresh() {
    library_->clear();
    for (const auto& a : c_.project.assets) {
        const QString type = a.image ? "IMAGE" : a.video ? "VIDEO" : "AUDIO";
        auto* item = new QListWidgetItem(a.name + "\n" + type + "  ·  " + QString::number(a.duration, 'f', 1) + " s", library_);
        item->setData(Qt::UserRole, a.id); item->setToolTip(a.path.isEmpty() ? "Generated demonstration media" : a.path);
    }
    cues_->clear(); std::vector<const Marker*> sorted;
    for (const auto& marker : c_.project.show.markers) sorted.push_back(&marker);
    std::stable_sort(sorted.begin(), sorted.end(), [](const Marker* a, const Marker* b) { return a->time < b->time; });
    for (const auto* m : sorted) {
        auto* item = new QListWidgetItem((m->hold ? "Ⅱ  " : "◆  ") + QString::fromStdString(m->name) + "\n" + timecode(m->time, c_.project.fps) + (m->message.empty() ? "" : "  ·  MIDI"), cues_);
        item->setData(Qt::UserRole, QString::fromStdString(m->id));
    }
    projectLabel_->setText(c_.project.title + (c_.dirty ? "  •" : "") + QString("  ·  %1 × %2 / %3 fps").arg(c_.project.width).arg(c_.project.height).arg(c_.project.fps));
    setWindowTitle(c_.project.title + (c_.dirty ? " *" : "") + " — SimpleAVShow");
    showButton_->setChecked(c_.showMode); showButton_->setText(c_.showMode ? "Show mode · locked" : "Show mode");
    timeline_->refresh(); updateInspector(); updatePlayback(); statusBar()->showMessage(c_.status);
}
void MainWindow::updatePlayback() {
    const bool outputsAvailable = !c_.showMode && !c_.busy;
    const QString outputsTip = c_.showMode ? "Leave show mode to open Outputs" : c_.busy ? "Wait for media preparation to finish" :
        "Inspect outputs or choose the program display; stop all playback before changing audio or MIDI connections";
    outputsButton_->setEnabled(outputsAvailable); outputsButton_->setToolTip(outputsTip);
    openOutputButton_->setEnabled(outputsAvailable); openOutputButton_->setToolTip(outputsTip);
    outputsAction_->setEnabled(outputsAvailable); outputsAction_->setToolTip(outputsTip);
    const State state = c_.transport.state(); const bool playing = state == State::Playing;
    clock_->setText(timecode(c_.position(), c_.project.fps));
    const QString label = state == State::Playing ? "PLAYING" : state == State::Held ? "HOLDING" : state == State::Paused ? "PAUSED" : state == State::Ended ? "ENDED" : "STOPPED";
    state_->setText(label); state_->setStyleSheet(state == State::Held ? "color: #e5c47d; font-weight: 600;" : "color: #a0b6d5;");
    play_->setText(playing ? "Ⅱ  Pause" : state == State::Held ? "▶  Continue" : "▶  Play");
    QString async = c_.transport.asyncVoices() ? QString(" · %1 async playing").arg(c_.transport.asyncVoices()) : "";
    health_->setText((c_.device.running() ? QString("%1 ch · 48 kHz").arg(c_.device.channels()) : "Audio not configured") + async +
        (c_.transport.clipped() ? " · CLIPPING" : "") + (c_.device.underruns() ? QString(" · %1 underruns").arg(c_.device.underruns()) : "") +
        (c_.lateFrames ? QString(" · %1 video waits").arg(c_.lateFrames) : ""));
    if (state == State::Held) {
        const int index = c_.transport.heldMarker();
        hint_->setText("HOLD — " + (index >= 0 && static_cast<std::size_t>(index) < c_.project.show.markers.size() ? QString::fromStdString(c_.project.show.markers[index].name) : "") + "  ·  Press Space to continue" + async);
    } else hint_->setText(c_.showMode ? "SHOW MODE  ·  Space: play / pause  ·  Escape: Stop All" : "Space: play / pause  ·  Escape: Stop All  ·  Ruler: seek  ·  Edges: trim");
    inspector_->setEnabled(c_.canEdit(false));
    const auto picture = c_.compose(); preview_->setPicture(picture); if (output_) output_->setPicture(picture);
}
void MainWindow::updateInspector() {
    refreshing_ = true;
    if (auto* clip = selectedClip()) {
        inspector_->setCurrentIndex(1); clipStart_->setValue(seconds(clip->start)); clipDuration_->setValue(seconds(clip->duration)); clipOffset_->setValue(seconds(clip->offset)); clipLoop_->setChecked(clip->loop);
        const bool visual = c_.project.show.tracks[clip->track].visual; audioTransform_->setVisible(!visual); videoTransform_->setVisible(visual); clipText_->setVisible(!clip->text.empty());
        clipText_->setPlainText(QString::fromStdString(clip->text)); clipGain_->setValue(clip->gain); clipPan_->setValue(clip->pan); clipX_->setValue(clip->x); clipY_->setValue(clip->y); clipScale_->setValue(clip->scale); clipOpacity_->setValue(clip->opacity);
        const int property = curveProperty_->currentIndex(); if (visual && property < 2) curveProperty_->setCurrentIndex(5); if (!visual && property >= 2) curveProperty_->setCurrentIndex(0);
        updateCurve();
    } else if (auto* marker = selectedMarker()) {
        inspector_->setCurrentIndex(2); markerName_->setText(QString::fromStdString(marker->name)); markerTime_->setValue(seconds(marker->time)); markerHold_->setChecked(marker->hold);
        midiTiming_->setCurrentIndex(marker->timing == MidiTiming::Arrival ? 0 : 1); midiRaw_->setText(midiHex(marker->message));
        int type = marker->message.empty() ? 0 : 8;
        if (!marker->message.empty() && marker->message[0] < 0xf0) {
            const int status = marker->message[0] & 0xf0;
            type = status == 0x90 ? 1 : status == 0x80 ? 2 : status == 0xb0 ? 3 : status == 0xc0 ? 4 : status == 0xa0 ? 5 : status == 0xd0 ? 6 : 7;
            midiChannel_->setValue((marker->message[0] & 0xf) + 1); midiData1_->setValue(marker->message.size() > 1 ? marker->message[1] : 0); midiData2_->setValue(marker->message.size() > 2 ? marker->message[2] : 0);
        }
        midiType_->setCurrentIndex(type);
    } else {
        inspector_->setCurrentIndex(0); const auto& track = c_.project.show.tracks[timeline_->selectedTrack];
        trackName_->setText(QString::fromStdString(track.name)); trackMute_->setChecked(track.muted); trackAudio_->setVisible(!track.visual);
        trackAsync_->setChecked(track.async); trackGain_->setValue(track.gain); trackPan_->setValue(track.pan); trackLeft_->setValue(track.left + 1); trackRight_->setValue(track.right + 1);
    }
    refreshing_ = false; inspector_->setEnabled(c_.canEdit(false));
}
void MainWindow::updateCurve() {
    curveTable_->setRowCount(0); auto* clip = selectedClip(); if (!clip) return;
    const auto* e = envelope(*clip, curveProperty_->currentIndex());
    for (const auto& point : e->points) {
        const int row = curveTable_->rowCount(); curveTable_->insertRow(row);
        curveTable_->setItem(row, 0, new QTableWidgetItem(QString::number(seconds(point.time), 'f', 3)));
        curveTable_->setItem(row, 1, new QTableWidgetItem(QString::number(point.value, 'f', 3)));
    }
}
void MainWindow::applyTrack() {
    const int index = timeline_->selectedTrack; Track edited = c_.project.show.tracks[index];
    edited.name = trackName_->text().toStdString(); edited.muted = trackMute_->isChecked();
    if (!edited.visual) { edited.async = trackAsync_->isChecked(); edited.gain = trackGain_->value(); edited.pan = trackPan_->value(); edited.left = trackLeft_->value() - 1; edited.right = trackRight_->value() - 1; }
    c_.edit([=](Project& p) { p.show.tracks[index] = edited; });
}
void MainWindow::applyClip() {
    auto* clip = selectedClip(); if (!clip) return; Clip edited = *clip;
    edited.start = frames(clipStart_->value()); edited.duration = frames(clipDuration_->value()); edited.offset = frames(clipOffset_->value()); edited.loop = clipLoop_->isChecked();
    edited.gain = clipGain_->value(); edited.pan = clipPan_->value(); edited.x = clipX_->value(); edited.y = clipY_->value(); edited.scale = clipScale_->value(); edited.opacity = clipOpacity_->value();
    if (!edited.text.empty()) edited.text = clipText_->toPlainText().toStdString();
    c_.edit([=](Project& p) { for (auto& c : p.show.clips) if (c.id == edited.id) c = edited; });
}
void MainWindow::applyMarker() {
    auto* marker = selectedMarker(); if (!marker) return; Marker edited = *marker;
    edited.name = markerName_->text().toStdString(); edited.time = frames(markerTime_->value()); edited.hold = markerHold_->isChecked(); edited.timing = midiTiming_->currentIndex() == 0 ? MidiTiming::Arrival : MidiTiming::Resume;
    edited.message.clear(); const int type = midiType_->currentIndex();
    if (type == 8) {
        const auto bytes = midiRaw_->text().simplified().split(' ', Qt::SkipEmptyParts);
        for (const auto& byte : bytes) {
            bool ok; const unsigned value = byte.toUInt(&ok, 16);
            if (!ok || byte.size() > 2 || value > 255) { c_.report("Enter MIDI bytes as two-digit hexadecimal numbers separated by spaces"); return; }
            edited.message.push_back(static_cast<unsigned char>(value));
        }
    } else if (type > 0) {
        const std::array<int, 8> statuses = {0, 0x90, 0x80, 0xb0, 0xc0, 0xa0, 0xd0, 0xe0};
        edited.message = {static_cast<unsigned char>(statuses[type] | (midiChannel_->value() - 1)), static_cast<unsigned char>(midiData1_->value())};
        if (type != 4 && type != 6) edited.message.push_back(static_cast<unsigned char>(midiData2_->value()));
    }
    if (!validMidi(edited.message)) { c_.report("That is not a complete valid MIDI 1.0 message"); return; }
    c_.edit([=](Project& p) { for (auto& m : p.show.markers) if (m.id == edited.id) m = edited; });
}
void MainWindow::applyCurve() {
    const auto* clip = selectedClip(); if (!clip) return; const auto id = clip->id; const int property = curveProperty_->currentIndex(); Envelope curve;
    for (int row = 0; row < curveTable_->rowCount(); ++row) {
        bool timeOk = false, valueOk = false;
        const double time = curveTable_->item(row, 0) ? curveTable_->item(row, 0)->text().toDouble(&timeOk) : -1;
        const double value = curveTable_->item(row, 1) ? curveTable_->item(row, 1)->text().toDouble(&valueOk) : 0;
        if (!timeOk || !valueOk || !std::isfinite(time) || time < 0 || time > 86400 || !std::isfinite(value)) { c_.report("Each keyframe needs a valid nonnegative time and numeric value"); return; }
        curve.points.push_back({frames(time), value});
    }
    std::sort(curve.points.begin(), curve.points.end(), [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    c_.edit([=](Project& p) { for (auto& c : p.show.clips) if (c.id == id) *envelope(c, property) = curve; });
}

void MainWindow::importMedia() {
    if (!c_.canEdit()) return;
    const QStringList paths = QFileDialog::getOpenFileNames(this, "Import media", {}, "Media (*.wav *.aiff *.aif *.flac *.mp3 *.ogg *.m4a *.mp4 *.mov *.mkv *.webm *.avi *.png *.jpg *.jpeg *.bmp *.webp);;All files (*)");
    if (paths.isEmpty()) return;
    std::size_t budget = audioBudget; for (const auto& a : c_.project.assets) if (a.audio) budget -= a.audio->samples.size() * sizeof(float);
    c_.busy = true;
    auto* progress = new QProgressDialog("Decoding and preparing media…", QString(), 0, 0, this); progress->setWindowModality(Qt::WindowModal); progress->setMinimumDuration(0); progress->show();
    auto* watcher = new QFutureWatcher<MediaResult>(this);
    connect(watcher, &QFutureWatcher<MediaResult>::finished, this, [this, watcher, progress] {
        const auto result = watcher->result(); c_.busy = false; progress->deleteLater(); watcher->deleteLater();
        c_.edit([&](Project& p) { for (const auto& a : result.assets) { if (p.assets.size() >= 256) throw std::runtime_error("Project exceeds 256 media assets"); p.assets.insert(a.id, a); } });
        if (!result.errors.isEmpty()) QMessageBox::warning(this, "Some media could not be imported", result.errors.join("\n\n"));
        else c_.report(QString("Imported %1 media files. Drag them onto the timeline.").arg(result.assets.size()));
    });
    watcher->setFuture(QtConcurrent::run([paths, budget]() mutable {
        MediaResult result;
        for (const auto& path : paths) {
            try { auto a = loadMedia(path, budget); if (a.audio) budget -= a.audio->samples.size() * sizeof(float); result.assets.push_back(std::move(a)); }
            catch (const std::exception& e) { result.errors << QFileInfo(path).fileName() + ": " + QString::fromUtf8(e.what()); }
        }
        return result;
    }));
}
void MainWindow::placeAsset(QString id, int track, Frame time) {
    if (!c_.canEdit() || !c_.project.assets.contains(id)) return;
    const auto& asset = c_.project.assets[id]; const bool visual = c_.project.show.tracks[track].visual;
    if ((visual && !asset.image && !asset.video) || (!visual && !asset.audio)) { c_.report("Choose a compatible picture or audio track"); return; }
    Clip clip; clip.id = newId().toStdString(); clip.assetId = id.toStdString(); clip.track = track; clip.start = time;
    clip.duration = frames(asset.duration); if (clip.start + clip.duration + frames(5) > frames(86400)) { c_.report("Media extends beyond the 24-hour project limit"); return; }
    if (c_.edit([=](Project& p) { p.show.duration = std::max(p.show.duration, clip.end() + frames(5)); p.show.clips.push_back(clip);
        if (visual && asset.video && asset.audio) {
            auto audioTrack = std::find_if(p.show.tracks.begin(), p.show.tracks.end(), [](const Track& t) { return !t.visual && !t.async; });
            if (audioTrack != p.show.tracks.end()) { Clip sound = clip; sound.id = newId().toStdString(); sound.track = static_cast<int>(audioTrack - p.show.tracks.begin()); p.show.clips.push_back(sound); }
        }
    })) timeline_->selectClip(QString::fromStdString(clip.id));
}
void MainWindow::addText() {
    if (!c_.canEdit()) return; int track = timeline_->selectedTrack;
    if (!c_.project.show.tracks[track].visual) {
        auto it = std::find_if(c_.project.show.tracks.begin(), c_.project.show.tracks.end(), [](const Track& t) { return t.visual; });
        if (it == c_.project.show.tracks.end()) { c_.report("Add a picture track first"); return; } track = static_cast<int>(it - c_.project.show.tracks.begin());
    }
    Clip clip; clip.id = newId().toStdString(); clip.track = track; clip.text = "Your text"; clip.start = c_.position(); clip.duration = frames(10);
    if (c_.edit([=](Project& p) { p.show.duration = std::max(p.show.duration, clip.end() + frames(5)); p.show.clips.push_back(clip); })) timeline_->selectClip(QString::fromStdString(clip.id));
}
void MainWindow::addMarker() {
    Marker marker; marker.id = newId().toStdString(); marker.name = "Hold " + std::to_string(c_.project.show.markers.size() + 1); marker.time = c_.position();
    if (c_.edit([=](Project& p) { p.show.markers.push_back(marker); })) timeline_->selectMarker(QString::fromStdString(marker.id));
}
void MainWindow::addTrack(bool visual, bool async) {
    Track track; track.id = newId().toStdString(); track.name = visual ? "Picture layer" : async ? "Async audio" : "Audio"; track.visual = visual; track.async = async;
    if (c_.edit([=](Project& p) { p.show.tracks.push_back(track); })) { timeline_->selectedTrack = static_cast<int>(c_.project.show.tracks.size()) - 1; timeline_->selectedClip.clear(); timeline_->selectedMarker.clear(); updateInspector(); }
}
void MainWindow::deleteSelection() {
    if (auto* clip = selectedClip()) { const auto id = clip->id; c_.edit([=](Project& p) { std::erase_if(p.show.clips, [=](const Clip& c) { return c.id == id; }); }); }
    else if (auto* marker = selectedMarker()) { const auto id = marker->id; c_.edit([=](Project& p) { std::erase_if(p.show.markers, [=](const Marker& m) { return m.id == id; }); }); }
}

void MainWindow::projectSettings() {
    if (!c_.canEdit()) return;
    QDialog dialog(this); dialog.setWindowTitle("Project settings"); auto* layout = new QVBoxLayout(&dialog); auto* form = new QFormLayout;
    auto* title = new QLineEdit(c_.project.title); auto* width = intSpin(320, 7680); width->setValue(c_.project.width); auto* height = intSpin(240, 4320); height->setValue(c_.project.height);
    auto* fps = realSpin(1, 120, 1); fps->setValue(c_.project.fps); auto* duration = realSpin(.001, 86400, 10); duration->setValue(seconds(c_.project.show.duration));
    form->addRow("Title", title); form->addRow("Width", width); form->addRow("Height", height); form->addRow("Frames / second", fps); form->addRow("Duration (s)", duration);
    form->addRow(new QLabel("Audio engine: 48 kHz. Picture is fitted to the selected display.")); layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) {
        const QString name = title->text(); const int w = width->value(), h = height->value(); const double rate = fps->value(); const Frame length = frames(duration->value());
        c_.edit([=](Project& p) { p.title = name; p.width = w; p.height = h; p.fps = rate; p.show.duration = length; });
    }
}
void MainWindow::outputSettings() {
    if (c_.showMode || c_.busy) { c_.report(c_.showMode ? "Leave show mode to open Outputs" : "Wait for media preparation to finish"); return; }
    QDialog dialog(this); dialog.setObjectName("outputSettingsDialog"); dialog.setWindowTitle("Outputs"); dialog.setMinimumWidth(540);
    auto* layout = new QVBoxLayout(&dialog);
    auto* connectionStatus = new QLabel; connectionStatus->setObjectName("outputConnectionStatus"); connectionStatus->setWordWrap(true);
    layout->addWidget(connectionStatus);
    auto* stopForSettings = button("Stop All to change connections", layout); stopForSettings->setObjectName("stopForOutputSettings");
    QSettings preferences;
    auto* form = new QFormLayout; auto* api = new QComboBox; auto* devices = new QComboBox; auto* channels = intSpin(1, 64); channels->setValue(2);
    auto* buffer = new QComboBox; for (int n : {128, 256, 512, 1024, 2048}) buffer->addItem(QString::number(n) + " samples", n); buffer->setCurrentIndex(2);
    const auto apis = c_.device.apis(); for (const auto& [id, name] : apis) api->addItem(name, static_cast<int>(id));
    const int previousApi = api->findData(preferences.value("audio/api", static_cast<int>(RtAudio::UNSPECIFIED)).toInt());
    if (previousApi >= 0) api->setCurrentIndex(previousApi);
    auto refreshDevices = [&] {
        devices->clear(); const auto selected = static_cast<RtAudio::Api>(api->currentData().toInt());
        // Inspecting a live/held show must not probe and load audio drivers.
        if (!c_.canEdit(false)) {
            devices->addItem(c_.audioName.isEmpty() ? "Audio not configured" : c_.audioName);
            devices->setItemData(0, c_.device.channels(), Qt::UserRole + 1);
            channels->setMaximum(std::max(1, static_cast<int>(c_.device.channels())));
            return;
        }
        try {
            const auto list = c_.device.devices(selected); int defaultIndex = 0;
            for (const auto& d : list) { devices->addItem(QString("%1 (%2 outputs)").arg(d.name).arg(d.channels), d.id); devices->setItemData(devices->count() - 1, d.channels, Qt::UserRole + 1);
                devices->setItemData(devices->count() - 1, d.name, Qt::UserRole + 2); if (d.isDefault) defaultIndex = devices->count() - 1; }
            const QString savedName = preferences.value("audio/device").toString();
            for (int i = 0; i < devices->count(); ++i) if (devices->itemData(i, Qt::UserRole + 2).toString() == savedName) defaultIndex = i;
            devices->setCurrentIndex(defaultIndex);
        } catch (const std::exception& error) { c_.report(QString::fromUtf8(error.what())); }
        channels->setMaximum(std::max(1, std::min(64, devices->currentData(Qt::UserRole + 1).toInt())));
    };
    connect(api, &QComboBox::currentIndexChanged, &dialog, refreshDevices);
    connect(devices, &QComboBox::currentIndexChanged, &dialog, [&] { channels->setMaximum(std::max(1, std::min(64, devices->currentData(Qt::UserRole + 1).toInt()))); }); refreshDevices();
    channels->setValue(preferences.value("audio/channels", 2).toInt());
    const int savedBuffer = buffer->findData(preferences.value("audio/buffer", 512).toInt()); if (savedBuffer >= 0) buffer->setCurrentIndex(savedBuffer);
    form->addRow("Audio driver", api); form->addRow("Single output device", devices); form->addRow("Open output channels", channels); form->addRow("Buffer size", buffer);
    auto* audioApply = button("Connect audio device"); audioApply->setObjectName("connectAudioOutput"); form->addRow(audioApply);
    connect(audioApply, &QPushButton::clicked, &dialog, [&] {
        if (devices->currentIndex() < 0) { QMessageBox::warning(&dialog, "No device", "No audio output device is available"); return; }
        const bool ok = c_.configureAudio(static_cast<RtAudio::Api>(api->currentData().toInt()), devices->currentData().toUInt(), devices->currentData(Qt::UserRole + 2).toString(), channels->value(), buffer->currentData().toUInt());
        if (!ok) QMessageBox::warning(&dialog, "Audio connection failed", c_.status);
    });
    auto* routing = new QLabel("Track routing is edited in the track inspector.\nGuide and Click start disconnected. For one interface with four outputs,\nuse Main 1/2, Guide 3/Off, Click 4/Off. Windows: prefer the interface's ASIO driver."); routing->setWordWrap(true); form->addRow(routing);
    auto* midiPort = new QComboBox; midiPort->addItem("Disconnected", -1); const auto ports = c_.midi.ports(); for (int i = 0; i < ports.size(); ++i) midiPort->addItem(ports[i], i);
    const int savedPort = midiPort->findText(preferences.value("midi/port").toString()); if (savedPort >= 0) midiPort->setCurrentIndex(savedPort);
    form->addRow("Native MIDI output", midiPort); auto* midiConnect = button("Connect MIDI output"); midiConnect->setObjectName("connectMidiOutput"); form->addRow(midiConnect);
    connect(midiConnect, &QPushButton::clicked, &dialog, [&] { if (!c_.configureMidi(midiPort->currentData().toInt())) QMessageBox::warning(&dialog, "MIDI connection failed", c_.status); });
    auto* displays = new QComboBox; const auto screens = QGuiApplication::screens();
    for (int i = 0; i < screens.size(); ++i) displays->addItem(QString("%1 · %2 × %3%4").arg(screens[i]->name()).arg(screens[i]->size().width()).arg(screens[i]->size().height()).arg(screens[i] == screen() ? " (editor)" : ""), i);
    form->addRow("Program display", displays); auto* displayOpen = button("Open fullscreen program output"); displayOpen->setObjectName("openProgramDisplay"); form->addRow(displayOpen);
    connect(displayOpen, &QPushButton::clicked, &dialog, [&] { openOutput(displays->currentData().toInt()); });
    auto* displayClose = button("Close program output"); form->addRow(displayClose); connect(displayClose, &QPushButton::clicked, &dialog, [this] { if (outputWindow_) outputWindow_->hide(); });
    layout->addLayout(form); auto* close = new QDialogButtonBox(QDialogButtonBox::Close); layout->addWidget(close); connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    bool wasEditable = c_.canEdit(false);
    auto updateConnections = [&] {
        if (c_.showMode || c_.busy) { dialog.reject(); return; }
        const bool editable = c_.canEdit(false);
        if (editable && !wasEditable) {
            const int requestedChannels = preferences.value("audio/channels", 2).toInt();
            refreshDevices(); channels->setValue(requestedChannels);
        }
        wasEditable = editable;
        const QString reason = editable ? QString() : "Use Stop All before changing audio or MIDI connections; this silences any running async audio.";
        for (QWidget* control : std::initializer_list<QWidget*>{api, devices, channels, buffer, midiPort, midiConnect}) {
            control->setEnabled(editable); control->setToolTip(reason);
        }
        audioApply->setEnabled(editable && devices->count() > 0);
        audioApply->setToolTip(editable && devices->count() == 0 ? "No audio output device is available" : reason);
        stopForSettings->setVisible(!editable);
        const QString state = c_.transport.state() == State::Held ? "Held at a stop marker." : c_.transport.state() == State::Playing ? "Playback is running." : "Async audio is still running.";
        connectionStatus->setText(editable ? "Audio and MIDI connections can be changed." : state + " Audio and MIDI connections are locked. Use Stop All to silence audio and unlock them.");
        displayOpen->setEnabled(displays->count() > 0);
    };
    connect(&c_, &Controller::tick, &dialog, updateConnections);
    connect(&c_, &Controller::changed, &dialog, updateConnections);
    connect(stopForSettings, &QPushButton::clicked, &dialog, [&] { c_.stopAll(); updateConnections(); });
    updateConnections(); dialog.exec();
}
void MainWindow::openOutput(int index) {
    const auto screens = QGuiApplication::screens(); if (index < 0 || index >= screens.size()) return;
    if (!outputWindow_) {
        outputWindow_ = new QWidget(this, Qt::Window | Qt::FramelessWindowHint); outputWindow_->setWindowTitle("SimpleAVShow — Program"); outputWindow_->setStyleSheet("background: black;");
        auto* layout = new QVBoxLayout(outputWindow_); layout->setContentsMargins(0, 0, 0, 0); output_ = new ProgramView; layout->addWidget(output_);
    }
    output_->setPicture(c_.compose()); outputWindow_->hide(); outputWindow_->winId(); outputWindow_->windowHandle()->setScreen(screens[index]);
    outputWindow_->setGeometry(screens[index]->geometry()); outputWindow_->showFullScreen(); activateWindow();
}
void MainWindow::openPath(const QString& path) {
    if (!c_.canEdit()) return; c_.busy = true;
    auto* progress = new QProgressDialog("Opening project and preparing media…", QString(), 0, 0, this); progress->setWindowModality(Qt::WindowModal); progress->setMinimumDuration(0); progress->show();
    auto* watcher = new QFutureWatcher<ProjectResult>(this);
    connect(watcher, &QFutureWatcher<ProjectResult>::finished, this, [this, watcher, progress, path] {
        const auto result = watcher->result(); c_.busy = false; progress->deleteLater(); watcher->deleteLater();
        if (!result.error.isEmpty()) { QMessageBox::warning(this, "Could not open project", result.error); return; }
        timeline_->selectedClip.clear(); timeline_->selectedMarker.clear(); timeline_->selectedTrack = 0; c_.replace(result.project, path);
    });
    watcher->setFuture(QtConcurrent::run([path] { ProjectResult result; try { result.project = readProject(path); } catch (const std::exception& e) { result.error = QString::fromUtf8(e.what()); } return result; }));
}
void MainWindow::save(bool as) {
    QString target = c_.path;
    if (as || target.isEmpty()) target = QFileDialog::getSaveFileName(this, "Save show", target, "SimpleAVShow (*.savshow)");
    if (target.isEmpty()) return; if (!target.endsWith(".savshow")) target += ".savshow"; c_.save(target);
}
bool MainWindow::checkDiscard() {
    if (!c_.dirty) return true;
    const auto result = QMessageBox::question(this, "Unsaved show", "Save changes to this show?", QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (result == QMessageBox::Cancel) return false; if (result == QMessageBox::Save) { save(); return !c_.dirty; } return true;
}
void MainWindow::closeEvent(QCloseEvent* event) {
    if (c_.busy) { c_.report("Wait for media preparation to finish before closing"); event->ignore(); return; }
    if (!checkDiscard()) { event->ignore(); return; }
    c_.stopAll(); event->accept();
}
bool MainWindow::eventFilter(QObject* object, QEvent* event) {
    if (event->type() != QEvent::KeyPress || !isVisible()) return QMainWindow::eventFilter(object, event);
    auto* key = static_cast<QKeyEvent*>(event); auto* widget = qobject_cast<QWidget*>(object);
    if (!widget || (widget->window() != this && widget->window() != outputWindow_)) return QMainWindow::eventFilter(object, event);
    if (key->key() == Qt::Key_Escape && !key->isAutoRepeat()) { c_.stopAll(); return true; }
    const bool editing = qobject_cast<QLineEdit*>(widget) || qobject_cast<QTextEdit*>(widget) || qobject_cast<QAbstractSpinBox*>(widget) || qobject_cast<QComboBox*>(widget);
    if (key->key() == Qt::Key_Space && !editing && key->modifiers() == Qt::NoModifier) { if (!key->isAutoRepeat()) c_.toggle(); return true; }
    if ((key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) && !editing && !qobject_cast<QTableWidget*>(widget)) { if (!key->isAutoRepeat()) deleteSelection(); return true; }
    return QMainWindow::eventFilter(object, event);
}
}
