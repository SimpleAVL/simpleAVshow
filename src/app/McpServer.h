#pragma once
#include "app/Controller.h"
#include <QTcpServer>
#include <QJsonArray>

namespace sav {
// A local, opt-in Streamable HTTP MCP endpoint. All project access stays on the GUI thread.
class McpServer : public QObject {
    Q_OBJECT
public:
    explicit McpServer(Controller& controller, QObject* parent = nullptr);
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
private:
    Controller& c_;
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
    QJsonObject importStatus() const;
    void checkWrite(const QJsonObject& arguments);
};
}
