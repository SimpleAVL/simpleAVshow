#pragma once
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <memory>
#include <vector>

namespace sav {

struct AcpAgentPreset {
    QString id;
    QString name;
    QString program;
    QStringList args;
    QString installHint;
};

struct AcpMcpConfig {
    QString name = "simpleavshow";
    QString url;
    QString token;
    // Executable used for the built-in --mcp-proxy stdio bridge when the agent lacks HTTP MCP support.
    QString proxyExecutable;
};

struct AcpPermissionOption {
    QString optionId;
    QString name;
    QString kind;
};

struct AcpPermissionRequest {
    int requestKey = 0;
    QString toolCallId;
    QString title;
    QString kind;
    QString detail;
    std::vector<AcpPermissionOption> options;
};

struct AcpToolCallUpdate {
    QString toolCallId;
    QString title;
    QString kind;
    QString status;
    QString detail;
};

enum class AcpState {
    Disconnected,
    Connecting,
    Ready,
    Prompting,
    Error
};

// Known ACP-compatible coding/assistant agents and helper lookup utilities.
std::vector<AcpAgentPreset> defaultAcpPresets();
QString findAcpExecutable(const QString& program);
QStringList splitCommandLine(const QString& commandLine);
bool validateBatchWrapperArgs(const QString& executable, const QStringList& args, QString& error);

// A native Qt6 Agent Client Protocol (ACP) client communicating via newline-delimited
// JSON-RPC 2.0 over QProcess stdin/stdout.
class AcpClient : public QObject {
    Q_OBJECT
public:
    explicit AcpClient(QObject* parent = nullptr);
    ~AcpClient() override;

    AcpState state() const { return state_; }
    QString agentName() const { return agentName_; }
    QString agentVersion() const { return agentVersion_; }
    QString sessionId() const { return sessionId_; }
    QString lastError() const { return lastError_; }
    bool usedHttpMcp() const { return usedHttpMcp_; }
    bool agentSupportsHttpMcp() const { return agentSupportsHttpMcp_; }

    void connectToAgent(const QString& program, const QStringList& args, const QString& cwd, const AcpMcpConfig& mcp);
    void startNewSession(const QString& cwd, const AcpMcpConfig& mcp);
    void sendPrompt(const QString& text);
    void cancelPrompt();
    void respondPermission(int requestKey, const QString& optionId);
    void disconnectFromAgent();
    void setTimeoutsForTesting(int startupMs, int handshakeMs, int cancelMs);

signals:
    void stateChanged(sav::AcpState state);
    void connected(const QString& agentName, const QString& sessionId);
    void messageChunk(const QString& text);
    void thoughtChunk(const QString& text);
    void toolCallUpdated(const sav::AcpToolCallUpdate& update);
    void planUpdated(const QStringList& entries);
    void permissionRequested(const sav::AcpPermissionRequest& request);
    void promptFinished(const QString& stopReason);
    void stderrReceived(const QString& text);
    void errorOccurred(const QString& message);

private:
    enum class PendingKind { Initialize, SessionClose, SessionNew, Prompt };
    struct PendingRequest {
        PendingKind kind = PendingKind::Initialize;
        QString sessionId;
    };
    struct PendingPermission {
        QJsonValue rpcId;
        QString sessionId;
    };
    struct KnownToolCall {
        QString title;
        QString kind;
        QString rawInput;
        QString content;
        bool rawInputTooLarge = false;
        bool contentTooLarge = false;
    };

    struct ProcessTreeHandle;
    struct RetiringProcess {
        QPointer<QProcess> process;
        std::shared_ptr<ProcessTreeHandle> tree;
    };

    QProcess* process_ = nullptr;
    std::shared_ptr<ProcessTreeHandle> processTree_;
    AcpState state_ = AcpState::Disconnected;
    QString agentName_, agentVersion_, sessionId_, lastError_;
    QString program_;
    QStringList args_;
    QString cwd_;
    AcpMcpConfig mcp_;
    bool agentSupportsHttpMcp_ = false;
    bool agentSupportsSessionClose_ = false;
    bool usedHttpMcp_ = false;
    bool stopping_ = false;
    bool cancelling_ = false;
    bool stdoutDrainScheduled_ = false;
    bool stderrDrainScheduled_ = false;
    QByteArray stdoutBuffer_;
    int nextId_ = 1;
    int nextPermissionKey_ = 1;
    QMap<int, PendingRequest> pending_;
    QMap<int, PendingPermission> pendingPermissions_;
    QHash<QString, KnownToolCall> knownToolCalls_;
    QStringList knownToolCallOrder_;
    qsizetype knownToolCallsBytes_ = 0;
    std::vector<RetiringProcess> retiringProcesses_;
    std::vector<std::shared_ptr<ProcessTreeHandle>> retiringTrees_;
    QTimer deadlineTimer_;
    QTimer cancelTimer_;
    QString deadlineReason_;
    int startupTimeoutMs_ = 5000;
    int handshakeTimeoutMs_ = 15000;
    int cancelTimeoutMs_ = 5000;

    void setState(AcpState next);
    void armDeadline(int ms, const QString& reason);
    void launchProcess(bool emitDisconnectedOnReset);
    void teardownProcess(bool blocking);
    void scheduleTreeCleanup(std::shared_ptr<ProcessTreeHandle> tree);
    void sendJson(const QJsonObject& payload);
    int sendRequest(const QString& method, const QJsonObject& params, PendingKind kind, const QString& sessionId = {});
    void sendNotification(const QString& method, const QJsonObject& params);
    void sendResponse(const QJsonValue& id, const QJsonObject& result);
    void sendErrorResponse(const QJsonValue& id, int code, const QString& message);
    void handleStdout();
    void handleStderr();
    void handleLine(const QByteArray& line);
    void handleResponse(int id, const QJsonObject& msg);
    void handleNotification(const QString& method, const QJsonObject& params);
    void handleAgentRequest(const QJsonValue& id, const QString& method, const QJsonObject& params);
    QJsonArray buildMcpServers();
    void cancelAllPendingPermissions(const QString& onlySessionId = {});
    void failConnection(const QString& message);
};

}
