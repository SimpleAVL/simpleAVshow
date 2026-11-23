#pragma once
#include "app/Controller.h"
#include <QTcpServer>
#include <QJsonArray>
#include <functional>

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
    bool editing_ = false;
    quint64 revision_ = 0;
    QString importId_, importState_, importError_;
    std::vector<MediaAsset> imported_;
    void acceptConnection();
    QJsonObject dispatch(const QJsonObject& request);
    QJsonObject callTool(const QString& name, const QJsonObject& arguments);
    QJsonObject snapshot() const;
    QJsonObject environmentReport() const;
    QJsonObject preflightReport() const;
    QJsonObject renderPreview(const QJsonObject& arguments) const;
    QJsonObject appGuide(const QJsonObject& arguments) const;
    QJsonObject encodeMidiTool(const QJsonObject& arguments) const;
    QJsonObject listMediaFiles(const QJsonObject& arguments) const;
    QJsonObject relinkMediaTool(const QJsonObject& arguments);
    QJsonObject configureOutputsTool(const QJsonObject& arguments);
    QJsonObject configureRemoteTool(const QJsonObject& arguments);
    QJsonObject projectActionTool(const QJsonObject& arguments);
    QJsonObject transportControlTool(const QJsonObject& arguments);
    QJsonObject historyTool(const QJsonObject& arguments);
    QJsonObject seekAndSelectTool(const QJsonObject& arguments);
    QJsonObject importStatus() const;
    void checkWrite(const QJsonObject& arguments, bool requireRevision = true);
    void checkEnabledAndUnlocked(bool requireStopped = true);
};
}
