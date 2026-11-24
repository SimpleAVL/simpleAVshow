#pragma once
#include "app/Controller.h"
#include <QTcpServer>
#include <QJsonArray>
#include <atomic>
#include <functional>
#include <memory>

namespace sav {
// Optional UI/window hooks supplied by MainWindow so MCP tools can inspect and control
// displays, timeline selection, and the phone remote while staying testable headlessly.
struct McpHost {
    std::function<QJsonObject()> displayStatus;
    std::function<bool(int screen, bool allowCoverEditor, QString& error)> openDisplay;
    std::function<void()> closeDisplay;
    std::function<QJsonObject()> remoteStatus;
    std::function<bool(bool enable, int port, bool changeKey, QString& error)> configureRemote;
    std::function<void(int track, const QString& clipId, const QString& markerId)> selectItem;
    std::function<QJsonObject()> currentSelection;
};

// A local, opt-in Streamable HTTP MCP endpoint. All project access stays on the GUI thread.
class McpServer : public QObject {
    Q_OBJECT
public:
    explicit McpServer(Controller& controller, QObject* parent = nullptr);
    void setHost(McpHost host) { host_ = std::move(host); }
    bool start(quint16 port = 48123);
    void stop();
    bool running() const { return server_.isListening(); }
    QString endpoint() const;
    QString token() const { return token_; }
    // Issues a revocable bearer token for an embedded chat session without affecting external clients.
    QString issueChatToken();
    void revokeChatToken(const QString& token);
    bool externallyShared() const { return externallyShared_; }
    void markExternallyShared() { if (running()) externallyShared_ = true; }
    QString errorString() const { return server_.errorString(); }
    bool editingEnabled() const { return editing_; }
    void setEditingEnabled(bool enabled);
    QJsonObject clientConfig() const;
signals:
    void stateChanged();
    // Flush in-progress inspector fields before checking the caller's revision.
    void aboutToEdit();
    void showModified(const QString& summary);
private:
    Controller& c_;
    McpHost host_;
    QTcpServer server_;
    QString token_;
    QSet<QString> chatTokens_;
    QMap<QString, std::shared_ptr<std::atomic<bool>>> chatTokenCancelled_;
    QMap<QString, QString> issuedSessions_; // Mcp-Session-Id -> issuing bearer token
    QMap<QString, QStringList> bearerSessions_; // bearer token -> FIFO session IDs
    QStringList sessionOrder_; // global FIFO session IDs
    bool editing_ = false;
    bool externallyShared_ = false;
    quint64 revision_ = 0;
    quint64 generation_ = 0;
    quint64 editGeneration_ = 0;
    quint64 importShowEpoch_ = 0;
    QString importOwnerBearer_;
    std::shared_ptr<std::atomic<bool>> editSessionCancelled_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> importCancelled_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> previewCancelled_ = std::make_shared<std::atomic<bool>>(false);
    int activeAsyncJobs_ = 0;
    int activePreviewJobs_ = 0;
    struct InflightRequest {
        QString bearerToken;
        std::shared_ptr<std::atomic<bool>> cancelled;
        std::function<void()> abortConnection;
    };
    QMap<QString, InflightRequest> inflightRequests_;
    struct TrackedLease {
        quint64 lease = 0;
        std::weak_ptr<std::atomic<bool>> cancelled;
    };
    std::vector<TrackedLease> activeLeases_;
    void trackBusyLease(quint64 lease, const std::shared_ptr<std::atomic<bool>>& cancelled);
    void releaseBusyLease(quint64 lease);
    void releaseAllBusyLeases();
    void releaseCancelledBusyLeases();
    void invalidateEditSession();
    void invalidatePreviews();
    QString importId_, importState_, importError_;
    std::vector<MediaAsset> imported_;
    void acceptConnection();
    void dispatch(const QJsonObject& request, const QString& bearerToken, const QString& clientScope,
                  std::shared_ptr<std::atomic<bool>> cancelled,
                  std::function<void()> abortConnection, std::function<void(QJsonObject, QByteArray)> finish);
    void callTool(const QString& name, const QJsonObject& arguments, const QString& bearerToken,
                  std::shared_ptr<std::atomic<bool>> cancelled, std::function<void(QJsonObject)> finish);
    QJsonObject snapshot() const;
    QJsonObject environmentReport() const;
    QJsonObject preflightReport() const;
    void renderPreviewAsync(const QJsonObject& arguments, std::shared_ptr<std::atomic<bool>> cancelled, std::function<void(QJsonObject)> finish);
    QJsonObject appGuide(const QJsonObject& arguments) const;
    QJsonObject encodeMidiTool(const QJsonObject& arguments) const;
    void listMediaFilesAsync(const QJsonObject& arguments, std::shared_ptr<std::atomic<bool>> cancelled, std::function<void(QJsonObject)> finish);
    void relinkMediaAsync(const QJsonObject& arguments, std::shared_ptr<std::atomic<bool>> cancelled, std::function<void(QJsonObject)> finish);
    QJsonObject configureOutputsTool(const QJsonObject& arguments, const QString& bearerToken);
    QJsonObject configureRemoteTool(const QJsonObject& arguments);
    void projectActionAsync(const QJsonObject& arguments, std::shared_ptr<std::atomic<bool>> cancelled, std::function<void(QJsonObject)> finish);
    QJsonObject transportControlTool(const QJsonObject& arguments, const QString& bearerToken);
    QJsonObject historyTool(const QJsonObject& arguments);
    QJsonObject seekAndSelectTool(const QJsonObject& arguments, const QString& bearerToken);
    QJsonObject importStatus() const;
    void checkWrite(const QJsonObject& arguments, bool requireRevision = true);
    void checkEnabledAndUnlocked(bool requireStopped = true);
};
}
