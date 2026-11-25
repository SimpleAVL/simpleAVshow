#pragma once
#include "app/AcpClient.h"
#include "app/Controller.h"
#include "app/McpServer.h"
#include <QTimer>
#include <QWidget>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QTextBrowser;
class QToolButton;
class QVBoxLayout;

namespace sav {

class AgentChatPanel : public QWidget {
    Q_OBJECT
public:
    AgentChatPanel(Controller& controller, McpServer& mcp, QWidget* parent = nullptr);
    ~AgentChatPanel() override;

    AcpClient& acpClient() { return acp_; }
    void shutdown();
    void focusPrompt();

signals:
    void closeRequested();
    void mcpSettingsRequested();
    void aboutToEdit();

protected:
    void showEvent(QShowEvent* event) override;

private:
    struct ChatEntry {
        enum class Kind { User, Assistant, ToolCall, System, Error };
        Kind kind = Kind::System;
        QString id;
        QString title;
        QString meta;
        QString text;
        QString thought;
        QString status;
        quint64 sessionGeneration = 0;
    };

    Controller& c_;
    McpServer& mcp_;
    AcpClient acp_;
    std::vector<AcpAgentPreset> presets_;
    std::vector<ChatEntry> entries_;
    bool autoStartedMcp_ = false;
    bool connecting_ = false;
    bool sentContextPreamble_ = false;
    int agentUndoCount_ = 0;
    int preChangeAgentUndoCount_ = 0;
    int mcpBatchStartUndoCount_ = 0;
    int prevUndoCount_ = 0;
    int lastUndoDelta_ = 0;
    quint64 lastHistoryEpoch_ = 0;
    quint64 mcpBatchStartHistoryEpoch_ = 0;
    quint64 sessionGeneration_ = 0;
    bool undoingAgent_ = false;
    bool lastCanEdit_ = true;
    bool shuttingDown_ = false;
    bool renderPending_ = false;
    bool entriesTrimmed_ = false;
    bool logsPending_ = false;
    QString pendingLogs_;
    QString chatMcpToken_;

    QLabel* statusLabel_ = nullptr;
    QComboBox* presetCombo_ = nullptr;
    QLineEdit* customCommand_ = nullptr;
    QLabel* presetHint_ = nullptr;
    QPushButton* connectButton_ = nullptr;
    QPushButton* newSessionButton_ = nullptr;
    // The session's model and thinking level, when the agent offers them; remembered per preset.
    QWidget* sessionSettings_ = nullptr;
    QComboBox* modelCombo_ = nullptr;
    QComboBox* thinkingCombo_ = nullptr;
    QString activePresetId_;
    QStringList savedSettingsToApply_; // Categories still to restore for the new session, in order.
    void refreshSessionSettings();
    void applyNextSavedSetting();
    void chooseSessionSetting(const QString& category, QComboBox* combo);
    QCheckBox* allowEditing_ = nullptr;
    QPushButton* undoButton_ = nullptr;
    QToolButton* logsToggle_ = nullptr;
    QPlainTextEdit* logsView_ = nullptr;
    QWidget* quickPromptsBox_ = nullptr;
    QTextBrowser* transcript_ = nullptr;
    QScrollArea* permissionScroll_ = nullptr;
    QWidget* permissionBox_ = nullptr;
    QVBoxLayout* permissionLayout_ = nullptr;
    QPlainTextEdit* promptInput_ = nullptr;
    QPushButton* sendButton_ = nullptr;
    QTimer renderTimer_;
    QTimer logsTimer_;

    void populatePresets();
    void updatePresetUi();
    void updateControls();
    void toggleConnection();
    void startNewSession();
    void sendCurrentPrompt();
    void sendPromptText(const QString& userText);
    void scheduleRender();
    void renderTranscript();
    void flushLogs();
    void appendEntry(ChatEntry entry);
    void addPermissionCard(const AcpPermissionRequest& request);
    void clearPermissionCards();
    QString currentWorkingDirectory() const;
    QString buildFirstTurnPrompt(const QString& userText) const;
};

}
