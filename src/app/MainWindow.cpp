#include "app/MainWindow.h"
#include "app/MidiMessageEditor.h"
#include <limits>
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
#include <QDirIterator>
#include <QGuiApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
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
#include <QClipboard>
#include <QJsonDocument>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
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
#include <QValidator>
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
// Times display as project timecode like the clock, and accept timecode, MM:SS, or plain seconds.
class TimeSpin : public QDoubleSpinBox {
public:
    TimeSpin(const double& fps, double min) : fps_(fps) {
        setRange(min, maxSeconds); setDecimals(3); setSingleStep(1); setKeyboardTracking(false);
        setToolTip("Timecode HH:MM:SS:FF. You can also type seconds (12.5), MM:SS, or MM:SS:FF.");
    }
    static std::optional<double> parse(QString text, double fps) {
        text = text.trimmed(); const auto parts = text.split(':'); bool ok = true; double total = 0;
        if (parts.size() > 4 || text.isEmpty()) return std::nullopt;
        if (parts.size() >= 3) {
            // The last field counts frames; the rest are [hours:]minutes:seconds.
            const double frame = parts.back().toInt(&ok); if (!ok || frame < 0 || frame >= std::ceil(fps)) return std::nullopt;
            for (int i = 0; i + 1 < parts.size(); ++i) { const int field = parts[i].toInt(&ok); if (!ok || field < 0) return std::nullopt; total = total * 60 + field; }
            return total + frame / fps;
        }
        for (const auto& part : parts) { const double field = part.toDouble(&ok); if (!ok || field < 0) return std::nullopt; total = total * 60 + field; }
        return total;
    }
protected:
    QString textFromValue(double value) const override { return timecode(frames(value), fps_); }
    double valueFromText(const QString& text) const override {
        // Leaving an unedited field must not round a sub-frame time to the displayed frame.
        if (text == textFromValue(value())) return value();
        return parse(text, fps_).value_or(value());
    }
    QValidator::State validate(QString& text, int&) const override {
        for (const QChar c : text) if (!c.isDigit() && c != ':' && c != '.' && !c.isSpace()) return QValidator::Invalid;
        const auto parsed = parse(text, fps_);
        return parsed && *parsed >= minimum() && *parsed <= maximum() ? QValidator::Acceptable : QValidator::Intermediate;
    }
private:
    const double& fps_;
};
QSpinBox* intSpin(int min, int max) { auto* box = new QSpinBox; box->setRange(min, max); box->setKeyboardTracking(false); return box; }
QPushButton* button(QString text, QLayout* layout = nullptr) {
    auto* b = new QPushButton(text); b->setCursor(Qt::PointingHandCursor); b->setFocusPolicy(Qt::TabFocus); // Reachable by Tab; clicks never steal focus from Space handling.
    if (layout) layout->addWidget(b); return b;
}
QLabel* section(QString text) { auto* l = new QLabel(text); l->setObjectName("section"); return l; }
struct ProjectResult { Project project; QString error; };
struct MediaResult { std::vector<MediaAsset> assets; QStringList errors; };
class PackageProgress : public QProgressDialog {
public:
    using QProgressDialog::QProgressDialog;
    // Keep the modal wait alive until the worker completes, including Escape/window close.
    void reject() override {}
};
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), mcp_(c_, this) {
    connect(&mcp_, &McpServer::aboutToEdit, this, &MainWindow::commitInspector);
    setWindowTitle("SimpleAVShow"); resize(1440, 960); setMinimumSize(1100, 720);
    setStyleSheet(R"(
      QMainWindow, QDialog { background: #111720; color: #dee7f3; }
      QWidget { color: #dee7f3; font-family: 'Segoe UI', 'Helvetica Neue', 'Arial'; font-size: 12px; }
      QLabel#section { color: #8499b5; font-size: 11px; font-weight: 600; letter-spacing: 1px; margin: 7px 0; }
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
      QWidget#inspectorContent, QWidget#outputSettingsContent { background: #192331; }
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
    file->setToolTipsVisible(true);
    file->addAction("New show", QKeySequence::New, this, [this] { if (c_.canEdit() && checkDiscard()) c_.replace(newProject()); });
    file->addAction("Open…", QKeySequence::Open, this, [this] {
        if (!c_.canEdit() || !checkDiscard()) return;
        const auto path = QFileDialog::getOpenFileName(this, "Open show", {}, "SimpleAVShow (*.savshow *.savshow.autosave *.savpack);;Show packages (*.savpack)"); if (!path.isEmpty()) openPath(path);
    });
    file->addAction("Save", QKeySequence::Save, this, [this] { save(); });
    file->addAction("Save as…", QKeySequence::SaveAs, this, [this] { save(true); });
    exportPackageAction_ = file->addAction("Export package…", this, &MainWindow::exportPackage);
    exportPackageAction_->setObjectName("exportPackageAction");
    exportPackageAction_->setToolTip("Copy this show and all media library items into a portable .savpack file");
    file->addSeparator(); file->addAction("Open demonstration", this, [this] { if (c_.canEdit() && checkDiscard()) { c_.replace(demoProject()); c_.dirty = true; refresh(); } });
    file->addSeparator(); file->addAction("Import media…", QKeySequence("Ctrl+I"), this, &MainWindow::importMedia);
    file->addAction("Relink missing media…", this, [this] { relinkMedia(); });
    auto* edit = menuBar()->addMenu("Edit");
    edit->addAction("Undo", QKeySequence::Undo, &c_, &Controller::undo);
    edit->addAction("Redo", QKeySequence::Redo, &c_, &Controller::redo);
    edit->addAction("Delete selected clip / cue", this, &MainWindow::deleteSelection);
    edit->addSeparator();
    edit->addAction("Copy clip", QKeySequence::Copy, this, [this] {
        if (const auto* clip = selectedClip()) { clipboard_ = *clip; c_.report("Copied clip. Paste places it at the playhead on the selected track."); }
    });
    edit->addAction("Paste clip at playhead", QKeySequence::Paste, this, [this] {
        if (!clipboard_ || !c_.canEdit()) return;
        Clip clip = *clipboard_; clip.id = newId().toStdString(); clip.link.clear(); clip.track = timeline_->selectedTrack; clip.start = c_.position();
        if (c_.project.show.tracks[clip.track].visual != c_.project.show.tracks[clipboard_->track].visual) { c_.report("Choose a compatible picture or audio track before pasting"); return; }
        if (c_.edit([=](Project& p) { p.show.clips.push_back(clip); })) timeline_->selectClip(QString::fromStdString(clip.id));
    });
    edit->addAction("Duplicate clip", QKeySequence("Ctrl+D"), this, [this] {
        const auto* clip = selectedClip(); if (!clip || !c_.canEdit()) return;
        Clip copy = *clip; copy.id = newId().toStdString(); copy.link.clear(); copy.start = clip->end();
        if (c_.edit([=](Project& p) { p.show.clips.push_back(copy); })) timeline_->selectClip(QString::fromStdString(copy.id));
    });
    edit->addAction("Split clip at playhead", QKeySequence("Ctrl+B"), this, &MainWindow::splitClip);
    edit->addAction("Unlink clip", this, [this] {
        const auto* clip = selectedClip(); if (!clip || clip->link.empty() || !c_.canEdit()) { if (clip && clip->link.empty()) c_.report("This clip is not linked"); return; }
        const auto link = clip->link;
        if (c_.edit([=](Project& p) { for (auto& c : p.show.clips) if (c.link == link) c.link.clear(); })) c_.report("Picture and sound now move independently");
    });
    for (const auto& [label, keys, step] : std::initializer_list<std::tuple<const char*, const char*, int>>{
            {"Nudge left one frame", "Alt+Left", -1}, {"Nudge right one frame", "Alt+Right", 1},
            {"Nudge left one second", "Alt+Shift+Left", -1000}, {"Nudge right one second", "Alt+Shift+Right", 1000}})
        edit->addAction(label, QKeySequence(keys), this, [this, step] { nudge(step); });
    auto* playback = menuBar()->addMenu("Playback");
    playback->addAction("Previous cue", QKeySequence("Ctrl+Left"), this, [this] { jumpToCue(false); });
    playback->addAction("Next cue", QKeySequence("Ctrl+Right"), this, [this] { jumpToCue(true); });
    auto* project = menuBar()->addMenu("Project");
    project->setToolTipsVisible(true);
    project->addAction("Project settings…", this, &MainWindow::projectSettings);
    auto* agentAction = project->addAction("Agent collaboration…", this, &MainWindow::agentSettings);
    agentAction->setObjectName("agentCollaborationAction");
    outputsAction_ = project->addAction("Outputs…", this, &MainWindow::outputSettings);
    outputsAction_->setObjectName("outputSettingsAction");
    auto* tracks = project->addMenu("Add track");
    tracks->addAction("Picture layer", this, [this] { addTrack(true, false); });
    tracks->addAction("Audio", this, [this] { addTrack(false, false); });
    tracks->addAction("Async audio", this, [this] { addTrack(false, true); });
    project->addAction("Delete selected track…", this, [this] {
        if (!c_.canEdit()) return;
        const int index = timeline_->selectedTrack;
        const auto clips = std::count_if(c_.project.show.clips.begin(), c_.project.show.clips.end(), [=](const Clip& clip) { return clip.track == index; });
        if (QMessageBox::question(this, "Delete track", QString("Delete the track “%1” and its %2 clip%3?").arg(QString::fromStdString(c_.project.show.tracks[index].name))
                .arg(clips).arg(clips == 1 ? "" : "s"), QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
        c_.edit([=](Project& p) { std::erase_if(p.show.clips, [=](const Clip& clip) { return clip.track == index; });
            p.show.tracks.erase(p.show.tracks.begin() + index); for (auto& clip : p.show.clips) if (clip.track > index) --clip.track; });
    });
    auto* help = menuBar()->addMenu("Help");
    help->addAction("About SimpleAVShow", this, [this] {
        QMessageBox::about(this, "About SimpleAVShow", QString(
            "<b>SimpleAVShow %1</b><p>Copyright © 2026 the SimpleAVShow contributors.</p>"
            "<p>This program is free software: you can redistribute it and/or modify it under the terms of the "
            "GNU General Public License as published by the Free Software Foundation, either version 3 of the License, "
            "or (at your option) any later version.</p>"
            "<p>This program is distributed in the hope that it will be useful, but <b>without any warranty</b>; without even "
            "the implied warranty of merchantability or fitness for a particular purpose. See the LICENSE file included "
            "with this program or <a href='https://www.gnu.org/licenses/gpl-3.0.html'>gnu.org/licenses</a>.</p>"
            "<p>Source code: <a href='https://github.com/staubichsauger/simpleAVshow'>github.com/staubichsauger/simpleAVshow</a>. "
            "Third-party notices are in the bundled licenses folder.</p>").arg(QCoreApplication::applicationVersion()));
    });
    help->addAction("Keyboard and playback rules", this, [this] {
        QMessageBox::information(this, "Operating SimpleAVShow",
            "Space: play / pause / resume a hold (outside text fields).\nEscape: Stop All, including async audio (press twice in Show mode).\nDelete: delete the selected clip or marker.\n\n"
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
    connect(showButton_, &QPushButton::clicked, this, &MainWindow::requestShowMode);
    agentButton_ = button("Agent · off", bar);
    agentButton_->setObjectName("agentCollaborationButton");
    connect(agentButton_, &QPushButton::clicked, this, &MainWindow::agentSettings);
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
    auto* libraryHint = new QLabel("Drag media onto a track.\nDouble-click to place at the playhead."); libraryHint->setStyleSheet("color: #7890b0; font-size: 11px;"); libraryLayout->addWidget(libraryHint);
    libraryPane->setMinimumWidth(210); upper->addWidget(libraryPane);
    auto* previewPane = new QWidget; auto* previewLayout = new QVBoxLayout(previewPane); previewLayout->setContentsMargins(18, 8, 18, 14);
    auto* previewTop = new QHBoxLayout; previewTop->addWidget(section("PROGRAM")); previewTop->addStretch();
    openOutputButton_ = button("Audience display…", previewTop); openOutputButton_->setObjectName("openOutputSettingsButton");
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
    auto* fit = button("Fit", toolLayout); fit->setToolTip("Fit the whole show into the timeline (Ctrl+wheel zooms at the cursor)");
    auto* follow = button("Follow", toolLayout); follow->setCheckable(true); follow->setChecked(true); follow->setToolTip("Keep the playhead in view while playing");
    lowerLayout->addWidget(tools);
    timeline_ = new Timeline(c_); lowerLayout->addWidget(timeline_, 1); split->addWidget(lower); split->setSizes({430, 420});
    connect(zoom, &QSlider::valueChanged, timeline_, &Timeline::setZoom);
    // Blocking keeps a fitted zoom exact instead of re-applying the slider's rounded value.
    connect(timeline_, &Timeline::zoomChanged, zoom, [zoom](double value) { QSignalBlocker block(zoom); zoom->setValue(qRound(value)); });
    connect(fit, &QPushButton::clicked, timeline_, &Timeline::zoomToFit);
    connect(follow, &QPushButton::toggled, this, [this](bool on) { timeline_->follow = on; });
    connect(snap, &QPushButton::clicked, this, [this](bool checked) { timeline_->snapping = checked; });
    connect(timeline_, &Timeline::selectionChanged, this, &MainWindow::updateInspector);
    connect(timeline_, &Timeline::assetDropped, this, &MainWindow::placeAsset);
    auto* transportBar = new QWidget; transportBar->setStyleSheet("background: #1b2737;"); auto* controls = new QHBoxLayout(transportBar); controls->setContentsMargins(18, 12, 18, 12);
    play_ = button("▶  Play", controls); play_->setObjectName("primary"); play_->setMinimumWidth(145); connect(play_, &QPushButton::clicked, &c_, &Controller::toggle);
    // Keep Stop All away from Play/Continue so a hurried press cannot hit it.
    controls->addSpacing(28);
    auto* stop = button("■  Stop All", controls); stop->setObjectName("danger"); connect(stop, &QPushButton::clicked, &c_, &Controller::stopAll);
    clock_ = new QLabel; clock_->setStyleSheet("font-family: 'Menlo', 'Consolas'; font-size: 23px; letter-spacing: 2px; padding: 0 15px;"); controls->addWidget(clock_);
    state_ = new QLabel; state_->setMinimumWidth(85); controls->addWidget(state_);
    next_ = new QLabel; next_->setObjectName("nextCue"); next_->setStyleSheet("color: #c9d6e8; font-size: 13px; padding-left: 12px;"); controls->addWidget(next_); controls->addStretch();
    health_ = new QLabel; health_->setStyleSheet("color: #92a8c6;"); controls->addWidget(health_); vertical->addWidget(transportBar);
    setCentralWidget(root); statusBar()->setSizeGripEnabled(false);
    connect(&c_, &Controller::changed, this, &MainWindow::refresh);
    connect(&c_, &Controller::tick, this, &MainWindow::updatePlayback);
    connect(&c_, &Controller::message, this, [this](QString message) { statusBar()->showMessage(message); });
    connect(qApp, &QGuiApplication::screenRemoved, this, &MainWindow::screensChanged, Qt::QueuedConnection);
    connect(qApp, &QGuiApplication::screenAdded, this, &MainWindow::screensChanged, Qt::QueuedConnection);
    outputScreen_ = QSettings().value("display/screen").toString();
    setAcceptDrops(true);
    qApp->installEventFilter(this); refresh();
}

void MainWindow::buildInspector() {
    auto* wrapper = new QWidget; wrapper->setObjectName("inspectorContent"); auto* layout = new QVBoxLayout(wrapper); layout->setContentsMargins(14, 10, 14, 12); layout->addWidget(section("INSPECTOR"));
    inspector_ = new QStackedWidget; layout->addWidget(inspector_, 1);
    // Track panel.
    auto* trackPage = new QWidget; auto* trackLayout = new QVBoxLayout(trackPage); trackLayout->setContentsMargins(0, 0, 0, 0);
    auto* trackForm = new QFormLayout; trackName_ = new QLineEdit; trackMute_ = new QCheckBox("Mute / hide track");
    trackForm->addRow("Name", trackName_); trackForm->addRow(trackMute_); trackLayout->addLayout(trackForm);
    trackAudio_ = new QGroupBox("Audio and routing"); auto* audio = new QFormLayout(trackAudio_);
    trackAsync_ = new QCheckBox("Keep playing through pauses / holds"); audio->addRow(trackAsync_);
    trackGain_ = realSpin(0, 4, .05); trackPan_ = realSpin(-1, 1, .05); trackLeft_ = intSpin(0, std::numeric_limits<int>::max()); trackRight_ = intSpin(0, std::numeric_limits<int>::max());
    trackLeft_->setObjectName("trackLeftOutput"); trackRight_->setObjectName("trackRightOutput");
    trackLeft_->setSpecialValueText("Off"); trackRight_->setSpecialValueText("Off");
    auto* route = trackRouting_ = new QComboBox; route->setObjectName("trackRoutingPreset");
    route->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); route->setMinimumContentsLength(10);
    updateRoutingPresets();
    audio->addRow("Quick routing", route);
    connect(route, &QComboBox::activated, this, [this, route] {
        const int output = route->currentData().toInt();
        if (output == -1) { trackLeft_->setValue(1); trackRight_->setValue(2); trackPan_->setValue(0); }
        else if (output == -3) { trackLeft_->setValue(0); trackRight_->setValue(0); }
        else if (output > 0) { trackLeft_->setValue(output); trackRight_->setValue(0); trackPan_->setValue(0); }
        route->setCurrentIndex(0); if (output != -2) { inspectorDirty_ = true; touched_.insert({trackLeft_, trackRight_, trackPan_}); commitInspector(); }
    });
    trackPan_->setToolTip("−1 = left, 0 = center, +1 = right. With one connected output, stereo sources are summed to mono at full level and pan has no effect.");
    audio->addRow("Volume (1 = full)", trackGain_); audio->addRow("Pan", trackPan_); audio->addRow("Left → output", trackLeft_); audio->addRow("Right → output", trackRight_);
    auto* routeHint = new QLabel("Presets and fields apply immediately. Guide and Click start silent so they do not reach the audience. Use separate interface outputs for them."); routeHint->setWordWrap(true); routeHint->setStyleSheet("color: #88a0be; font-size: 11px;"); audio->addRow(routeHint);
    trackLayout->addWidget(trackAudio_); auto* applyTrackButton = button("Apply track", trackLayout); connect(applyTrackButton, &QPushButton::clicked, this, &MainWindow::applyTrack); trackLayout->addStretch(); inspector_->addWidget(trackPage);
    // Clip panel.
    auto* clipPage = new QWidget; auto* clipLayout = new QVBoxLayout(clipPage); clipLayout->setContentsMargins(0, 0, 0, 0);
    auto* clipForm = new QFormLayout; clipStart_ = new TimeSpin(c_.project.fps, 0); clipDuration_ = new TimeSpin(c_.project.fps, .001); clipOffset_ = new TimeSpin(c_.project.fps, 0); clipStart_->setObjectName("clipStart");
    clipLoop_ = new QCheckBox("Loop source"); clipForm->addRow("Start", clipStart_); clipForm->addRow("Length", clipDuration_); clipForm->addRow("Source in", clipOffset_); clipForm->addRow(clipLoop_); clipLayout->addLayout(clipForm);
    clipText_ = new QTextEdit; clipText_->setMaximumHeight(100); clipText_->setPlaceholderText("Text on the program output"); clipLayout->addWidget(clipText_);
    audioTransform_ = new QGroupBox("Audio"); auto* audioForm = new QFormLayout(audioTransform_); clipGain_ = realSpin(0, 4, .05); clipPan_ = realSpin(-1, 1, .05);
    audioForm->addRow("Gain", clipGain_); audioForm->addRow("Pan", clipPan_); clipLayout->addWidget(audioTransform_);
    videoTransform_ = new QGroupBox("Picture"); auto* videoForm = new QFormLayout(videoTransform_);
    clipX_ = realSpin(-2, 2, .05); clipY_ = realSpin(-2, 2, .05); clipScale_ = realSpin(.01, 8, .05); clipOpacity_ = realSpin(0, 1, .05); clipScale_->setObjectName("clipScale"); clipOpacity_->setObjectName("clipOpacity");
    videoForm->addRow("X (frame widths)", clipX_); videoForm->addRow("Y (frame heights)", clipY_); videoForm->addRow("Scale", clipScale_); videoForm->addRow("Opacity", clipOpacity_); clipLayout->addWidget(videoTransform_);
    auto* applyClipButton = button("Apply clip", clipLayout); connect(applyClipButton, &QPushButton::clicked, this, &MainWindow::applyClip);
    auto* automation = new QGroupBox("Automation · follows show time"); auto* automationLayout = new QVBoxLayout(automation);
    curveProperty_ = new QComboBox; curveProperty_->addItems({"Volume", "Pan", "Position X", "Position Y", "Scale", "Opacity"}); automationLayout->addWidget(curveProperty_);
    curveTable_ = new QTableWidget(0, 2); curveTable_->setHorizontalHeaderLabels({"Clip time (s)", "Value"}); curveTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    curveTable_->verticalHeader()->hide(); curveTable_->setMinimumHeight(150); automationLayout->addWidget(curveTable_);
    auto* pointButtons = new QHBoxLayout; auto* addPoint = button("+ At playhead", pointButtons); auto* removePoint = button("− Point", pointButtons); automationLayout->addLayout(pointButtons);
    auto* fadeButtons = new QHBoxLayout; auto* fadeIn = button("Fade in", fadeButtons); auto* fadeOut = button("After hold", fadeButtons); automationLayout->addLayout(fadeButtons);
    auto* applyCurveButton = button("Apply curve", automationLayout);
    auto* curveHint = new QLabel("Times are relative to clip start.\nLinear interpolation; held while paused.\nAfter hold: fade out over 4 seconds."); curveHint->setWordWrap(true); curveHint->setStyleSheet("color: #88a0be; font-size: 11px;"); automationLayout->addWidget(curveHint);
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
    auto* cueForm = new QFormLayout; markerName_ = new QLineEdit; markerTime_ = new TimeSpin(c_.project.fps, 0); markerHold_ = new QCheckBox("Hold · Space to continue");
    cueForm->addRow("Name", markerName_); cueForm->addRow("Time", markerTime_); cueForm->addRow(markerHold_); cueLayout->addLayout(cueForm);
    auto* midiBox = new QGroupBox("Control another device with MIDI"); auto* midiLayout = new QVBoxLayout(midiBox);
    midiDestination_ = new QLabel; midiDestination_->setWordWrap(true); midiDestination_->setStyleSheet("color: #88a0be; font-size: 11px;"); midiLayout->addWidget(midiDestination_);
    midiTiming_ = new QComboBox; midiTiming_->addItems({"At this cue", "After continuing the hold"});
    midiTiming_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); midiTiming_->setMinimumContentsLength(10);
    midiTiming_->setObjectName("midiCueTiming");
    auto* timingForm = new QFormLayout; timingForm->addRow("Send", midiTiming_); midiLayout->addLayout(timingForm);
    midiEditor_ = new MidiMessageEditor; midiLayout->addWidget(midiEditor_);
    auto* midiButtons = new QHBoxLayout;
    midiTest_ = button("Send test", midiButtons); midiTest_->setObjectName("testMidiMessage");
    midiTest_->setToolTip("Sends the message shown above immediately, without saving the cue or moving the playhead. It can trigger the connected device.");
    connect(midiTest_, &QPushButton::clicked, this, [this] {
        std::vector<unsigned char> bytes; QString error;
        if (!midiEditor_->message(bytes, error)) { c_.report(error); return; }
        if (bytes.empty()) { c_.report("Choose a MIDI action before sending a test message."); return; }
        c_.testMidi(bytes);
    });
    auto* midiSetup = button("Device…", midiButtons); connect(midiSetup, &QPushButton::clicked, this, &MainWindow::outputSettings);
    midiLayout->addLayout(midiButtons);
    cueLayout->addWidget(midiBox); auto* applyCueButton = button("Apply cue", cueLayout); connect(applyCueButton, &QPushButton::clicked, this, [this] { applyMarker(true); });
    connect(markerHold_, &QCheckBox::toggled, this, [this](bool hold) {
        if (!hold) midiTiming_->setCurrentIndex(0);
        midiTiming_->setEnabled(hold); midiTiming_->setToolTip(hold ? "" : "Without a hold, the message is sent when the playhead reaches this cue.");
    });
    cueLayout->addStretch(); inspector_->addWidget(cuePage);
    inspectorScroll_ = new QScrollArea; inspectorScroll_->setWidgetResizable(true); inspectorScroll_->setWidget(wrapper); inspectorScroll_->setMinimumWidth(280);
    // Fields apply when left (Enter or focus change) and toggles at once; each change is one undo step.
    for (QWidget* field : std::initializer_list<QWidget*>{trackName_, trackMute_, trackAsync_, trackGain_, trackPan_, trackLeft_, trackRight_,
            clipStart_, clipDuration_, clipOffset_, clipLoop_, clipText_, clipGain_, clipPan_, clipX_, clipY_, clipScale_, clipOpacity_,
            markerName_, markerTime_, markerHold_, midiTiming_}) autoApply(field);
}

Clip* MainWindow::selectedClip() { for (auto& clip : c_.project.show.clips) if (clip.id == timeline_->selectedClip.toStdString()) return &clip; return nullptr; }
Marker* MainWindow::selectedMarker() { for (auto& marker : c_.project.show.markers) if (marker.id == timeline_->selectedMarker.toStdString()) return &marker; return nullptr; }
void MainWindow::refresh() {
    const int libraryScroll = library_->verticalScrollBar()->value(), cueScroll = cues_->verticalScrollBar()->value();
    library_->clear();
    for (const auto& a : c_.project.assets) {
        const QString type = a.image ? "IMAGE" : a.video ? "VIDEO" : "AUDIO";
        auto* item = new QListWidgetItem(a.name + "\n" + (a.offline ? QString("OFFLINE  ·  relink from File") : type + "  ·  " + QString::number(a.duration, 'f', 1) + " s"), library_);
        item->setData(Qt::UserRole, a.id); item->setToolTip(a.path.isEmpty() ? "Generated demonstration media" : a.offline ? a.path + "\n" + a.problem : a.path);
        if (a.offline) item->setForeground(QColor("#f0a66e"));
    }
    cues_->clear(); std::vector<const Marker*> sorted;
    for (const auto& marker : c_.project.show.markers) sorted.push_back(&marker);
    std::stable_sort(sorted.begin(), sorted.end(), [](const Marker* a, const Marker* b) { return a->time < b->time; });
    for (const auto* m : sorted) {
        auto* item = new QListWidgetItem((m->hold ? "Ⅱ  " : "◆  ") + QString::fromStdString(m->name) + "\n" + timecode(m->time, c_.project.fps) + (m->message.empty() ? "" : "\n" + midiSummary(m->message)), cues_);
        item->setData(Qt::UserRole, QString::fromStdString(m->id));
    }
    library_->verticalScrollBar()->setValue(libraryScroll); cues_->verticalScrollBar()->setValue(cueScroll);
    projectLabel_->setText(c_.project.title + (c_.dirty ? "  •" : "") + QString("  ·  %1 × %2 / %3 fps").arg(c_.project.width).arg(c_.project.height).arg(c_.project.fps));
    setWindowTitle(c_.project.title + (c_.dirty ? " *" : "") + " — SimpleAVShow");
    showButton_->setChecked(c_.showMode); showButton_->setText(c_.showMode ? "Show mode · locked" : "Show mode");
    timeline_->refresh(); updateInspector(); updatePlayback(); statusBar()->showMessage(c_.status);
}
void MainWindow::updatePlayback() {
    agentButton_->setText(!mcp_.running() ? "Agent · off" : !mcp_.editingEnabled() ? "Agent · read only" : c_.canEdit(false) ? "Agent · editing" : "Agent · locked");
    agentButton_->setToolTip("Connect an agent to help build this show through MCP");
    exportPackageAction_->setEnabled(c_.canEdit(false));
    updateRoutingPresets();
    const bool outputsAvailable = !c_.showMode && !c_.busy;
    const QString outputsTip = c_.showMode ? "Leave show mode to open Outputs" : c_.busy ? "Wait for media preparation to finish" :
        "Inspect outputs or choose the program display; stop all playback before changing audio or MIDI connections";
    outputsButton_->setEnabled(outputsAvailable); outputsButton_->setToolTip(outputsTip);
    openOutputButton_->setEnabled(outputsAvailable); openOutputButton_->setToolTip(outputsTip);
    outputsAction_->setEnabled(outputsAvailable); outputsAction_->setToolTip(outputsTip);
    const State state = c_.transport.state(); const bool playing = state == State::Playing;
    clock_->setText(timecode(c_.position(), c_.project.fps));
    const QString label = state == State::Playing ? "PLAYING" : state == State::Held ? "HOLDING" : state == State::Paused ? "PAUSED" : state == State::Ended ? "ENDED" : "STOPPED";
    state_->setText(label);
    state_->setStyleSheet(QString("font-weight: 600; color: %1;").arg(state == State::Playing ? "#7fd19b" : state == State::Held ? "#e5c47d" : state == State::Paused ? "#8fb4f0" : "#7d8ea6"));
    // Operator readout: the next cue and the time left in the show.
    const Frame now = c_.position(); const Marker* upcoming = nullptr;
    for (const auto& m : c_.project.show.markers)
        if ((m.time > now || (m.time == now && state != State::Held)) && (!upcoming || m.time < upcoming->time)) upcoming = &m;
    const auto clockText = [](Frame frames) {
        const auto total = static_cast<qint64>(std::ceil(seconds(std::max<Frame>(0, frames))));
        return total >= 3600 ? QString("%1:%2:%3").arg(total / 3600).arg(total / 60 % 60, 2, 10, QChar('0')).arg(total % 60, 2, 10, QChar('0'))
                             : QString("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QChar('0'));
    };
    next_->setText((upcoming ? QString("NEXT  %1 %2 · in %3").arg(upcoming->hold ? "Ⅱ" : "◆", QString::fromStdString(upcoming->name), clockText(upcoming->time - now)) : QString("No further cues")) +
        "   ·   " + clockText(c_.project.show.duration - now) + " left");
    play_->setText(playing ? "Ⅱ  Pause" : state == State::Held ? "▶  Continue" : "▶  Play");
    QString async = c_.transport.asyncVoices() ? QString(" · %1 async playing").arg(c_.transport.asyncVoices()) : "";
    health_->setText((c_.device.running() ? QString("%1 ch · 48 kHz").arg(c_.device.channels()) : "Audio · connects on Play") + async +
        (c_.needsMidi() ? (c_.midi.connected() ? " · MIDI connected" : c_.midiDestination().isEmpty() ? " · MIDI needs a device" : " · MIDI reconnects on Play") : "") +
        (c_.transport.clipped() ? " · CLIPPING" : "") + (c_.device.underruns() ? QString(" · %1 underruns").arg(c_.device.underruns()) : "") +
        (c_.transport.bufferWaits() ? QString(" · %1 audio waits").arg(c_.transport.bufferWaits()) : "") +
        (c_.transport.readRetries() ? QString(" · %1 disk retries").arg(c_.transport.readRetries()) : "") +
        (c_.lateFrames ? QString(" · %1 video waits").arg(c_.lateFrames) : ""));
    midiDestination_->setText(c_.midi.connected() ? "Destination: " + c_.midi.name() : c_.midiDestination().isEmpty() ? "Choose a MIDI device once; Play reconnects it automatically." : "On Play: reconnect to " + c_.midiDestination());
    health_->setToolTip(c_.device.running() ? "Audio: " + c_.audioName + "\nMIDI: " + (c_.midi.connected() ? c_.midi.name() : c_.needsMidi() ? "Choose a destination in Outputs" : "Not needed for this show") : "Play connects your remembered audio interface, or your default speakers on first use.");
    outputsButton_->setText(c_.needsMidi() && !c_.midi.connected() && c_.midiDestination().isEmpty() ? "Outputs · MIDI needed" : "Outputs");
    if (state == State::Held) {
        const int index = c_.transport.heldMarker();
        hint_->setText("HOLD — " + (index >= 0 && static_cast<std::size_t>(index) < c_.project.show.markers.size() ? QString::fromStdString(c_.project.show.markers[index].name) : "") + "  ·  Press Space to continue" + async);
    } else if (!c_.playbackProblem.isEmpty()) hint_->setText("⚠  " + c_.playbackProblem + (c_.showMode ? " Leave Show mode to change Outputs." : ""));
    else if (c_.project.show.clips.empty() && c_.project.show.markers.empty()) hint_->setText("Start with + Import media or + Text. Press Play to use your speakers automatically.");
    else hint_->setText(c_.showMode ? "SHOW MODE  ·  Space: play / pause  ·  Escape twice: Stop All" : "Space: play / pause  ·  Escape: Stop All  ·  Ruler: seek  ·  Edges: trim");
    const bool problem = state != State::Held && !c_.playbackProblem.isEmpty();
    hint_->setStyleSheet(state == State::Held ? "color: #e5c47d; font-weight: 600;" :
        problem ? "color: #ffd7d2; background: #5a2523; border-radius: 4px; padding: 6px 8px;" : "color: #8fa8c9;");
    health_->setStyleSheet(c_.transport.clipped() || c_.device.underruns() || c_.transport.bufferWaits() || c_.transport.readRetries() || c_.lateFrames ? "color: #f0a66e;" : "color: #92a8c6;");
    inspector_->setEnabled(c_.canEdit(false));
    const auto picture = c_.compose(); preview_->setPicture(picture); if (output_) output_->setPicture(picture);
}
void MainWindow::updateRoutingPresets() {
    // Until an interface is connected, offer common routes. Custom channel
    // fields remain unrestricted; connected interfaces supply the full list.
    const unsigned channels = c_.device.running() ? c_.device.channels() : 4;
    if (channels == routingChannels_) return;
    routingChannels_ = channels;
    trackRouting_->clear();
    trackRouting_->addItem("Choose a routing preset…", -2);
    trackRouting_->addItem("Speakers · stereo outputs 1 + 2", -1);
    trackRouting_->addItem("Silent · no output", -3);
    for (unsigned n = 1; n <= channels; ++n)
        trackRouting_->addItem(QString("Mono · output %1").arg(n), n);
}
void MainWindow::updateInspector() {
    commitInspector();
    refreshing_ = true; inspectorDirty_ = false; touched_.clear();
    inspectedClip_ = selectedClip() ? selectedClip()->id : std::string(); inspectedMarker_ = !selectedClip() && selectedMarker() ? selectedMarker()->id : std::string();
    inspectedTrack_ = timeline_->selectedTrack;
    if (auto* clip = selectedClip()) {
        inspector_->setCurrentIndex(1); clipStart_->setValue(seconds(clip->start)); clipDuration_->setValue(seconds(clip->duration)); clipOffset_->setValue(seconds(clip->offset)); clipLoop_->setChecked(clip->loop);
        const bool visual = c_.project.show.tracks[clip->track].visual; audioTransform_->setVisible(!visual); videoTransform_->setVisible(visual); clipText_->setVisible(!clip->text.empty());
        clipText_->setPlainText(QString::fromStdString(clip->text)); clipGain_->setValue(clip->gain); clipPan_->setValue(clip->pan); clipX_->setValue(clip->x); clipY_->setValue(clip->y); clipScale_->setValue(clip->scale); clipOpacity_->setValue(clip->opacity);
        const int property = curveProperty_->currentIndex(); if (visual && property < 2) curveProperty_->setCurrentIndex(5); if (!visual && property >= 2) curveProperty_->setCurrentIndex(0);
        updateCurve();
    } else if (auto* marker = selectedMarker()) {
        inspector_->setCurrentIndex(2); markerName_->setText(QString::fromStdString(marker->name)); markerTime_->setValue(seconds(marker->time)); markerHold_->setChecked(marker->hold);
        midiTiming_->setCurrentIndex(!marker->hold || marker->timing == MidiTiming::Arrival ? 0 : 1);
        midiTiming_->setEnabled(marker->hold);
        midiTiming_->setToolTip(marker->hold ? "" : "Without a hold, the message is sent when the playhead reaches this cue.");
        midiEditor_->setMessage(marker->message);
    } else {
        inspector_->setCurrentIndex(0); const auto& track = c_.project.show.tracks[timeline_->selectedTrack];
        trackName_->setText(QString::fromStdString(track.name)); trackMute_->setChecked(track.muted); trackAudio_->setVisible(!track.visual);
        trackAsync_->setChecked(track.async); trackGain_->setValue(track.gain); trackPan_->setValue(track.pan); trackLeft_->setValue(track.left + 1); trackRight_->setValue(track.right + 1);
    }
    refreshing_ = false; inspector_->setEnabled(c_.canEdit(false));
    const QSignalBlocker block(cues_); cues_->clearSelection();
    for (int i = 0; i < cues_->count(); ++i) if (cues_->item(i)->data(Qt::UserRole).toString() == timeline_->selectedMarker) { cues_->setCurrentRow(i); break; }
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
namespace {
// Untouched fields keep their exact model value instead of the spin box's rounded display.
double kept(const QDoubleSpinBox* spin, double model) { return std::abs(spin->value() - model) < .5 * std::pow(10., -spin->decimals()) ? model : spin->value(); }
Frame kept(const QDoubleSpinBox* spin, Frame model) { return std::abs(spin->value() - seconds(model)) < .5 * std::pow(10., -spin->decimals()) ? model : frames(spin->value()); }
}
void MainWindow::applyTrack() {
    inspectorDirty_ = false;
    const int index = inspectedTrack_; if (index < 0 || static_cast<std::size_t>(index) >= c_.project.show.tracks.size()) return;
    const Track& current = c_.project.show.tracks[index]; Track edited = current;
    const auto pick = [this](QWidget* field, auto shown, auto model) { return use(field) ? shown : model; };
    edited.name = pick(trackName_, trackName_->text().toStdString(), current.name); edited.muted = pick(trackMute_, trackMute_->isChecked(), current.muted);
    if (!edited.visual) {
        edited.async = pick(trackAsync_, trackAsync_->isChecked(), current.async); edited.gain = pick(trackGain_, kept(trackGain_, current.gain), current.gain);
        edited.pan = pick(trackPan_, kept(trackPan_, current.pan), current.pan); edited.left = pick(trackLeft_, trackLeft_->value() - 1, current.left); edited.right = pick(trackRight_, trackRight_->value() - 1, current.right);
    }
    if (edited.name == current.name && edited.muted == current.muted && edited.async == current.async && edited.gain == current.gain && edited.pan == current.pan &&
        edited.left == current.left && edited.right == current.right) return;
    c_.edit([=](Project& p) { p.show.tracks[index] = edited; });
}
void MainWindow::applyClip() {
    inspectorDirty_ = false;
    const Clip* clip = nullptr; for (const auto& c : c_.project.show.clips) if (c.id == inspectedClip_) clip = &c;
    if (!clip) return; Clip edited = *clip;
    const auto real = [this](QDoubleSpinBox* field, auto model) { return use(field) ? kept(field, model) : model; };
    edited.start = real(clipStart_, clip->start); edited.duration = real(clipDuration_, clip->duration); edited.offset = real(clipOffset_, clip->offset);
    if (use(clipLoop_)) edited.loop = clipLoop_->isChecked();
    edited.gain = real(clipGain_, clip->gain); edited.pan = real(clipPan_, clip->pan); edited.x = real(clipX_, clip->x); edited.y = real(clipY_, clip->y);
    edited.scale = real(clipScale_, clip->scale); edited.opacity = real(clipOpacity_, clip->opacity);
    if (!edited.text.empty() && use(clipText_)) edited.text = clipText_->toPlainText().toStdString();
    // A one-shot clip cannot outlast its source; the remainder would be silence or a frozen frame.
    const auto asset = c_.project.assets.constFind(QString::fromStdString(edited.assetId));
    const Frame source = edited.audio ? edited.audio->length() : asset != c_.project.assets.cend() && asset->video ? frames(asset->duration) : 0;
    if (!edited.loop && source > 0 && edited.offset + edited.duration > source) {
        edited.duration = std::max<Frame>(1, source - edited.offset);
        c_.report("Length is limited to the end of the source. Enable Loop source to play longer.");
    }
    if (edited.start == clip->start && edited.duration == clip->duration && edited.offset == clip->offset && edited.loop == clip->loop && edited.gain == clip->gain &&
        edited.pan == clip->pan && edited.x == clip->x && edited.y == clip->y && edited.scale == clip->scale && edited.opacity == clip->opacity && edited.text == clip->text) return;
    c_.edit([=](Project& p) {
        const auto it = std::find_if(p.show.clips.begin(), p.show.clips.end(), [&](const Clip& c) { return c.id == edited.id; });
        if (it == p.show.clips.end()) return;
        const Frame start = edited.start, duration = edited.duration, offset = edited.offset;
        Clip properties = edited; properties.start = it->start; properties.duration = it->duration; properties.offset = it->offset; *it = properties;
        retimeLinked(p, edited.id, start, duration, offset);
    });
}
void MainWindow::applyMarker(bool requireMidi) {
    inspectorDirty_ = false;
    const Marker* marker = nullptr; for (const auto& m : c_.project.show.markers) if (m.id == inspectedMarker_) marker = &m;
    if (!marker) return; Marker edited = *marker;
    if (use(markerName_)) edited.name = markerName_->text().toStdString();
    if (use(markerTime_)) edited.time = kept(markerTime_, marker->time);
    if (use(markerHold_) || use(midiTiming_)) { edited.hold = markerHold_->isChecked(); edited.timing = midiTiming_->currentIndex() == 0 ? MidiTiming::Arrival : MidiTiming::Resume; }
    QString error;
    // The MIDI message applies with Apply cue; automatic commits keep the stored one.
    if (requireMidi) {
        if (std::vector<unsigned char> bytes; midiEditor_->message(bytes, error)) edited.message = bytes;
        else { c_.report(error); return; }
    }
    if (edited.name == marker->name && edited.time == marker->time && edited.hold == marker->hold && edited.timing == marker->timing && edited.message == marker->message) return;
    c_.edit([=](Project& p) { for (auto& m : p.show.markers) if (m.id == edited.id) m = edited; });
}
void MainWindow::commitInspector() {
    if (!inspectorDirty_ || refreshing_) return;
    if (!c_.canEdit(false)) { inspectorDirty_ = false; return; }
    automatic_ = true;
    if (!inspectedClip_.empty()) applyClip(); else if (!inspectedMarker_.empty()) applyMarker(false); else applyTrack();
    automatic_ = false; touched_.clear();
}
void MainWindow::autoApply(QWidget* field) {
    const auto dirty = [this, field] { if (!refreshing_) { inspectorDirty_ = true; touched_.insert(field); } };
    const auto commit = [this, field] { if (!refreshing_) { inspectorDirty_ = true; touched_.insert(field); commitInspector(); } };
    if (auto* edit = qobject_cast<QLineEdit*>(field)) { connect(edit, &QLineEdit::textEdited, this, dirty); connect(edit, &QLineEdit::editingFinished, this, &MainWindow::commitInspector); }
    else if (auto* spin = qobject_cast<QDoubleSpinBox*>(field)) { connect(spin, &QDoubleSpinBox::valueChanged, this, dirty); connect(spin, &QDoubleSpinBox::editingFinished, this, &MainWindow::commitInspector); }
    else if (auto* spin = qobject_cast<QSpinBox*>(field)) { connect(spin, &QSpinBox::valueChanged, this, dirty); connect(spin, &QSpinBox::editingFinished, this, &MainWindow::commitInspector); }
    else if (auto* box = qobject_cast<QCheckBox*>(field)) connect(box, &QCheckBox::toggled, this, commit);
    else if (auto* combo = qobject_cast<QComboBox*>(field)) connect(combo, &QComboBox::activated, this, commit);
    else if (auto* text = qobject_cast<QTextEdit*>(field)) connect(text, &QTextEdit::textChanged, this, dirty);
}
void MainWindow::applyCurve() {
    const auto* clip = selectedClip(); if (!clip) return; const auto id = clip->id; const int property = curveProperty_->currentIndex(); Envelope curve;
    for (int row = 0; row < curveTable_->rowCount(); ++row) {
        bool timeOk = false, valueOk = false;
        const double time = curveTable_->item(row, 0) ? curveTable_->item(row, 0)->text().toDouble(&timeOk) : -1;
        const double value = curveTable_->item(row, 1) ? curveTable_->item(row, 1)->text().toDouble(&valueOk) : 0;
        if (!timeOk || !valueOk || !std::isfinite(time) || time < 0 || time > maxSeconds || !std::isfinite(value)) { c_.report("Each keyframe needs a valid nonnegative time and numeric value"); return; }
        curve.points.push_back({frames(time), value});
    }
    std::sort(curve.points.begin(), curve.points.end(), [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    c_.edit([=](Project& p) { for (auto& c : p.show.clips) if (c.id == id) *envelope(c, property) = curve; });
}

void MainWindow::importMedia() {
    if (!c_.canEdit()) return;
    importPaths(QFileDialog::getOpenFileNames(this, "Import media", {}, "Media (*.wav *.aiff *.aif *.flac *.mp3 *.ogg *.m4a *.mp4 *.mov *.mkv *.webm *.avi *.png *.jpg *.jpeg *.bmp *.webp);;All files (*)"));
}
void MainWindow::importPaths(const QStringList& paths) {
    if (paths.isEmpty() || !c_.canEdit()) return;
    c_.busy = true;
    auto* progress = new QProgressDialog("Decoding and preparing media…", QString(), 0, 0, this); progress->setWindowModality(Qt::WindowModal); progress->setMinimumDuration(0); progress->show();
    auto* watcher = new QFutureWatcher<MediaResult>(this);
    connect(watcher, &QFutureWatcher<MediaResult>::finished, this, [this, watcher, progress] {
        const auto result = watcher->result(); c_.busy = false; progress->deleteLater(); watcher->deleteLater();
        c_.edit([&](Project& p) { for (const auto& a : result.assets) { p.assets.insert(a.id, a); } });
        if (!result.errors.isEmpty()) QMessageBox::warning(this, "Some media could not be imported", result.errors.join("\n\n"));
        else c_.report(QString("Imported %1 media files. Drag them onto the timeline.").arg(result.assets.size()));
    });
    watcher->setFuture(QtConcurrent::run([paths]() {
        MediaResult result;
        for (const auto& path : paths) {
            try { auto a = loadMedia(path); result.assets.push_back(std::move(a)); }
            catch (const std::exception& e) { result.errors << QFileInfo(path).fileName() + ": " + QString::fromUtf8(e.what()); }
        }
        return result;
    }));
}
void MainWindow::splitClip() {
    const auto* clip = selectedClip(); if (!clip || !c_.canEdit()) return;
    const Frame time = c_.position();
    if (time <= clip->start || time >= clip->end()) { c_.report("Place the playhead inside the selected clip to split it"); return; }
    const auto ids = linkedClips(c_.project, clip->id);
    for (const auto& c : c_.project.show.clips) if (c.loop && std::find(ids.begin(), ids.end(), c.id) != ids.end()) {
        c_.report("Looping clips cannot be split; their loop starts at the clip's source in"); return;
    }
    const auto link = clip->link.empty() ? std::string() : newId().toStdString(); std::string second;
    const bool done = c_.edit([&](Project& p) {
        std::vector<Clip> tails;
        for (auto& c : p.show.clips) {
            if (std::find(ids.begin(), ids.end(), c.id) == ids.end() || time <= c.start || time >= c.end()) continue;
            const Frame cut = time - c.start;
            Clip tail = c; tail.id = newId().toStdString(); tail.link = link; tail.start = time; tail.duration = c.duration - cut; tail.offset = c.offset + cut;
            if (c.id == ids.front()) second = tail.id;
            // Clip-relative automation continues across the cut with the value at the cut.
            for (int property = 0; property < 6; ++property) {
                auto* head = envelope(c, property); auto* rest = envelope(tail, property);
                if (head->points.empty()) continue;
                const double at = head->at(cut, propertyValue(c, property));
                rest->points.clear(); rest->points.push_back({0, at});
                for (const auto& point : head->points) if (point.time > cut) rest->points.push_back({point.time - cut, point.value});
                std::erase_if(head->points, [=](const Keyframe& point) { return point.time >= cut; }); head->points.push_back({cut, at});
            }
            c.duration = cut; tails.push_back(std::move(tail));
        }
        p.show.clips.insert(p.show.clips.end(), tails.begin(), tails.end());
    });
    if (done) timeline_->selectClip(QString::fromStdString(second));
}
void MainWindow::nudge(int step) {
    if (!c_.canEdit()) return;
    const Frame delta = std::abs(step) == 1 ? std::max<Frame>(1, frames(1. / c_.project.fps)) * step : frames(step / 1000.);
    if (const auto* clip = selectedClip()) {
        const auto id = clip->id; const Frame start = std::clamp(clip->start + delta, Frame{0}, c_.project.show.duration - clip->duration);
        const Frame duration = clip->duration, offset = clip->offset;
        c_.edit([=](Project& p) { retimeLinked(p, id, start, duration, offset); });
    } else if (const auto* marker = selectedMarker()) {
        const auto id = marker->id; const Frame time = std::clamp(marker->time + delta, Frame{0}, c_.project.show.duration);
        c_.edit([=](Project& p) { for (auto& m : p.show.markers) if (m.id == id) m.time = time; });
    }
}
void MainWindow::jumpToCue(bool forward) {
    const Frame now = c_.position(); const Marker* target = nullptr;
    for (const auto& m : c_.project.show.markers) {
        if (forward ? m.time > now && (!target || m.time < target->time) : m.time < now && (!target || m.time > target->time)) target = &m;
    }
    if (!target) { c_.report(forward ? "No later cue" : "No earlier cue"); return; }
    timeline_->selectMarker(QString::fromStdString(target->id)); c_.seek(target->time);
}
void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls() && c_.canEdit(false)) event->acceptProposedAction();
}
void MainWindow::dropEvent(QDropEvent* event) {
    QStringList paths;
    for (const auto& url : event->mimeData()->urls()) if (url.isLocalFile()) paths << url.toLocalFile();
    if (paths.isEmpty()) return;
    event->acceptProposedAction();
    // A dropped show opens; anything else imports into the media library.
    if (paths.size() == 1 && (paths[0].endsWith(".savshow", Qt::CaseInsensitive) || isProjectPackage(paths[0]))) { if (c_.canEdit() && checkDiscard()) openPath(paths[0]); }
    else importPaths(paths);
}
void MainWindow::relinkMedia(bool afterOpening) {
    QStringList missing;
    for (const auto& a : c_.project.assets) if (a.offline) missing << a.name + "  —  " + a.problem;
    if (missing.isEmpty()) { if (!afterOpening) c_.report("All media is online."); return; }
    if (!c_.canEdit()) return;
    if (afterOpening) {
        QMessageBox box(QMessageBox::Warning, "Media is missing", QString("%1 media file%2 could not be loaded. Their clips stay on the timeline but are silent or blank until relinked.")
            .arg(missing.size()).arg(missing.size() == 1 ? "" : "s"), QMessageBox::Close, this);
        box.setDetailedText(missing.join("\n"));
        auto* locate = box.addButton("Locate folder…", QMessageBox::AcceptRole); box.setDefaultButton(locate); box.exec();
        if (box.clickedButton() != locate) return;
    }
    const QString folder = QFileDialog::getExistingDirectory(this, "Folder containing the missing media");
    if (folder.isEmpty()) return;
    // Search by file name below the chosen folder, then load matches off the GUI thread.
    QMap<QString, QString> wanted; // Lower-case file name → asset id.
    for (const auto& a : c_.project.assets) if (a.offline) wanted.insert(QFileInfo(a.path).fileName().toLower(), a.id);
    c_.busy = true;
    auto* progress = new QProgressDialog("Searching for missing media…", QString(), 0, 0, this); progress->setWindowModality(Qt::WindowModal); progress->setMinimumDuration(0); progress->show();
    auto* watcher = new QFutureWatcher<MediaResult>(this);
    connect(watcher, &QFutureWatcher<MediaResult>::finished, this, [this, watcher, progress] {
        const auto result = watcher->result(); c_.busy = false; progress->deleteLater(); watcher->deleteLater();
        if (!result.assets.empty()) c_.edit([&](Project& p) { for (const auto& a : result.assets) p.assets.insert(a.id, a); });
        int remaining = 0; for (const auto& a : c_.project.assets) remaining += a.offline;
        const QString summary = QString("Relinked %1 media file%2.").arg(result.assets.size()).arg(result.assets.size() == 1 ? "" : "s") +
            (remaining ? QString(" %1 still missing.").arg(remaining) : "");
        if (!result.errors.isEmpty()) QMessageBox::warning(this, "Some media could not be relinked", summary + "\n\n" + result.errors.join("\n\n"));
        c_.report(summary); if (!remaining) c_.playbackProblem.clear();
    });
    const auto offline = c_.project.assets;
    watcher->setFuture(QtConcurrent::run([folder, wanted, offline]() {
        MediaResult result; QSet<QString> found;
        for (QDirIterator it(folder, QDir::Files, QDirIterator::Subdirectories); it.hasNext();) {
            const QString path = it.next(); const auto key = QFileInfo(path).fileName().toLower();
            if (!wanted.contains(key) || found.contains(key)) continue;
            found.insert(key);
            try {
                auto a = loadMedia(path); const auto& entry = offline[wanted.value(key)];
                a.id = entry.id; a.name = entry.name; result.assets.push_back(std::move(a));
            } catch (const std::exception& e) { result.errors << QFileInfo(path).fileName() + ": " + QString::fromUtf8(e.what()); }
        }
        return result;
    }));
}
void MainWindow::placeAsset(QString id, int track, Frame time) {
    if (!c_.canEdit() || !c_.project.assets.contains(id)) return;
    const auto& asset = c_.project.assets[id]; const bool visual = c_.project.show.tracks[track].visual;
    if ((visual && !asset.image && !asset.video) || (!visual && !asset.audio)) { c_.report("Choose a compatible picture or audio track"); return; }
    Clip clip; clip.id = newId().toStdString(); clip.assetId = id.toStdString(); clip.track = track; clip.start = time;
    clip.duration = frames(asset.duration); if (clip.duration > maxFrame - clip.start - frames(5)) { c_.report("Media extends beyond representable frame timing"); return; }
    if (c_.edit([=](Project& p) { p.show.duration = std::max(p.show.duration, clip.end() + frames(5)); p.show.clips.push_back(clip);
        if (visual && asset.video && asset.audio) {
            auto audioTrack = std::find_if(p.show.tracks.begin(), p.show.tracks.end(), [](const Track& t) { return !t.visual && !t.async; });
            if (audioTrack != p.show.tracks.end()) {
                // The picture and its sound stay in sync: they move, trim, split, and delete together.
                Clip sound = clip; sound.id = newId().toStdString(); sound.track = static_cast<int>(audioTrack - p.show.tracks.begin());
                sound.link = p.show.clips.back().link = newId().toStdString(); p.show.clips.push_back(sound);
            }
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
    // Number after the highest existing "Hold N" so names stay unique after deletions.
    int number = 0;
    for (const auto& m : c_.project.show.markers) if (m.name.starts_with("Hold ")) number = std::max(number, QString::fromStdString(m.name.substr(5)).toInt());
    Marker marker; marker.id = newId().toStdString(); marker.name = "Hold " + std::to_string(number + 1); marker.time = c_.position();
    if (c_.edit([=](Project& p) { p.show.markers.push_back(marker); })) timeline_->selectMarker(QString::fromStdString(marker.id));
}
void MainWindow::addTrack(bool visual, bool async) {
    Track track; track.id = newId().toStdString(); track.name = visual ? "Picture layer" : async ? "Async audio" : "Audio"; track.visual = visual; track.async = async;
    if (c_.edit([=](Project& p) { p.show.tracks.push_back(track); })) { timeline_->selectedTrack = static_cast<int>(c_.project.show.tracks.size()) - 1; timeline_->selectedClip.clear(); timeline_->selectedMarker.clear(); updateInspector(); }
}
void MainWindow::deleteSelection() {
    if (auto* clip = selectedClip()) {
        const auto ids = linkedClips(c_.project, clip->id);
        c_.edit([=](Project& p) { std::erase_if(p.show.clips, [&](const Clip& c) { return std::find(ids.begin(), ids.end(), c.id) != ids.end(); }); });
    }
    else if (auto* marker = selectedMarker()) { const auto id = marker->id; c_.edit([=](Project& p) { std::erase_if(p.show.markers, [=](const Marker& m) { return m.id == id; }); }); }
}

void MainWindow::projectSettings() {
    if (!c_.canEdit()) return;
    QDialog dialog(this); dialog.setWindowTitle("Project settings"); auto* layout = new QVBoxLayout(&dialog); auto* form = new QFormLayout;
    auto* title = new QLineEdit(c_.project.title); auto* width = intSpin(320, 7680); width->setValue(c_.project.width); auto* height = intSpin(240, 4320); height->setValue(c_.project.height);
    auto* fps = realSpin(1, 120, 1); fps->setValue(c_.project.fps); auto* duration = realSpin(.001, maxSeconds, 10); duration->setValue(seconds(c_.project.show.duration));
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
    QDialog dialog(this); dialog.setObjectName("outputSettingsDialog"); dialog.setWindowTitle("Outputs"); dialog.setMinimumWidth(580);
    auto* outer = new QVBoxLayout(&dialog);
    auto* scroll = new QScrollArea; scroll->setWidgetResizable(true);
    auto* content = new QWidget; content->setObjectName("outputSettingsContent"); auto* layout = new QVBoxLayout(content); scroll->setWidget(content); outer->addWidget(scroll);
    dialog.resize(620, std::min(780, screen()->availableGeometry().height() - 60));
    auto* connectionStatus = new QLabel; connectionStatus->setObjectName("outputConnectionStatus"); connectionStatus->setWordWrap(true);
    layout->addWidget(connectionStatus);
    auto* stopForSettings = button("Stop All to change connections", layout); stopForSettings->setObjectName("stopForOutputSettings");
    QSettings preferences;
    auto* audioBox = new QGroupBox("Audio · speakers or show interface"); auto* audioLayout = new QVBoxLayout(audioBox);
    auto* audioHint = new QLabel("Play prepares audio automatically. Selecting a device below switches audio to it. Use audio settings to apply channel or latency changes.");
    audioHint->setWordWrap(true); audioLayout->addWidget(audioHint);
    auto* form = new QFormLayout; audioLayout->addLayout(form); layout->addWidget(audioBox);
    auto* api = new QComboBox; auto* devices = new QComboBox; auto* channels = intSpin(1, std::numeric_limits<int>::max()); channels->setValue(2);
    auto* buffer = new QComboBox; for (int n : {128, 256, 512, 1024, 2048}) buffer->addItem(QString::number(n) + " samples", n); buffer->setCurrentIndex(2);
    const auto apis = c_.device.apis(); for (const auto& [id, name] : apis) api->addItem(name, static_cast<int>(id));
    const int previousApi = api->findData(preferences.value("audio/api", static_cast<int>(RtAudio::UNSPECIFIED)).toInt());
    if (previousApi >= 0) api->setCurrentIndex(previousApi);
    auto refreshDevices = [&] {
        const int requestedChannels = channels->value();
        devices->clear(); const auto selected = static_cast<RtAudio::Api>(api->currentData().toInt());
        // Inspecting a live/held show must not probe and load audio drivers.
        if (!c_.canEdit(false)) {
            devices->addItem(c_.audioName.isEmpty() ? "Audio not configured" : c_.audioName);
            devices->setItemData(0, c_.device.channels(), Qt::UserRole + 1);
            channels->setMaximum(std::max(1, static_cast<int>(c_.device.channels())));
            return;
        }
        try {
            const auto list = c_.device.devices(selected); int defaultIndex = -1;
            const auto savedApi = static_cast<RtAudio::Api>(preferences.value("audio/api", 0).toInt());
            const QString savedName = selected == savedApi || savedApi == RtAudio::UNSPECIFIED ? preferences.value("audio/device").toString() : QString();
            const auto preferred = playbackDevice(list, savedName);
            for (const auto& d : list) { devices->addItem(QString("%1 (%2 outputs)").arg(d.name).arg(d.channels), d.id); devices->setItemData(devices->count() - 1, d.channels, Qt::UserRole + 1);
                devices->setItemData(devices->count() - 1, d.name, Qt::UserRole + 2); if (preferred && d.id == preferred->id) defaultIndex = devices->count() - 1; }
            if (!preferred && !savedName.isEmpty()) {
                devices->addItem(savedName + " (missing)"); defaultIndex = devices->count() - 1;
                devices->setItemData(defaultIndex, std::numeric_limits<int>::max(), Qt::UserRole + 1);
            }
            devices->setCurrentIndex(defaultIndex);
        } catch (const std::exception& error) { c_.report(QString::fromUtf8(error.what())); }
        channels->setMaximum(std::max(1, devices->currentData(Qt::UserRole + 1).toInt()));
        channels->setValue(requestedChannels);
    };
    connect(api, &QComboBox::currentIndexChanged, &dialog, refreshDevices);
    connect(devices, &QComboBox::currentIndexChanged, &dialog, [&] { channels->setMaximum(std::max(1, devices->currentData(Qt::UserRole + 1).toInt())); }); refreshDevices();
    channels->setValue(preferences.value("audio/channels", 2).toInt());
    const int savedBuffer = buffer->findData(preferences.value("audio/buffer", 512).toInt()); if (savedBuffer >= 0) buffer->setCurrentIndex(savedBuffer);
    devices->setObjectName("audioOutputDevice"); channels->setObjectName("audioOutputChannels");
    form->addRow("Play sound through", devices); form->addRow("Outputs to use", channels);
    auto* advanced = new QGroupBox("Advanced audio settings"); auto* advancedForm = new QFormLayout(advanced);
    advancedForm->addRow("Audio driver", api); advancedForm->addRow("Buffer size", buffer);
    auto* advancedToggle = new QCheckBox("Show driver and latency settings"); form->addRow(advancedToggle); form->addRow(advanced);
    advanced->hide(); connect(advancedToggle, &QCheckBox::toggled, advanced, &QWidget::setVisible);
    buffer->setToolTip("512 samples is a good starting point. Increase if audio crackles; decrease for lower latency.");
    channels->setToolTip("Stereo speakers use 2 outputs. A show interface may need more for Guide and Click.");
    auto* audioApply = button("Use audio settings"); audioApply->setObjectName("connectAudioOutput"); form->addRow(audioApply);
    auto useAudio = [&] {
        if (!devices->currentData().isValid()) { c_.report("Choose an available audio output, or plug in the missing device and click Refresh devices."); return; }
        const bool ok = c_.configureAudio(static_cast<RtAudio::Api>(api->currentData().toInt()), devices->currentData().toUInt(), devices->currentData(Qt::UserRole + 2).toString(), channels->value(), buffer->currentData().toUInt());
        if (!ok) QMessageBox::warning(&dialog, "Audio connection failed", c_.status);
    };
    connect(audioApply, &QPushButton::clicked, &dialog, useAudio);
    connect(devices, &QComboBox::activated, &dialog, useAudio);
    auto* routing = new QLabel("Track routing is edited in the track inspector.\nGuide and Click start disconnected. For one interface with four outputs,\nuse Main 1/2, Guide 3/Off, Click 4/Off. Windows: prefer the interface's ASIO driver."); routing->setWordWrap(true); form->addRow(routing);
    auto* midiBox = new QGroupBox("MIDI · control lights, instruments or other apps"); auto* midiForm = new QFormLayout(midiBox); layout->addWidget(midiBox);
    auto* midiHint = new QLabel("Only needed for cues that send MIDI. Choose a destination once; selecting it connects immediately and Play reconnects it next time.");
    midiHint->setWordWrap(true); midiForm->addRow(midiHint);
    auto* midiPort = new QComboBox; midiPort->setObjectName("midiOutputDevice");
    auto refreshMidi = [&] {
        midiPort->clear(); midiPort->addItem("No MIDI device", -1);
        const QString remembered = c_.midi.connected() ? c_.midi.name() : QSettings().value("midi/port").toString();
        const auto ports = c_.canEdit(false) ? c_.midi.ports() : QStringList{c_.midi.name()};
        for (int i = 0; i < ports.size(); ++i) if (!ports[i].isEmpty()) midiPort->addItem(ports[i], i);
        const int savedPort = midiPort->findText(remembered);
        if (savedPort >= 0) midiPort->setCurrentIndex(savedPort);
        else if (!remembered.isEmpty()) { midiPort->addItem(remembered + " (missing)", -2); midiPort->setCurrentIndex(midiPort->count() - 1); }
    };
    refreshMidi(); midiForm->addRow("Send cues to", midiPort);
    auto* midiConnect = button("Reconnect MIDI device"); midiConnect->setObjectName("connectMidiOutput"); midiForm->addRow(midiConnect);
    auto useMidi = [&] {
        const int port = midiPort->currentData().toInt();
        if (port == -2) { c_.report("This MIDI destination is missing. Plug it in and click Refresh devices."); return; }
        if (!c_.configureMidi(port)) QMessageBox::warning(&dialog, "MIDI connection failed", c_.status);
    };
    connect(midiPort, &QComboBox::activated, &dialog, useMidi);
    connect(midiConnect, &QPushButton::clicked, &dialog, useMidi);
    auto* refreshPorts = button("Refresh devices", layout); refreshPorts->setObjectName("refreshOutputDevices");
    connect(refreshPorts, &QPushButton::clicked, &dialog, [&] { if (c_.canEdit(false)) { refreshDevices(); refreshMidi(); } });
    auto* displayBox = new QGroupBox("Picture · audience display"); auto* displayForm = new QFormLayout(displayBox); layout->addWidget(displayBox);
    auto* displays = new QComboBox; displays->setObjectName("audienceOutputDisplay"); const auto screens = QGuiApplication::screens();
    for (int i = 0; i < screens.size(); ++i) displays->addItem(QString("%1 · %2 × %3%4").arg(screens[i]->name()).arg(screens[i]->size().width()).arg(screens[i]->size().height()).arg(screens[i] == screen() ? " (editor)" : ""), i);
    displayForm->addRow("Show picture on", displays); auto* displayOpen = button("Open fullscreen program output"); displayOpen->setObjectName("openProgramDisplay"); displayForm->addRow(displayOpen);
    if (const int remembered = displays->findText(QSettings().value("display/screen").toString(), Qt::MatchStartsWith); remembered >= 0) displays->setCurrentIndex(remembered);
    connect(displayOpen, &QPushButton::clicked, &dialog, [&] { openOutput(displays->currentData().toInt()); });
    auto* displayClose = button("Close program output"); displayForm->addRow(displayClose); connect(displayClose, &QPushButton::clicked, &dialog, [this] { if (outputWindow_) outputWindow_->hide(); });
    auto* feedback = new QLabel; feedback->setWordWrap(true); feedback->setStyleSheet("color: #e5c47d;"); outer->addWidget(feedback);
    connect(&c_, &Controller::message, &dialog, [feedback](const QString& text) { feedback->setText(text); });
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close); outer->addWidget(close); connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    bool wasEditable = c_.canEdit(false);
    auto updateConnections = [&] {
        if (c_.showMode || c_.busy) { dialog.reject(); return; }
        const bool editable = c_.canEdit(false);
        if (editable && !wasEditable) {
            const int requestedChannels = preferences.value("audio/channels", 2).toInt();
            refreshDevices(); refreshMidi(); channels->setValue(requestedChannels);
        }
        wasEditable = editable;
        const QString reason = editable ? QString() : "Use Stop All before changing audio or MIDI connections; this silences any running async audio.";
        for (QWidget* control : std::initializer_list<QWidget*>{api, devices, channels, buffer, midiPort, midiConnect, refreshPorts}) {
            control->setEnabled(editable); control->setToolTip(reason);
        }
        const bool audioAvailable = devices->currentData().isValid();
        audioApply->setEnabled(editable && audioAvailable);
        audioApply->setToolTip(editable && !audioAvailable ? "Choose an available device or click Refresh devices after plugging it in" : reason);
        stopForSettings->setVisible(!editable);
        const QString state = c_.transport.state() == State::Held ? "Held at a stop marker." : c_.transport.state() == State::Playing ? "Playback is running." : "Async audio is still running.";
        connectionStatus->setText(editable ?
            (c_.device.running() ? "Audio: " + c_.audioName : "Audio: automatic on Play") + "\n" +
            (c_.midi.connected() ? "MIDI: " + c_.midi.name() : c_.needsMidi() ? "MIDI: choose a destination for this show's cues" : "MIDI: not needed for this show") : state + " Audio and MIDI connections are locked. Use Stop All to silence audio and unlock them.");
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
        outputWindow_->setCursor(Qt::BlankCursor);
        auto* close = new QShortcut(QKeySequence::Close, outputWindow_); connect(close, &QShortcut::activated, outputWindow_, &QWidget::hide);
    }
    if (screens[index] == screen() && QMessageBox::question(this, "Cover the editor?",
            "This display shows the editor. The program output will cover it until you press " + QKeySequence(QKeySequence::Close).toString(QKeySequence::NativeText) + " on the output.",
            QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Ok) return;
    output_->setPicture(c_.compose()); outputWindow_->hide(); outputWindow_->winId(); outputWindow_->windowHandle()->setScreen(screens[index]);
    outputWindow_->setGeometry(screens[index]->geometry()); outputWindow_->showFullScreen();
#ifdef Q_OS_WIN
    // Windows can choose the primary monitor during the fullscreen transition.
    // Apply the selected screen and native geometry after that transition too.
    outputWindow_->windowHandle()->setScreen(screens[index]);
    outputWindow_->windowHandle()->setGeometry(screens[index]->geometry());
#endif
    activateWindow();
    outputScreen_ = screens[index]->name(); QSettings().setValue("display/screen", outputScreen_);
}
void MainWindow::screensChanged() {
    // A display that drops out must not leave a frameless fullscreen window over the editor; it returns with the display.
    if (outputWindow_ && outputWindow_->isVisible() && !QGuiApplication::screens().contains(outputWindow_->screen())) {
        outputWindow_->hide(); outputLost_ = true;
        c_.report("Program display “" + outputScreen_ + "” disconnected. The output reopens when it returns."); c_.playbackProblem = c_.status;
    } else if (outputLost_) {
        const auto screens = QGuiApplication::screens();
        for (int i = 0; i < screens.size(); ++i) if (screens[i]->name() == outputScreen_ && screens[i] != screen()) {
            outputLost_ = false; openOutput(i);
            c_.report("Program display “" + outputScreen_ + "” reconnected."); if (c_.playbackProblem.contains("Program display")) c_.playbackProblem.clear();
            break;
        }
    }
}
void MainWindow::requestShowMode(bool enabled) {
    const State state = c_.transport.state();
    if (enabled) {
        auto problems = c_.preflight();
        const bool picture = std::any_of(c_.project.show.clips.begin(), c_.project.show.clips.end(), [this](const Clip& clip) { return c_.project.show.tracks[clip.track].visual; });
        if (picture && !(outputWindow_ && outputWindow_->isVisible())) problems << "The program output is not open. Open it from Audience display.";
        if (!problems.isEmpty()) {
            QMessageBox box(QMessageBox::Warning, "Check before the show", "Show mode can start, but these need attention:", QMessageBox::Cancel, this);
            box.setInformativeText("• " + problems.join("\n• "));
            auto* start = box.addButton("Enter Show mode anyway", QMessageBox::AcceptRole); box.setDefaultButton(QMessageBox::Cancel); box.exec();
            if (box.clickedButton() != start) { showButton_->setChecked(false); return; }
        }
    } else if (state == State::Playing || state == State::Held || c_.transport.asyncVoices()) {
        if (QMessageBox::question(this, "Leave Show mode?", "The show is running. Leaving Show mode unlocks editing and timeline seeking.",
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) { showButton_->setChecked(true); return; }
    }
    c_.setShowMode(enabled);
}
void MainWindow::openPath(const QString& path) {
    if (!c_.canEdit()) return;
    // A sidecar opens as unsaved edits to its show, so Save replaces the show rather than writing "x.savshow.autosave.savshow".
    QString source = path, target = isProjectPackage(path) ? QString() : path;
    bool recovered = path.endsWith(".savshow.autosave", Qt::CaseInsensitive);
    if (recovered) target = path == Controller::untitledAutosavePath() ? QString() : target.chopped(QString(".autosave").size());
    else if (const QFileInfo sidecar(path + ".autosave"); !target.isEmpty() && sidecar.exists() && sidecar.lastModified() > QFileInfo(path).lastModified()) {
        QMessageBox box(QMessageBox::Question, "Recover unsaved changes?",
            "An autosave of “" + QFileInfo(path).fileName() + "” from " + QLocale().toString(sidecar.lastModified(), QLocale::ShortFormat) + " is newer than the saved show.",
            QMessageBox::NoButton, this);
        box.setInformativeText("Recovering opens the autosave as unsaved changes. Opening the saved show keeps the autosave until the next autosave or save replaces it.");
        auto* recover = box.addButton("Recover autosave", QMessageBox::AcceptRole);
        box.addButton("Open saved show", QMessageBox::RejectRole); box.setDefaultButton(recover); box.exec();
        if (box.clickedButton() == recover) { source = sidecar.filePath(); recovered = true; }
    }
    c_.busy = true;
    auto* progress = new QProgressDialog("Opening project and preparing media…", QString(), 0, 0, this); progress->setWindowModality(Qt::WindowModal); progress->setMinimumDuration(0); progress->show();
    auto* watcher = new QFutureWatcher<ProjectResult>(this);
    connect(watcher, &QFutureWatcher<ProjectResult>::finished, this, [this, watcher, progress, path, target, recovered] {
        const auto result = watcher->result(); c_.busy = false; progress->deleteLater(); watcher->deleteLater();
        if (!result.error.isEmpty()) { QMessageBox::warning(this, "Could not open project", result.error); return; }
        timeline_->selectedClip.clear(); timeline_->selectedMarker.clear(); timeline_->selectedTrack = 0;
        c_.replace(result.project, target);
        QTimer::singleShot(0, this, [this] { if (std::any_of(c_.project.assets.cbegin(), c_.project.assets.cend(), [](const MediaAsset& a) { return a.offline; })) relinkMedia(true); });
        if (recovered) {
            c_.dirty = true;
            c_.report(target.isEmpty() ? "Recovered an unsaved show. Save it to keep it." : "Recovered autosave. Save to replace " + QFileInfo(target).fileName() + ".");
            refresh();
        } else if (isProjectPackage(path)) {
            c_.dirty = true;
            c_.report("Opened package. Save this show to keep an editable copy, or use Export package to share it.");
            refresh();
        }
    });
    watcher->setFuture(QtConcurrent::run([source] { ProjectResult result; try { result.project = readProject(source); } catch (const std::exception& e) { result.error = QString::fromUtf8(e.what()); } return result; }));
}
void MainWindow::save(bool as) {
    if (!c_.canEdit()) return;
    QString target = c_.path;
    if (as || target.isEmpty()) target = QFileDialog::getSaveFileName(this, "Save show", target, "SimpleAVShow (*.savshow)");
    if (target.isEmpty()) return;
    if (!target.endsWith(".savshow", Qt::CaseInsensitive)) target += ".savshow";
    if (!c_.savingCopiesMedia()) { c_.save(target); return; }
    // Copying package media can take minutes; keep the GUI responsive and the project locked meanwhile.
    c_.busy = true; updatePlayback();
    QProgressDialog progress("Copying media next to the show…", QString(), 0, 0, this);
    progress.setWindowTitle("Saving show"); progress.setWindowModality(Qt::WindowModal); progress.setMinimumDuration(0);
    progress.setAutoClose(false); progress.setAutoReset(false);
    QFutureWatcher<ProjectResult> watcher;
    connect(&watcher, &QFutureWatcher<ProjectResult>::finished, &progress, &QDialog::accept);
    const QString directory = Controller::mediaDirectory(target);
    watcher.setFuture(QtConcurrent::run([candidate = c_.project, directory]() mutable {
        ProjectResult result;
        try { materializeProjectMedia(candidate, directory); result.project = std::move(candidate); }
        catch (const std::exception& e) { result.error = QString::fromUtf8(e.what()); }
        return result;
    }));
    progress.exec();
    auto result = watcher.result(); c_.busy = false;
    if (!result.error.isEmpty()) { c_.report("Save failed: " + result.error); QMessageBox::warning(this, "Could not save", result.error); refresh(); return; }
    c_.commitSave(std::move(result.project), target);
}
void MainWindow::exportPackage() {
    if (!c_.canEdit()) return;
    const QString suggested = c_.path.isEmpty() ? "show.savpack" : QFileInfo(c_.path).absoluteDir().filePath(QFileInfo(c_.path).completeBaseName() + ".savpack");
    QFileDialog dialog(this, "Export show package", suggested, "SimpleAVShow package (*.savpack)");
    dialog.setAcceptMode(QFileDialog::AcceptSave); dialog.setDefaultSuffix("savpack");
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return;
    auto target = dialog.selectedFiles().front();
    if (!isProjectPackage(target)) {
        target += ".savpack";
        if (QFileInfo::exists(target) && QMessageBox::question(this, "Replace package?", "A package with this name already exists. Replace it?", QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    }
    writePackage(target);
}
void MainWindow::writePackage(const QString& path) {
    if (!c_.canEdit()) return;
    c_.busy = true; updatePlayback();
    PackageProgress progress("Collecting show and media into a package…", QString(), 0, 0, this);
    progress.setWindowTitle("Exporting show package");
    progress.setWindowModality(Qt::WindowModal); progress.setMinimumDuration(0);
    progress.setAutoClose(false); progress.setAutoReset(false);
    QFutureWatcher<QString> watcher;
    connect(&watcher, &QFutureWatcher<QString>::finished, &progress, &QDialog::accept);
    const Project snapshot = c_.project;
    watcher.setFuture(QtConcurrent::run([snapshot, path] {
        try { writeProjectPackage(snapshot, path); return QString(); }
        catch (const std::exception& e) { return QString::fromUtf8(e.what()); }
    }));
    progress.exec();
    const auto error = watcher.result(); c_.busy = false;
    if (!error.isEmpty()) {
        c_.report("Package failed: " + error);
        QMessageBox::warning(this, "Could not write package", error);
    } else {
        c_.report("Exported " + QFileInfo(path).fileName());
    }
    refresh();
}
void MainWindow::offerUntitledRecovery() {
    const QFileInfo sidecar(Controller::untitledAutosavePath());
    if (!sidecar.exists() || !c_.canEdit(false)) return;
    const auto answer = QMessageBox::question(this, "Recover unsaved show?",
        "SimpleAVShow closed before an untitled show was saved. Recover the autosave from " + QLocale().toString(sidecar.lastModified(), QLocale::ShortFormat) + "?",
        QMessageBox::Yes | QMessageBox::Discard, QMessageBox::Yes);
    if (answer == QMessageBox::Yes) openPath(sidecar.filePath());
    else QFile::remove(sidecar.filePath());
}
bool MainWindow::checkDiscard() {
    if (!c_.dirty) return true;
    const auto result = QMessageBox::question(this, "Unsaved show", "Save changes to this show?", QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (result == QMessageBox::Cancel) return false; if (result == QMessageBox::Save) { save(); return !c_.dirty; }
    c_.discardAutosave(); return true;
}
void MainWindow::closeEvent(QCloseEvent* event) {
    if (c_.busy) { c_.report("Wait for media preparation to finish before closing"); event->ignore(); return; }
    // Quitting mid-performance must be deliberate; Cancel is the default so Space or Enter keeps the show running.
    const State state = c_.transport.state();
    if (c_.showMode || state == State::Playing || state == State::Held || c_.transport.asyncVoices()) {
        QMessageBox box(QMessageBox::Warning, "Show is running", c_.showMode ? "Show mode is active." : "Playback is active.",
            QMessageBox::Cancel, this);
        box.setInformativeText("Quitting stops all audio, MIDI cues, and the program output.");
        auto* quit = box.addButton("Stop and quit", QMessageBox::DestructiveRole);
        box.setDefaultButton(QMessageBox::Cancel); box.exec();
        if (box.clickedButton() != quit) { event->ignore(); return; }
    }
    if (!checkDiscard()) { event->ignore(); return; }
    c_.stopAll(); event->accept();
}
bool MainWindow::eventFilter(QObject* object, QEvent* event) {
    if (event->type() != QEvent::KeyPress || !isVisible()) return QMainWindow::eventFilter(object, event);
    auto* key = static_cast<QKeyEvent*>(event); auto* widget = qobject_cast<QWidget*>(object);
    if (!widget || (widget->window() != this && widget->window() != outputWindow_)) return QMainWindow::eventFilter(object, event);
    const bool editing = qobject_cast<QLineEdit*>(widget) || qobject_cast<QTextEdit*>(widget) || qobject_cast<QAbstractSpinBox*>(widget) || qobject_cast<QComboBox*>(widget);
    // Outside Show mode Escape is Stop All unless it cancels a field edit. In Show mode a stray press
    // must not end the performance: two presses within 600 ms stop everything (the button still takes one click).
    if (key->key() == Qt::Key_Escape && !key->isAutoRepeat() && (c_.showMode || !editing)) {
        if (!c_.showMode) { c_.stopAll(); return true; }
        if (escape_.isValid() && !escape_.hasExpired(600)) { escape_.invalidate(); c_.stopAll(); }
        else { escape_.start(); c_.report("Press Escape again to Stop All."); }
        return true;
    }
    if (key->key() == Qt::Key_Space && !editing && key->modifiers() == Qt::NoModifier) { if (!key->isAutoRepeat()) c_.toggle(); return true; }
    if ((key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) && !editing && !qobject_cast<QTableWidget*>(widget)) { if (!key->isAutoRepeat()) deleteSelection(); return true; }
    return QMainWindow::eventFilter(object, event);
}

void MainWindow::agentSettings() {
    QDialog dialog(this); dialog.setWindowTitle("Agent collaboration · MCP"); dialog.resize(620, 360);
    auto* layout = new QVBoxLayout(&dialog);
    auto* hint = new QLabel("Connect an agent to this open show. Agent changes appear immediately and use the same Undo history. Playback, Show mode, outputs and saving stay with the operator.");
    hint->setWordWrap(true); layout->addWidget(hint);
    auto* enabled = new QCheckBox("Enable local MCP server"); enabled->setObjectName("agentServerEnabled"); layout->addWidget(enabled);
    auto* form = new QFormLayout; layout->addLayout(form);
    auto* port = intSpin(1024, 65535); port->setObjectName("agentServerPort"); port->setValue(mcp_.running() ? QUrl(mcp_.endpoint()).port() : 48123); form->addRow("Local port", port);
    auto* editing = new QCheckBox("Allow the agent to edit this show and import media"); editing->setObjectName("agentEditingEnabled"); form->addRow(editing);
    auto* endpoint = new QLineEdit; endpoint->setReadOnly(true); form->addRow("MCP URL", endpoint);
    auto* token = new QLineEdit; token->setReadOnly(true); token->setEchoMode(QLineEdit::Password); form->addRow("Bearer token", token);
    auto* copy = button("Copy connection configuration", layout);
    auto* copyToken = button("Copy bearer token", layout);
    auto* status = new QLabel; status->setWordWrap(true); layout->addWidget(status);
    auto refresh = [&] {
        QSignalBlocker a(enabled), b(editing);
        enabled->setChecked(mcp_.running()); editing->setChecked(mcp_.editingEnabled());
        port->setEnabled(!mcp_.running()); editing->setEnabled(mcp_.running());
        copy->setEnabled(mcp_.running()); copyToken->setEnabled(mcp_.running());
        endpoint->setText(mcp_.endpoint()); token->setText(mcp_.token());
        status->setText(mcp_.running() ? (mcp_.editingEnabled() ? "Agent editing enabled. Playback and Show mode still lock edits." : "Read only. Allow editing above to build the show together.") : "Disabled. Connections and tokens last for this app session only.");
    };
    connect(&mcp_, &McpServer::stateChanged, &dialog, refresh);
    connect(enabled, &QCheckBox::toggled, &dialog, [&](bool on) {
        if (on) {
            commitInspector();
            if (!mcp_.start(static_cast<quint16>(port->value()))) {
                refresh(); status->setText("Cannot start MCP server: " + mcp_.errorString());
            }
        } else { mcp_.stop(); c_.report("Agent collaboration disabled."); }
    });
    connect(editing, &QCheckBox::toggled, &dialog, [&](bool on) {
        commitInspector(); mcp_.setEditingEnabled(on);
        c_.report(on ? "Agent editing enabled. Changes can be undone from Edit." : "Agent collaboration is read only.");
    });
    connect(copy, &QPushButton::clicked, &dialog, [&] {
        QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(mcp_.clientConfig()).toJson(QJsonDocument::Indented)));
        status->setText("Connection configuration copied. Paste it into your agent's MCP server settings.");
    });
    connect(copyToken, &QPushButton::clicked, &dialog, [&] { QApplication::clipboard()->setText(mcp_.token()); status->setText("Bearer token copied."); });
    auto* close = button("Close", layout); connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    refresh(); dialog.exec();
}
}
