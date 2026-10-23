#include "app/MainWindow.h"
#include <QtTest>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QPushButton>
#include <QStandardPaths>

using namespace sav;
namespace {
struct Response { int status = 0; QByteArray type, body; QJsonObject json() const { return QJsonDocument::fromJson(body).object(); } };
Response http(RemoteServer& server, QString path, QByteArray method = "GET", QByteArray body = {}, QMap<QByteArray, QByteArray> headers = {}) {
    QNetworkAccessManager manager;
    QNetworkRequest request{QUrl(QString("http://127.0.0.1:%1%2").arg(server.port()).arg(path))};
    if (method == "POST") request.setRawHeader("Content-Type", "application/json");
    for (auto it = headers.begin(); it != headers.end(); ++it) request.setRawHeader(it.key(), it.value());
    auto* reply = manager.sendCustomRequest(request, method, body);
    QEventLoop loop; QTimer timer; timer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit); timer.start(5000); loop.exec();
    return {reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), reply->header(QNetworkRequest::ContentTypeHeader).toByteArray(), reply->readAll()};
}
Response send(RemoteServer& server, QByteArray action, QMap<QByteArray, QByteArray> headers = {}) {
    return http(server, "/" + server.key() + "/command", "POST", "{\"action\":\"" + action + "\"}", headers);
}
void settle(Controller& c) { float samples[2]{}; c.transport.render(samples, 1, 2); }
}
class RemoteTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("SimpleAVShowTests");
        QCoreApplication::setApplicationName("RemoteTests");
        QStandardPaths::setTestModeEnabled(true);
    }
    void pageStateAndAccess() {
        Controller c; RemoteServer server(c);
        QVERIFY(!server.running()); QVERIFY(server.links().isEmpty());
        QVERIFY(QRegularExpression("^[a-z2-9]{4}-[a-z2-9]{4}-[a-z2-9]{4}$").match(server.key()).hasMatch());
        QVERIFY(server.start(0));
        const QString base = "/" + server.key();
        const auto page = http(server, base + "/");
        QCOMPARE(page.status, 200); QVERIFY(page.type.startsWith("text/html")); QVERIFY(page.body.contains("SimpleAVShow remote"));
        QCOMPARE(http(server, base).status, 200);
        const auto state = http(server, base + "/state");
        QCOMPARE(state.status, 200); QCOMPARE(state.json()["state"].toString(), "stopped");
        QCOMPARE(state.json()["clock"].toString(), "00:00:00:00"); QCOMPARE(state.json()["title"].toString(), c.project.title);
        QCOMPARE(server.clients(), 1);
        // Unknown keys, paths and methods.
        QCOMPARE(http(server, "/").status, 404);
        QCOMPARE(http(server, "/abcd-efgh-jkmn/state").status, 404);
        QCOMPARE(http(server, base + "x/state").status, 404);
        QCOMPARE(http(server, base + "/other").status, 404);
        QCOMPARE(http(server, base + "/command").status, 405);
        QCOMPARE(http(server, base + "/state", "POST", "{}").status, 405);
        // Commands must be same-origin JSON naming a known action.
        QCOMPARE(send(server, "stop", {{"Origin", "http://example.com"}}).status, 403);
        QCOMPARE(send(server, "stop", {{"Content-Type", "text/plain"}}).status, 415);
        QCOMPARE(send(server, "rewind").status, 400);
        QCOMPARE(http(server, base + "/command", "POST", QByteArray(2048, ' ')).status, 413);
        const auto stop = send(server, "stop", {{"Origin", QString("http://127.0.0.1:%1").arg(server.port()).toUtf8()}});
        QCOMPARE(stop.status, 200); QVERIFY(stop.json()["accepted"].toBool()); QVERIFY(c.status.contains("Stop All"));
        // The key survives restarts until the operator changes it.
        const auto key = server.key(); server.stop(); QVERIFY(!server.running()); QCOMPARE(server.clients(), 0);
        RemoteServer again(c); QCOMPARE(again.key(), key);
        QVERIFY(server.start(0)); QCOMPARE(server.key(), key);
        server.changeKey(); QVERIFY(server.key() != key);
        QCOMPARE(http(server, "/" + key + "/state").status, 404);
        QCOMPARE(http(server, "/" + server.key() + "/state").status, 200);
    }
    void commandsFollowIntent() {
        Controller c; RemoteServer server(c); QVERIFY(server.start(0));
        c.replace(demoProject());
        c.transport.command({Action::Play}); settle(c);
        QCOMPARE(c.transport.state(), State::Playing);
        // A repeated Play tap cannot pause a running show.
        QCOMPARE(send(server, "go").json()["accepted"].toBool(), false); settle(c);
        QCOMPARE(c.transport.state(), State::Playing);
        QVERIFY(send(server, "pause").json()["accepted"].toBool()); settle(c);
        QCOMPARE(c.transport.state(), State::Paused);
        QCOMPARE(send(server, "pause").json()["accepted"].toBool(), false);
        // A Pause sent before a hold arrived must not continue past it.
        c.stopAll(); settle(c); c.seek(frames(12)); settle(c);
        c.transport.command({Action::Play}); settle(c);
        QCOMPARE(c.transport.state(), State::Held);
        const auto state = server.state();
        QCOMPARE(state["state"].toString(), "held"); QVERIFY(!state["hold"].toString().isEmpty());
        QVERIFY(state["next"].toObject()["name"].toString() != state["hold"].toString());
        QCOMPARE(send(server, "pause").json()["accepted"].toBool(), false); settle(c);
        QCOMPARE(c.transport.state(), State::Held);
        // Stop All works in Show mode, like the button.
        c.setShowMode(true);
        QVERIFY(send(server, "stop").json()["state"].toObject()["show_mode"].toBool()); settle(c);
        QVERIFY(c.transport.state() != State::Held && c.transport.state() != State::Playing);
        c.busy = true; QCOMPARE(server.command("stop"), false); c.busy = false;
    }
    void topBarButton() {
        MainWindow window; window.show();
        auto* button = window.findChild<QPushButton*>("remoteControlButton"); QVERIFY(button);
        QCOMPARE(button->text(), "Remote · off");
        QVERIFY(window.remoteServer().start(0));
        QTRY_COMPARE(button->text(), "Remote · on");
        QCOMPARE(http(window.remoteServer(), "/" + window.remoteServer().key() + "/state").status, 200);
        QTRY_COMPARE(button->text(), "Remote · 1 connected");
        window.remoteServer().stop();
        QTRY_COMPARE(button->text(), "Remote · off");
    }
};
QTEST_MAIN(RemoteTests)
#include "remote_tests.moc"
