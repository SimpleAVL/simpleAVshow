#pragma once
#include "app/Controller.h"
#include <QTcpServer>
#include <QElapsedTimer>
#include <QHash>

namespace sav {
// An opt-in web remote for phones and tablets on the local network. The page and its
// JSON API live below a secret path; all controller access stays on the GUI thread.
class RemoteServer : public QObject {
    Q_OBJECT
public:
    explicit RemoteServer(Controller& controller, QObject* parent = nullptr);
    bool start(quint16 port = 48124);
    void stop();
    bool running() const { return server_.isListening(); }
    quint16 port() const { return server_.serverPort(); }
    QString errorString() const { return server_.errorString(); }
    // The secret path segment. It is remembered across launches so phones can bookmark the page.
    QString key() const { return key_; }
    // Replaces the key, invalidating bookmarks and closing open connections.
    void changeKey();
    // One link per local network address; the phone must be on the same network.
    QStringList links() const;
    // Devices that read the show state within the last few seconds.
    int clients() const;
    QJsonObject state() const;
    // go plays or continues a hold, pause pauses, stop is Stop All. go and pause do nothing in the
    // other state, so a delayed or repeated tap cannot toggle the show back.
    bool command(const QString& action, const QString& from = {});
signals:
    void stateChanged();
private:
    Controller& c_;
    QTcpServer server_;
    QString key_;
    QHash<QString, QElapsedTimer> seen_;
    void acceptConnection();
    void closeConnections();
};
}
