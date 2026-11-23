#include "app/AgentChatPanel.h"
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QTextBrowser>
#include <QToolButton>
#include <QVBoxLayout>

namespace sav {
namespace {

class PromptInput : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit PromptInput(QWidget* parent = nullptr) : QPlainTextEdit(parent) {}
signals:
    void submitted();
protected:
    void keyPressEvent(QKeyEvent* event) override {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            !(event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
            event->accept();
            emit submitted();
            return;
        }
        QPlainTextEdit::keyPressEvent(event);
    }
};

QString formatInlineMarkdown(QString text) {
    text = text.toHtmlEscaped();
    // Inline code spans first so bold markers inside code stay untouched.
    static const QRegularExpression codeRe("`([^`]+)`");
    QString out;
    qsizetype last = 0;
    auto it = codeRe.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        QString before = text.mid(last, m.capturedStart() - last);
        static const QRegularExpression boldRe("\\*\\*(.+?)\\*\\*");
        before.replace(boldRe, "<b>\\1</b>");
        out += before;
        out += "<code style='background:#101822;color:#b8d4ff;padding:1px 4px;border-radius:3px;font-family:Menlo,Consolas,monospace;'>"
             + m.captured(1) + "</code>";
        last = m.capturedEnd();
    }
    QString tail = text.mid(last);
    static const QRegularExpression boldRe("\\*\\*(.+?)\\*\\*");
    tail.replace(boldRe, "<b>\\1</b>");
    out += tail;
    return out;
}

QString markdownToHtml(const QString& markdown) {
    const QStringList lines = markdown.split('\n');
    QString html;
    bool inCode = false;
    QString codeBuffer;
    bool inList = false;

    auto closeList = [&] {
        if (inList) { html += "</ul>"; inList = false; }
    };

    for (int i = 0; i < lines.size(); ++i) {
        const QString& line = lines[i];
        if (line.trimmed().startsWith("```")) {
            closeList();
            if (!inCode) {
                inCode = true;
                codeBuffer.clear();
            } else {
                html += "<pre style='background:#0f1620;border:1px solid #28374c;border-radius:4px;padding:8px;"
                        "font-family:Menlo,Consolas,monospace;font-size:11px;color:#cfe0f7;white-space:pre-wrap;margin:6px 0;'>"
                      + codeBuffer.toHtmlEscaped() + "</pre>";
                inCode = false;
            }
            continue;
        }
        if (inCode) {
            if (!codeBuffer.isEmpty()) codeBuffer += '\n';
            codeBuffer += line;
            continue;
        }
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) {
            closeList();
            html += "<div style='height:5px;'></div>";
            continue;
        }
        if (trimmed.startsWith("- ") || trimmed.startsWith("* ")) {
            if (!inList) { html += "<ul style='margin:4px 0 4px 16px;padding:0;'>"; inList = true; }
            html += "<li style='margin:2px 0;'>" + formatInlineMarkdown(trimmed.mid(2)) + "</li>";
            continue;
        }
        closeList();
        if (trimmed.startsWith("### ")) {
            html += "<div style='font-weight:600;color:#c7daf5;margin:6px 0 2px 0;'>" + formatInlineMarkdown(trimmed.mid(4)) + "</div>";
        } else if (trimmed.startsWith("## ")) {
            html += "<div style='font-weight:600;color:#d6e6fb;font-size:13px;margin:8px 0 3px 0;'>" + formatInlineMarkdown(trimmed.mid(3)) + "</div>";
        } else {
            html += "<div>" + formatInlineMarkdown(line) + "</div>";
        }
    }
    closeList();
    if (inCode) {
        html += "<pre style='background:#0f1620;border:1px solid #28374c;border-radius:4px;padding:8px;"
                "font-family:Menlo,Consolas,monospace;font-size:11px;color:#cfe0f7;white-space:pre-wrap;margin:6px 0;'>"
              + codeBuffer.toHtmlEscaped() + "</pre>";
    }
    return html;
}

QPushButton* compactButton(const QString& text, QLayout* layout = nullptr) {
    auto* btn = new QPushButton(text);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFocusPolicy(Qt::TabFocus);
    btn->setStyleSheet("padding: 5px 9px;");
    if (layout) layout->addWidget(btn);
    return btn;
}

}

AgentChatPanel::AgentChatPanel(Controller& controller, McpServer& mcp, QWidget* parent)
    : QWidget(parent), c_(controller), mcp_(mcp), acp_(this) {
    setObjectName("agentChatPanel");
    setMinimumWidth(320);
    setStyleSheet("QWidget#agentChatPanel { background: #161f2c; border-left: 1px solid #2a3647; }");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 10, 12, 10);
    root->setSpacing(8);

    // Header row: title + status + close
    auto* header = new QHBoxLayout;
    header->setSpacing(6);
    auto* title = new QLabel("AGENT CHAT");
    title->setObjectName("section");
    header->addWidget(title);
    statusLabel_ = new QLabel("Disconnected");
    statusLabel_->setObjectName("agentStatusLabel");
    statusLabel_->setStyleSheet("color: #8499b5; font-size: 11px;");
    header->addWidget(statusLabel_, 1);
    auto* closeBtn = new QToolButton;
    closeBtn->setText("✕");
    closeBtn->setToolTip("Hide Agent chat panel");
    closeBtn->setFocusPolicy(Qt::TabFocus);
    closeBtn->setStyleSheet("padding: 2px 6px;");
    connect(closeBtn, &QToolButton::clicked, this, &AgentChatPanel::closeRequested);
    header->addWidget(closeBtn);
    root->addLayout(header);

    // Agent selector + connection controls
    auto* agentRow = new QHBoxLayout;
    agentRow->setSpacing(6);
    presetCombo_ = new QComboBox;
    presetCombo_->setObjectName("agentPresetCombo");
    presetCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    presetCombo_->setMinimumContentsLength(12);
    agentRow->addWidget(presetCombo_, 1);
    connectButton_ = compactButton("Connect", agentRow);
    connectButton_->setObjectName("agentConnectButton");
    newSessionButton_ = compactButton("New", agentRow);
    newSessionButton_->setObjectName("agentNewSessionButton");
    newSessionButton_->setToolTip("Start a fresh conversation session");
    root->addLayout(agentRow);

    customCommand_ = new QLineEdit;
    customCommand_->setObjectName("agentCustomCommand");
    customCommand_->setPlaceholderText("ACP command, e.g. opencode acp");
    customCommand_->setText(QSettings().value("agent/customCommand").toString());
    connect(customCommand_, &QLineEdit::editingFinished, this, [this] {
        QSettings().setValue("agent/customCommand", customCommand_->text().trimmed());
    });
    root->addWidget(customCommand_);

    presetHint_ = new QLabel;
    presetHint_->setWordWrap(true);
    presetHint_->setStyleSheet("color: #8499b5; font-size: 11px;");
    root->addWidget(presetHint_);

    // Guardrails row: Allow edits + Undo + MCP Settings + Logs
    auto* guardRow = new QHBoxLayout;
    guardRow->setSpacing(6);
    allowEditing_ = new QCheckBox("Allow show edits");
    allowEditing_->setObjectName("agentChatAllowEditing");
    allowEditing_->setToolTip("Allow the agent to edit this show and import media. Every batch is a single Undo step.");
    guardRow->addWidget(allowEditing_);
    guardRow->addStretch();
    undoButton_ = compactButton("Undo edit", guardRow);
    undoButton_->setObjectName("agentChatUndoButton");
    undoButton_->setToolTip("Undo the last edit on the show");
    undoButton_->hide();
    auto* mcpBtn = new QToolButton;
    mcpBtn->setText("MCP…");
    mcpBtn->setToolTip("Open external MCP server settings");
    mcpBtn->setFocusPolicy(Qt::TabFocus);
    mcpBtn->setStyleSheet("padding: 4px 7px;");
    connect(mcpBtn, &QToolButton::clicked, this, &AgentChatPanel::mcpSettingsRequested);
    guardRow->addWidget(mcpBtn);
    logsToggle_ = new QToolButton;
    logsToggle_->setText("Logs");
    logsToggle_->setCheckable(true);
    logsToggle_->setToolTip("Show raw ACP agent diagnostics (stderr)");
    logsToggle_->setFocusPolicy(Qt::TabFocus);
    logsToggle_->setStyleSheet("padding: 4px 7px;");
    guardRow->addWidget(logsToggle_);
    root->addLayout(guardRow);

    logsView_ = new QPlainTextEdit;
    logsView_->setReadOnly(true);
    logsView_->setMaximumHeight(110);
    logsView_->setStyleSheet("background: #0f1620; color: #9ab0cc; font-family: Menlo, Consolas, monospace; font-size: 11px; border: 1px solid #28374c;");
    logsView_->hide();
    connect(logsToggle_, &QToolButton::toggled, logsView_, &QWidget::setVisible);
    root->addWidget(logsView_);

    // Transcript
    transcript_ = new QTextBrowser;
    transcript_->setObjectName("agentChatTranscript");
    transcript_->setOpenExternalLinks(false);
    transcript_->setStyleSheet("QTextBrowser { background: #121a25; border: 1px solid #263448; border-radius: 5px; padding: 6px; color: #dee7f3; }");
    root->addWidget(transcript_, 1);

    // Quick-start prompt chips
    quickPromptsBox_ = new QWidget;
    auto* chips = new QHBoxLayout(quickPromptsBox_);
    chips->setContentsMargins(0, 0, 0, 0);
    chips->setSpacing(6);
    for (const auto& [label, prompt] : std::initializer_list<std::pair<const char*, const char*>>{
            {"Check readiness", "Run get_preflight and get_show, then summarize any issues or disconnected tracks before rehearsal."},
            {"Cue sheet", "Read the current show with get_show and summarize all cues, holds, and timing as a concise cue sheet."},
            {"Inspect frame", "Call render_preview at the current playhead and describe the visual composition and any layout issues."}}) {
        auto* chip = compactButton(label, chips);
        chip->setStyleSheet("padding: 4px 8px; font-size: 11px; background: #1e2a3b; border-color: #2f4058;");
        connect(chip, &QPushButton::clicked, this, [this, text = QString::fromUtf8(prompt)] {
            if (acp_.state() == AcpState::Ready) sendPromptText(text);
            else { promptInput_->setPlainText(text); promptInput_->setFocus(); }
        });
    }
    chips->addStretch();
    root->addWidget(quickPromptsBox_);

    // Pending permission request area
    permissionBox_ = new QWidget;
    permissionBox_->setObjectName("agentPermissionBox");
    permissionLayout_ = new QVBoxLayout(permissionBox_);
    permissionLayout_->setContentsMargins(0, 0, 0, 0);
    permissionLayout_->setSpacing(6);
    permissionBox_->hide();
    root->addWidget(permissionBox_);

    // Composer
    auto* composer = new QHBoxLayout;
    composer->setSpacing(6);
    auto* input = new PromptInput;
    input->setObjectName("agentPromptInput");
    input->setPlaceholderText("Ask the agent about this show… (Enter to send, Shift+Enter for newline)");
    input->setFixedHeight(64);
    input->setStyleSheet("QPlainTextEdit { background: #141d28; border: 1px solid #35445b; border-radius: 4px; padding: 6px; color: #dee7f3; }");
    promptInput_ = input;
    composer->addWidget(promptInput_, 1);
    sendButton_ = compactButton("Send", composer);
    sendButton_->setObjectName("agentSendButton");
    sendButton_->setMinimumWidth(68);
    sendButton_->setFixedHeight(64);
    composer->addWidget(sendButton_);
    root->addLayout(composer);

    // Coalesce high-frequency streaming chunks into smooth UI updates.
    renderTimer_.setSingleShot(true);
    renderTimer_.setInterval(30);
    connect(&renderTimer_, &QTimer::timeout, this, &AgentChatPanel::renderTranscript);

    populatePresets();

    connect(presetCombo_, &QComboBox::currentIndexChanged, this, [this] {
        if (const int idx = presetCombo_->currentIndex(); idx >= 0 && idx < static_cast<int>(presets_.size()))
            QSettings().setValue("agent/preset", presets_[idx].id);
        updatePresetUi();
    });
    connect(connectButton_, &QPushButton::clicked, this, &AgentChatPanel::toggleConnection);
    connect(newSessionButton_, &QPushButton::clicked, this, &AgentChatPanel::startNewSession);
    connect(input, &PromptInput::submitted, this, &AgentChatPanel::sendCurrentPrompt);
    connect(sendButton_, &QPushButton::clicked, this, [this] {
        if (acp_.state() == AcpState::Prompting) acp_.cancelPrompt();
        else sendCurrentPrompt();
    });
    connect(undoButton_, &QPushButton::clicked, this, [this] {
        if (!c_.canEdit()) return;
        c_.undo();
        appendEntry({ChatEntry::Kind::System, {}, {}, {}, "Reverted last show edit (Undo).", {}, {}});
    });
    connect(allowEditing_, &QCheckBox::toggled, this, [this](bool on) {
        wantEditing_ = on;
        if (mcp_.running()) {
            emit aboutToEdit();
            mcp_.setEditingEnabled(on);
            c_.report(on ? "Agent editing enabled. Changes can be undone from Edit." : "Agent collaboration is read only.");
        }
        updateControls();
    });

    // ACP client signals
    connect(&acp_, &AcpClient::stateChanged, this, [this](AcpState) { updateControls(); });
    connect(&acp_, &AcpClient::connected, this, [this](const QString& name, const QString&) {
        sentContextPreamble_ = false;
        const QString transport = acp_.usedHttpMcp() ? "HTTP MCP" : "stdio MCP bridge";
        appendEntry({ChatEntry::Kind::System, {}, {}, {},
            QString("Connected to %1 (%2 · %3).").arg(name, transport, mcp_.editingEnabled() ? "editing enabled" : "read only"), {}, {}});
        updateControls();
        focusPrompt();
    });
    connect(&acp_, &AcpClient::messageChunk, this, [this](const QString& chunk) {
        if (entries_.empty() || entries_.back().kind != ChatEntry::Kind::Assistant)
            entries_.push_back({ChatEntry::Kind::Assistant, {}, acp_.agentName().isEmpty() ? QString("Agent") : acp_.agentName(), {}, {}, {}, {}});
        entries_.back().text += chunk;
        scheduleRender();
    });
    connect(&acp_, &AcpClient::thoughtChunk, this, [this](const QString& chunk) {
        if (entries_.empty() || entries_.back().kind != ChatEntry::Kind::Assistant)
            entries_.push_back({ChatEntry::Kind::Assistant, {}, acp_.agentName().isEmpty() ? QString("Agent") : acp_.agentName(), {}, {}, {}, {}});
        entries_.back().thought += chunk;
        scheduleRender();
    });
    connect(&acp_, &AcpClient::toolCallUpdated, this, [this](const AcpToolCallUpdate& update) {
        for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
            if (it->kind == ChatEntry::Kind::ToolCall && !update.toolCallId.isEmpty() && it->id == update.toolCallId) {
                if (!update.title.isEmpty()) it->title = update.title;
                if (!update.kind.isEmpty()) it->meta = update.kind;
                if (!update.status.isEmpty()) it->status = update.status;
                if (!update.detail.isEmpty()) it->text = update.detail;
                scheduleRender();
                return;
            }
        }
        entries_.push_back({ChatEntry::Kind::ToolCall, update.toolCallId,
            update.title.isEmpty() ? QString("Tool call") : update.title,
            update.kind, update.detail, {}, update.status.isEmpty() ? QString("in_progress") : update.status});
        scheduleRender();
    });
    connect(&acp_, &AcpClient::permissionRequested, this, &AgentChatPanel::addPermissionCard);
    connect(&acp_, &AcpClient::promptFinished, this, [this](const QString& reason) {
        clearPermissionCards();
        if (reason == "cancelled") appendEntry({ChatEntry::Kind::System, {}, {}, {}, "Turn cancelled.", {}, {}});
        updateControls();
        scheduleRender();
    });
    connect(&acp_, &AcpClient::stderrReceived, this, [this](const QString& text) {
        logsView_->moveCursor(QTextCursor::End);
        logsView_->insertPlainText(text);
        logsView_->moveCursor(QTextCursor::End);
    });
    connect(&acp_, &AcpClient::errorOccurred, this, [this](const QString& message) {
        clearPermissionCards();
        appendEntry({ChatEntry::Kind::Error, {}, {}, {}, message, {}, {}});
        updateControls();
    });

    // Sync with MCP server and controller state
    connect(&mcp_, &McpServer::stateChanged, this, [this] {
        if (!mcp_.running() && acp_.state() != AcpState::Disconnected) {
            autoStartedMcp_ = false;
            acp_.disconnectFromAgent();
            appendEntry({ChatEntry::Kind::System, {}, {}, {}, "Disconnected because the local MCP server was stopped.", {}, {}});
        }
        updateControls();
    });
    connect(&mcp_, &McpServer::showModified, this, [this](const QString& summary) {
        hasAgentEdit_ = true;
        appendEntry({ChatEntry::Kind::System, {}, {}, {}, summary, {}, {}});
        updateControls();
    });
    connect(&c_, &Controller::changed, this, [this] { updateControls(); });
    connect(&c_, &Controller::tick, this, [this] {
        const bool editable = c_.canEdit(false);
        if (editable != lastCanEdit_) {
            lastCanEdit_ = editable;
            updateControls();
        }
    });

    updatePresetUi();
    updateControls();
    renderTranscript();
}

AgentChatPanel::~AgentChatPanel() {
    shutdown();
}

void AgentChatPanel::shutdown() {
    clearPermissionCards();
    acp_.disconnectFromAgent();
    if (autoStartedMcp_ && mcp_.running()) {
        autoStartedMcp_ = false;
        mcp_.stop();
    }
}

void AgentChatPanel::focusPrompt() {
    promptInput_->setFocus();
}

void AgentChatPanel::populatePresets() {
    presets_ = defaultAcpPresets();
    QSignalBlocker block(presetCombo_);
    presetCombo_->clear();
    const QString savedId = QSettings().value("agent/preset").toString();
    int firstAvailable = -1;
    int savedIndex = -1;
    for (int i = 0; i < static_cast<int>(presets_.size()); ++i) {
        const auto& p = presets_[i];
        const bool available = !p.program.isEmpty() && !findAcpExecutable(p.program).isEmpty();
        if (available && firstAvailable < 0) firstAvailable = i;
        if (p.id == savedId) savedIndex = i;
        const QString label = p.id == "custom" ? p.name : (available ? p.name + " · installed" : p.name + " · not found");
        presetCombo_->addItem(label, p.id);
    }
    if (savedIndex >= 0) presetCombo_->setCurrentIndex(savedIndex);
    else if (firstAvailable >= 0) presetCombo_->setCurrentIndex(firstAvailable);
    else presetCombo_->setCurrentIndex(0);
}

void AgentChatPanel::updatePresetUi() {
    const int idx = presetCombo_->currentIndex();
    if (idx < 0 || idx >= static_cast<int>(presets_.size())) return;
    const auto& preset = presets_[idx];
    const bool isCustom = preset.id == "custom";
    customCommand_->setVisible(isCustom);
    if (isCustom) {
        presetHint_->setText(preset.installHint);
        presetHint_->setVisible(true);
    } else {
        const bool found = !findAcpExecutable(preset.program).isEmpty();
        presetHint_->setText(found ? QString() : preset.installHint);
        presetHint_->setVisible(!found);
    }
}

void AgentChatPanel::updateControls() {
    const AcpState st = acp_.state();
    const bool active = st == AcpState::Connecting || st == AcpState::Ready || st == AcpState::Prompting;
    const bool ready = st == AcpState::Ready;
    const bool prompting = st == AcpState::Prompting;
    const bool editable = c_.canEdit(false);
    lastCanEdit_ = editable;

    QString statusText;
    QString statusColor = "#8499b5";
    switch (st) {
    case AcpState::Disconnected: statusText = "Disconnected"; break;
    case AcpState::Connecting: statusText = "Connecting…"; statusColor = "#e5c47d"; break;
    case AcpState::Ready:
        statusText = (acp_.agentName().isEmpty() ? QString("Ready") : acp_.agentName()) +
            (mcp_.editingEnabled() ? (editable ? " · editing" : " · locked") : " · read only");
        statusColor = "#7fd19b";
        break;
    case AcpState::Prompting: statusText = "Working…"; statusColor = "#8fb4f0"; break;
    case AcpState::Error: statusText = "Error"; statusColor = "#e3adb3"; break;
    }
    statusLabel_->setText(statusText);
    statusLabel_->setStyleSheet(QString("color: %1; font-size: 11px; font-weight: 600;").arg(statusColor));

    presetCombo_->setEnabled(!active);
    customCommand_->setEnabled(!active);
    connectButton_->setText(active ? "Disconnect" : "Connect");
    newSessionButton_->setEnabled(ready || prompting);

    {
        QSignalBlocker block(allowEditing_);
        allowEditing_->setChecked(mcp_.running() ? mcp_.editingEnabled() : wantEditing_);
    }
    allowEditing_->setEnabled(!c_.showMode);
    allowEditing_->setToolTip(c_.showMode
        ? "Show mode locks editing. Leave Show mode to allow agent edits."
        : "Allow the agent to edit this show and import media. Every batch is one Undo step.");

    undoButton_->setVisible(hasAgentEdit_);
    undoButton_->setEnabled(editable);

    promptInput_->setEnabled(ready || prompting);
    sendButton_->setEnabled(ready || prompting);
    sendButton_->setText(prompting ? "Stop" : "Send");
    sendButton_->setProperty("role", prompting ? "danger" : "primary");
    sendButton_->setStyleSheet(prompting
        ? "background: #3b2630; border: 1px solid #67414e; color: #e3adb3; padding: 5px 9px; font-weight: 600;"
        : (ready ? "background: #5d84cf; border: 1px solid #739aee; color: #fff; padding: 5px 9px; font-weight: 600;"
                 : "background: #1a2432; border: 1px solid #2d3d51; color: #607188; padding: 5px 9px;"));

    bool hasUserMessages = false;
    for (const auto& e : entries_) if (e.kind == ChatEntry::Kind::User) { hasUserMessages = true; break; }
    quickPromptsBox_->setVisible(!hasUserMessages && !prompting);
}

void AgentChatPanel::toggleConnection() {
    if (acp_.state() == AcpState::Connecting || acp_.state() == AcpState::Ready || acp_.state() == AcpState::Prompting) {
        clearPermissionCards();
        acp_.disconnectFromAgent();
        if (autoStartedMcp_ && mcp_.running()) {
            autoStartedMcp_ = false;
            mcp_.stop();
        }
        appendEntry({ChatEntry::Kind::System, {}, {}, {}, "Disconnected.", {}, {}});
        updateControls();
        return;
    }

    const int idx = presetCombo_->currentIndex();
    if (idx < 0 || idx >= static_cast<int>(presets_.size())) return;
    const auto& preset = presets_[idx];
    QString program;
    QStringList args;
    if (preset.id == "custom") {
        const QString cmd = customCommand_->text().trimmed();
        QSettings().setValue("agent/customCommand", cmd);
        const QStringList parts = splitCommandLine(cmd);
        if (parts.isEmpty()) {
            appendEntry({ChatEntry::Kind::Error, {}, {}, {}, "Enter an ACP command first (for example: opencode acp).", {}, {}});
            return;
        }
        program = parts.front();
        args = parts.mid(1);
    } else {
        program = preset.program;
        args = preset.args;
    }

    // Ensure the local MCP server is running so the ACP session receives live show tools automatically.
    if (!mcp_.running()) {
        emit aboutToEdit();
        if (!mcp_.start(48123) && !mcp_.start(0)) {
            appendEntry({ChatEntry::Kind::Error, {}, {}, {}, "Could not start local MCP server: " + mcp_.errorString(), {}, {}});
            return;
        }
        autoStartedMcp_ = true;
    }
    if (wantEditing_ && !c_.showMode && !mcp_.editingEnabled()) {
        emit aboutToEdit();
        mcp_.setEditingEnabled(true);
    }

    AcpMcpConfig mcpConfig;
    mcpConfig.name = "simpleavshow";
    mcpConfig.url = mcp_.endpoint();
    mcpConfig.token = mcp_.token();
    mcpConfig.proxyExecutable = QCoreApplication::applicationFilePath();

    logsView_->clear();
    acp_.connectToAgent(program, args, currentWorkingDirectory(), mcpConfig);
    updateControls();
}

void AgentChatPanel::startNewSession() {
    if (acp_.state() != AcpState::Ready && acp_.state() != AcpState::Prompting) return;
    clearPermissionCards();
    entries_.clear();
    sentContextPreamble_ = false;
    hasAgentEdit_ = false;
    AcpMcpConfig mcpConfig{"simpleavshow", mcp_.endpoint(), mcp_.token(), QCoreApplication::applicationFilePath()};
    acp_.startNewSession(currentWorkingDirectory(), mcpConfig);
    renderTranscript();
    updateControls();
}

void AgentChatPanel::sendCurrentPrompt() {
    if (acp_.state() != AcpState::Ready) return;
    const QString text = promptInput_->toPlainText().trimmed();
    if (text.isEmpty()) return;
    promptInput_->clear();
    sendPromptText(text);
}

void AgentChatPanel::sendPromptText(const QString& userText) {
    if (acp_.state() != AcpState::Ready || userText.trimmed().isEmpty()) return;
    appendEntry({ChatEntry::Kind::User, {}, "You", {}, userText, {}, {}});
    QString payload = userText;
    if (!sentContextPreamble_) {
        payload = buildFirstTurnPrompt(userText);
        sentContextPreamble_ = true;
    }
    acp_.sendPrompt(payload);
    updateControls();
}

QString AgentChatPanel::currentWorkingDirectory() const {
    if (!c_.path.isEmpty()) return QFileInfo(c_.path).absolutePath();
    return QDir::homePath();
}

QString AgentChatPanel::buildFirstTurnPrompt(const QString& userText) const {
    return QString(
        "[SimpleAVShow embedded assistant context]\n"
        "You are running inside SimpleAVShow's Agent Chat panel with the `simpleavshow` MCP server attached.\n"
        "- Call `get_show` to inspect the open show, tracks, clips, cues, media assets, and revision.\n"
        "- Call `get_preflight` to check show readiness, disconnected tracks, missing media, and unused assets.\n"
        "- Call `render_preview` to visually inspect the composed program picture at any timeline frame.\n"
        "- When the operator enables editing, use `apply_edits` (project, track, clip, marker, split_clip, delete_clip, delete_marker, delete_track, delete_asset) and `import_media` / `get_import` / `commit_import`.\n"
        "- All timeline and automation times are 48,000 Hz audio frames (`seconds * 48000`). The operator controls playback, Show mode, outputs, and saving.\n"
        "- Keep replies concise and practical for a live-show operator.\n\n"
        "%1").arg(userText);
}

void AgentChatPanel::appendEntry(ChatEntry entry) {
    entries_.push_back(std::move(entry));
    scheduleRender();
}

void AgentChatPanel::scheduleRender() {
    if (!renderTimer_.isActive()) renderTimer_.start();
}

void AgentChatPanel::renderTranscript() {
    auto* bar = transcript_->verticalScrollBar();
    const bool stickToBottom = !bar || bar->value() >= bar->maximum() - 24;

    QString html = "<html><body style='margin:0;padding:2px;color:#dee7f3;font-family:\"Segoe UI\",\"Helvetica Neue\",Arial;font-size:12px;'>";
    if (entries_.empty()) {
        html += "<div style='color:#8fa6c4;padding:12px 8px;line-height:1.45;'>"
                "<div style='font-weight:600;color:#c7daf5;margin-bottom:6px;'>Co-edit this show with an ACP agent</div>"
                "<div>Choose an installed ACP agent above (such as OpenCode, Claude Code, Codex, Gemini CLI, or Pi) and click <b>Connect</b>. "
                "SimpleAVShow automatically connects its local MCP tools—no JSON or bearer tokens to copy.</div>"
                "<div style='margin-top:8px;color:#7890b0;'>Starts in read-only mode unless <b>Allow show edits</b> is checked. "
                "Every agent edit batch can be reversed with a single Undo.</div>"
                "</div>";
    } else {
        for (const auto& entry : entries_) {
            switch (entry.kind) {
            case ChatEntry::Kind::User:
                html += "<div style='background:#21344f;border:1px solid #355075;border-radius:6px;padding:8px 10px;margin:6px 0;'>"
                        "<div style='font-size:10px;font-weight:600;color:#9bc0f5;margin-bottom:3px;'>YOU</div>"
                        "<div>" + entry.text.toHtmlEscaped().replace('\n', "<br>") + "</div></div>";
                break;
            case ChatEntry::Kind::Assistant:
                html += "<div style='background:#182332;border:1px solid #28384e;border-radius:6px;padding:8px 10px;margin:6px 0;'>"
                        "<div style='font-size:10px;font-weight:600;color:#7fd19b;margin-bottom:3px;'>"
                      + entry.title.toUpper().toHtmlEscaped() + "</div>";
                if (!entry.thought.isEmpty() && entry.text.isEmpty()) {
                    html += "<div style='color:#7d93b0;font-style:italic;font-size:11px;margin-bottom:4px;'>"
                          + entry.thought.toHtmlEscaped().replace('\n', "<br>") + "</div>";
                }
                html += markdownToHtml(entry.text) + "</div>";
                break;
            case ChatEntry::Kind::ToolCall: {
                const QString icon = entry.status == "completed" ? "✓" : entry.status == "failed" ? "✕" : "⋯";
                const QString color = entry.status == "completed" ? "#7fd19b" : entry.status == "failed" ? "#e3adb3" : "#e5c47d";
                html += "<div style='background:#131c28;border-left:3px solid " + color + ";padding:5px 8px;margin:4px 0;font-size:11px;'>"
                        "<span style='color:" + color + ";font-weight:600;'>" + icon + " " + entry.title.toHtmlEscaped() + "</span>";
                if (!entry.meta.isEmpty()) html += " <span style='color:#7890b0;'>· " + entry.meta.toHtmlEscaped() + "</span>";
                if (!entry.text.isEmpty()) {
                    QString snippet = entry.text.simplified();
                    if (snippet.size() > 180) snippet = snippet.left(180) + "…";
                    html += "<div style='color:#8ea4c2;font-family:Menlo,Consolas,monospace;font-size:10px;margin-top:2px;'>"
                          + snippet.toHtmlEscaped() + "</div>";
                }
                html += "</div>";
                break;
            }
            case ChatEntry::Kind::System:
                html += "<div style='color:#8fa6c4;font-size:11px;padding:3px 4px;margin:3px 0;'>• "
                      + entry.text.toHtmlEscaped() + "</div>";
                break;
            case ChatEntry::Kind::Error:
                html += "<div style='background:#3b2630;border:1px solid #67414e;border-radius:5px;padding:7px 9px;margin:6px 0;color:#ffd7d2;font-size:11px;'>"
                        "<b>Error:</b> " + entry.text.toHtmlEscaped() + "</div>";
                break;
            }
        }
    }
    html += "</body></html>";
    transcript_->setHtml(html);
    if (stickToBottom && bar) bar->setValue(bar->maximum());
}

void AgentChatPanel::addPermissionCard(const AcpPermissionRequest& request) {
    auto* card = new QFrame(permissionBox_);
    card->setObjectName(QString("permissionCard_%1").arg(request.requestKey));
    card->setStyleSheet("QFrame { background: #262118; border: 1px solid #8a6d3b; border-radius: 6px; padding: 6px; }");
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    auto* heading = new QLabel("Permission requested: " + request.title);
    heading->setWordWrap(true);
    heading->setStyleSheet("color: #f2d38b; font-weight: 600; border: none; background: transparent;");
    layout->addWidget(heading);

    if (!request.detail.isEmpty()) {
        QString detail = request.detail;
        if (detail.size() > 260) detail = detail.left(260) + "…";
        auto* detailLabel = new QLabel(detail);
        detailLabel->setWordWrap(true);
        detailLabel->setStyleSheet("color: #d6c8a8; font-family: Menlo, Consolas, monospace; font-size: 11px; border: none; background: transparent;");
        layout->addWidget(detailLabel);
    }

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(6);
    auto resolve = [this, card, key = request.requestKey, title = request.title](const QString& optionId, const QString& label) {
        acp_.respondPermission(key, optionId);
        permissionLayout_->removeWidget(card);
        card->deleteLater();
        appendEntry({ChatEntry::Kind::System, {}, {}, {}, QString("Permission for “%1”: %2.").arg(title, label), {}, {}});
        if (permissionLayout_->count() == 0) permissionBox_->hide();
    };
    for (const auto& opt : request.options) {
        auto* btn = compactButton(opt.name, buttons);
        btn->setProperty("optionId", opt.optionId);
        if (opt.kind.startsWith("allow")) btn->setObjectName("primary");
        else if (opt.kind.startsWith("reject")) btn->setObjectName("danger");
        connect(btn, &QPushButton::clicked, this, [resolve, id = opt.optionId, name = opt.name] { resolve(id, name); });
    }
    auto* cancelBtn = compactButton("Cancel", buttons);
    cancelBtn->setObjectName("permissionCancelButton");
    connect(cancelBtn, &QPushButton::clicked, this, [resolve] { resolve({}, "Cancelled"); });
    buttons->addStretch();
    layout->addLayout(buttons);

    permissionLayout_->addWidget(card);
    permissionBox_->show();
}

void AgentChatPanel::clearPermissionCards() {
    while (auto* item = permissionLayout_->takeAt(0)) {
        if (auto* w = item->widget()) w->deleteLater();
        delete item;
    }
    permissionBox_->hide();
}

}
#include "AgentChatPanel.moc"
