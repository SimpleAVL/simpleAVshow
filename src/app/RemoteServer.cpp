#include "app/RemoteServer.h"
#include <QFile>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTcpSocket>

namespace sav {
namespace {
constexpr qsizetype maxHeader = 8192, maxBody = 1024;
constexpr qint64 clientWindow = 3000;
// Unambiguous characters, in three groups of four, so the link can be typed on a phone (about 59 bits).
QString newKey() {
    const QString alphabet = "abcdefghjkmnpqrstuvwxyz23456789"; QString key;
    for (int i = 0; i < 12; ++i) {
        if (i && i % 4 == 0) key += '-';
        key += alphabet[QRandomGenerator::system()->bounded(static_cast<int>(alphabet.size()))];
    }
    return key;
}
bool validKey(const QString& key) {
    static const QRegularExpression format("^[a-hjkmnp-z2-9]{4}-[a-hjkmnp-z2-9]{4}-[a-hjkmnp-z2-9]{4}$");
    return format.match(key).hasMatch();
}
const QByteArray& page() {
    static const QByteArray html = [] { QFile file(":/remote/remote.html"); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray(); }();
    return html;
}
}

RemoteServer::RemoteServer(Controller& controller, QObject* parent) : QObject(parent), c_(controller) {
    key_ = QSettings().value("remote/key").toString();
    if (!validKey(key_)) { key_ = newKey(); QSettings().setValue("remote/key", key_); }
    connect(&server_, &QTcpServer::newConnection, this, &RemoteServer::acceptConnection);
}
bool RemoteServer::start(quint16 port) {
    if (running()) return true;
    // Phones reach the computer through its network address, so this listens beyond loopback.
    if (!server_.listen(QHostAddress::AnyIPv4, port)) return false;
    QSettings().setValue("remote/port", server_.serverPort());
    c_.report("Phone remote enabled. Anyone with its link on this network can control playback.");
    emit stateChanged(); return true;
}
void RemoteServer::stop() {
    server_.close(); closeConnections(); seen_.clear();
    emit stateChanged();
}
void RemoteServer::changeKey() {
    key_ = newKey(); QSettings().setValue("remote/key", key_);
    closeConnections(); seen_.clear();
    emit stateChanged();
}
void RemoteServer::closeConnections() {
    for (auto* socket : findChildren<QTcpSocket*>()) socket->abort();
}
QStringList RemoteServer::links() const {
    QStringList result;
    if (!running()) return result;
    for (const auto& interface : QNetworkInterface::allInterfaces()) {
        const auto flags = interface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning) || flags.testFlag(QNetworkInterface::IsLoopBack)) continue;
        for (const auto& entry : interface.addressEntries()) {
            const auto ip = entry.ip();
            if (ip.protocol() != QAbstractSocket::IPv4Protocol || ip.isLinkLocal()) continue;
            result << QString("http://%1:%2/%3/").arg(ip.toString()).arg(port()).arg(key_);
        }
    }
    result.removeDuplicates();
    return result;
}
int RemoteServer::clients() const {
    return static_cast<int>(std::count_if(seen_.cbegin(), seen_.cend(), [](const QElapsedTimer& t) { return !t.hasExpired(clientWindow); }));
}
QJsonObject RemoteServer::state() const {
    const State state = c_.transport.state(); const Frame now = c_.position();
    QJsonObject result{{"title", c_.project.title}, {"state", stateName(state)}, {"clock", timecode(now, c_.project.fps)},
        {"left", countdown(c_.project.show.duration - now)}, {"show_mode", c_.showMode}, {"busy", c_.busy},
        {"async", c_.transport.asyncVoices()}, {"problem", c_.playbackProblem}, {"status", c_.status}};
    if (const auto* next = c_.nextCue())
        result.insert("next", QJsonObject{{"name", QString::fromStdString(next->name)}, {"hold", next->hold}, {"in", countdown(next->time - now)}});
    const int held = c_.transport.heldMarker();
    if (state == State::Held && held >= 0 && static_cast<std::size_t>(held) < c_.project.show.markers.size())
        result.insert("hold", QString::fromStdString(c_.project.show.markers[held].name));
    return result;
}
bool RemoteServer::command(const QString& action, const QString& from) {
    const State state = c_.transport.state();
    const QString by = from.isEmpty() ? QString("Phone remote") : "Phone remote " + from;
    if (c_.busy) return false;
    if (action == "go" && state != State::Playing) {
        // Report first so a failed start's own explanation replaces it in the status bar.
        c_.report(by + (state == State::Held ? ": Continue" : ": Play")); c_.toggle(); return true;
    }
    if (action == "pause" && state == State::Playing) { c_.report(by + ": Pause"); c_.toggle(); return true; }
    if (action == "stop") { c_.report(by + ": Stop All"); c_.stopAll(); return true; }
    return false;
}

void RemoteServer::acceptConnection() {
    while (server_.hasPendingConnections()) {
        auto* socket = server_.nextPendingConnection(); socket->setParent(this);
        if (findChildren<QTcpSocket*>().size() > 32) { socket->abort(); socket->deleteLater(); continue; }
        socket->setReadBufferSize(maxHeader + maxBody);
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        auto* timeout = new QTimer(socket); timeout->setSingleShot(true); timeout->start(10000);
        connect(timeout, &QTimer::timeout, socket, &QTcpSocket::abort);
        auto buffer = std::make_shared<QByteArray>(); auto done = std::make_shared<bool>(false);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer, done] {
            if (*done) return;
            auto respond = [&](int status, const QByteArray& reason, const QByteArray& type = {}, const QByteArray& body = {}, const QByteArray& extra = {}) {
                *done = true;
                // The key is in the URL: never leak it through referrers, caches or embedding.
                socket->write("HTTP/1.1 " + QByteArray::number(status) + " " + reason + "\r\n" + (type.isEmpty() ? QByteArray() : "Content-Type: " + type + "\r\n") +
                    "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\n"
                    "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\n" + extra + "\r\n" + body);
                socket->disconnectFromHost();
            };
            auto json = [&](const QJsonObject& value) { respond(200, "OK", "application/json", QJsonDocument(value).toJson(QJsonDocument::Compact)); };
            buffer->append(socket->readAll());
            const auto boundary = buffer->indexOf("\r\n\r\n");
            if (boundary < 0) { if (buffer->size() > maxHeader) respond(431, "Request Header Fields Too Large"); return; }
            if (boundary > maxHeader) { respond(431, "Request Header Fields Too Large"); return; }
            const auto lines = buffer->left(boundary).split('\n'); const auto requestLine = lines.front().trimmed().split(' ');
            if (requestLine.size() != 3 || requestLine[2] != "HTTP/1.1") { respond(400, "Bad Request"); return; }
            QMap<QByteArray, QByteArray> headers;
            for (int i = 1; i < lines.size(); ++i) {
                const auto colon = lines[i].indexOf(':');
                if (colon <= 0) { respond(400, "Bad Request"); return; }
                const auto name = lines[i].left(colon).toLower();
                if (headers.contains(name)) { respond(400, "Bad Request"); return; }
                headers.insert(name, lines[i].mid(colon + 1).trimmed());
            }
            // Unknown and outdated keys look like missing pages.
            const auto target = requestLine[1].split('?').front(); const QByteArray base = "/" + key_.toUtf8();
            if (target != base && !target.startsWith(base + "/")) { respond(404, "Not Found"); return; }
            const auto route = target.mid(base.size()); const auto& method = requestLine[0];
            if (route == "" || route == "/") {
                if (method != "GET") { respond(405, "Method Not Allowed", {}, {}, "Allow: GET\r\n"); return; }
                respond(200, "OK", "text/html; charset=utf-8", page(), "Content-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
                    "connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'\r\n");
                return;
            }
            if (route == "/state") {
                if (method != "GET") { respond(405, "Method Not Allowed", {}, {}, "Allow: GET\r\n"); return; }
                auto& seen = seen_[socket->peerAddress().toString()]; seen.start();
                for (auto it = seen_.begin(); it != seen_.end();) it = it->hasExpired(clientWindow) ? seen_.erase(it) : std::next(it);
                json(state()); return;
            }
            if (route != "/command") { respond(404, "Not Found"); return; }
            if (method != "POST") { respond(405, "Method Not Allowed", {}, {}, "Allow: POST\r\n"); return; }
            // Requiring JSON makes browsers preflight cross-site requests, which this server never approves.
            if (headers.contains("origin") && headers["origin"] != "http://" + headers["host"]) { respond(403, "Forbidden"); return; }
            if (headers["content-type"].split(';').front().trimmed().toLower() != "application/json") { respond(415, "Unsupported Media Type"); return; }
            if (headers.contains("transfer-encoding")) { respond(400, "Bad Request"); return; }
            bool ok = false; const auto length = headers["content-length"].toLongLong(&ok);
            if (!ok || length < 0) { respond(400, "Bad Request"); return; }
            if (length > maxBody) { respond(413, "Content Too Large"); return; }
            const auto received = buffer->size() - boundary - 4;
            if (received < length) return;
            if (received != length) { respond(400, "Bad Request"); return; }
            const auto action = QJsonDocument::fromJson(buffer->mid(boundary + 4)).object()["action"].toString();
            if (action != "go" && action != "pause" && action != "stop") { respond(400, "Bad Request"); return; }
            const bool accepted = command(action, socket->peerAddress().toString());
            json({{"accepted", accepted}, {"state", state()}});
        });
    }
}
}
