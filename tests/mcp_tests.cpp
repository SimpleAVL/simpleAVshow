#include "app/MainWindow.h"
#include <QtTest>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonArray>
#include <QCheckBox>
#include <QPushButton>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QFile>
#include <QTcpSocket>

using namespace sav;
namespace {
struct Response { int status = 0; QJsonObject body; };
Response http(McpServer& server, QByteArray body, QByteArray method = "POST", QMap<QByteArray, QByteArray> headers = {}) {
    QNetworkAccessManager manager;
    QNetworkRequest request{QUrl(server.endpoint())};
    request.setRawHeader("Authorization", "Bearer " + server.token().toUtf8());
    request.setRawHeader("Content-Type", "application/json");
    request.setRawHeader("Accept", "application/json, text/event-stream");
    request.setRawHeader("MCP-Protocol-Version", "2025-11-25");
    for (auto it = headers.begin(); it != headers.end(); ++it) request.setRawHeader(it.key(), it.value());
    auto* reply = manager.sendCustomRequest(request, method, body);
    QEventLoop loop; QTimer timer; timer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit); timer.start(5000); loop.exec();
    return {reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), QJsonDocument::fromJson(reply->readAll()).object()};
}
Response rpc(McpServer& server, QString method, QJsonObject params = {}) {
    return http(server, QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", "test"}, {"method", method}, {"params", params}}).toJson(QJsonDocument::Compact));
}
QJsonObject call(McpServer& server, const QString& name, QJsonObject arguments = {}) {
    return rpc(server, "tools/call", {{"name", name}, {"arguments", arguments}}).body["result"].toObject();
}
double revision(McpServer& server) { return call(server, "get_show")["structuredContent"].toObject()["revision"].toDouble(); }
QJsonObject operation(QString type, QString id, QJsonObject fields) { return {{"type", type}, {"id", id}, {"fields", fields}}; }
QJsonObject edits(McpServer& server, QJsonArray operations, double expected = -1) {
    return call(server, "apply_edits", {{"expected_revision", expected < 0 ? revision(server) : expected}, {"operations", operations}});
}
}
class McpTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("SimpleAVShowTests");
        QCoreApplication::setApplicationName("McpTests");
        QStandardPaths::setTestModeEnabled(true);
    }
    void protocolAndLocalAccess() {
        Controller c; McpServer server(c);
        QVERIFY(!server.running()); QVERIFY(server.start(0)); QVERIFY(!server.editingEnabled());
        const auto init = rpc(server, "initialize", {{"protocolVersion", "2025-11-25"}, {"clientInfo", QJsonObject{{"name", "test"}, {"version", "1"}}}, {"capabilities", QJsonObject{}}});
        QCOMPARE(init.status, 200); QCOMPARE(init.body["id"].toString(), "test");
        QCOMPARE(init.body["result"].toObject()["protocolVersion"].toString(), "2025-11-25");
        QCOMPARE(rpc(server, "tools/list").body["result"].toObject()["tools"].toArray().size(), 5);
        QCOMPARE(http(server, "{}", "POST", {{"Authorization", "Bearer wrong"}}).status, 401);
        QCOMPARE(http(server, "{}", "POST", {{"Origin", "https://example.com"}}).status, 403);
        QCOMPARE(http(server, "{}", "POST", {{"Host", "example.com"}}).status, 403);
        QCOMPARE(http(server, "{}", "POST", {{"MCP-Protocol-Version", "invalid"}}).status, 400);
        QCOMPARE(http(server, "{}", "GET").status, 405);
        QCOMPARE(http(server, "bad json").body["error"].toObject()["code"].toInt(), -32700);
        QCOMPARE(http(server, "[]").body["error"].toObject()["code"].toInt(), -32600);
        QCOMPARE(rpc(server, "not/a/method").body["error"].toObject()["code"].toInt(), -32601);
        QCOMPARE(rpc(server, "tools/call", {{"name", "not-a-tool"}}).body["error"].toObject()["code"].toInt(), -32602);
        QCOMPARE(http(server, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}").status, 202);
        QCOMPARE(rpc(server, "resources/read", {{"uri", "simpleavshow://show"}}).body["result"].toObject()["contents"].toArray().size(), 1);
        const auto token = server.token(); server.stop(); QVERIFY(server.token().isEmpty());
        QVERIFY(server.start(0)); QVERIFY(server.token() != token); QVERIFY(!server.editingEnabled());
    }
    void atomicSharedEditsAndConflicts() {
        Controller c; McpServer server(c); QVERIFY(server.start(0));
        const QJsonArray batch{operation("clip", "opening", {{"text", "Welcome"}, {"track", 0}, {"duration", frames(5)}}),
            operation("marker", "wait", {{"name", "Wait for actor"}, {"time", frames(5)}, {"message", QJsonArray{144, 60, 100}}})};
        QVERIFY(edits(server, batch)["isError"].toBool()); QVERIFY(c.project.show.clips.empty());
        server.setEditingEnabled(true);
        const auto oldRevision = revision(server); QVERIFY(!edits(server, batch)["isError"].toBool());
        QCOMPARE(c.project.show.clips.size(), size_t(1)); QCOMPARE(c.project.show.markers.size(), size_t(1)); QVERIFY(c.dirty);
        QVERIFY(revision(server) > oldRevision);
        c.undo(); QVERIFY(c.project.show.clips.empty()); QVERIFY(c.project.show.markers.empty());
        c.redo(); QCOMPARE(c.project.show.clips.front().text, std::string("Welcome"));
        const auto r = revision(server);
        QVERIFY(c.edit([](Project& p) { p.title = "Operator title"; }));
        QVERIFY(edits(server, {operation("marker", "stale", {{"time", 0}})}, r)["isError"].toBool());
        QCOMPARE(c.project.title, QString("Operator title")); QCOMPARE(c.project.show.markers.size(), size_t(1));
        const auto before = projectJson(c.project);
        QVERIFY(edits(server, {operation("marker", "would-add", {{"time", 0}}), operation("clip", "bad", {{"text", "overlap"}, {"track", 0}, {"duration", frames(3)}})})["isError"].toBool());
        QCOMPARE(projectJson(c.project), before);
        QVERIFY(edits(server, {operation("marker", "bad-midi", {{"message", QJsonArray{144, 60}}})})["isError"].toBool());
        QVERIFY(edits(server, {operation("marker", "wrong-type", {{"hold", "true"}})})["isError"].toBool());
        QVERIFY(edits(server, {operation("marker", "typo", {{"tim", 0}})})["isError"].toBool());
        QVERIFY(!edits(server, {operation("track", "ambience", {{"name", "Ambience 2"}, {"async", true}})})["isError"].toBool());
        QCOMPARE(c.project.show.tracks.back().left, -1); QCOMPARE(c.project.show.tracks.back().right, -1);
        c.setShowMode(true); QVERIFY(edits(server, batch)["isError"].toBool()); c.setShowMode(false);
        c.busy = true; QVERIFY(edits(server, batch)["isError"].toBool()); c.busy = false;
        c.transport.command({Action::Play}); c.transport.render(nullptr, 0, 0);
        QVERIFY(edits(server, batch)["isError"].toBool()); c.stopAll();
    }
    void heldAndAsyncPlaybackLocks() {
        Controller c; McpServer server(c); QVERIFY(server.start(0)); server.setEditingEnabled(true);
        c.replace(demoProject());
        float samples[2]{};
        c.transport.command({Action::Play}); c.transport.render(samples, 1, 2);
        QVERIFY(c.transport.asyncVoices() > 0);
        c.transport.command({Action::Pause}); c.transport.render(samples, 1, 2);
        QCOMPARE(c.transport.state(), State::Paused);
        QVERIFY(edits(server, {operation("marker", "blocked", {{"time", 0}})})["isError"].toBool());
        c.stopAll(); c.transport.render(samples, 1, 2);
        c.seek(frames(12)); c.transport.render(samples, 1, 2);
        c.transport.command({Action::Play}); c.transport.render(samples, 1, 2);
        QCOMPARE(c.transport.state(), State::Held);
        QVERIFY(edits(server, {operation("marker", "blocked", {{"time", 0}})})["isError"].toBool());
        c.stopAll(); c.transport.render(samples, 1, 2);
        QVERIFY(!edits(server, {operation("marker", "unlocked", {{"time", 0}})})["isError"].toBool());
    }
    void linkedTimingAndAutomation() {
        Controller c; McpServer server(c); QVERIFY(server.start(0)); server.setEditingEnabled(true);
        QVERIFY(!edits(server, {operation("clip", "picture", {{"track", 0}, {"text", "Picture"}, {"link", "av"}, {"duration", frames(10)}, {"alpha", QJsonArray{QJsonArray{0, 0}, QJsonArray{frames(4), 1}}}}),
            operation("clip", "partner", {{"track", 1}, {"text", "Partner"}, {"link", "av"}, {"duration", frames(10)}})})["isError"].toBool());
        QVERIFY(!edits(server, {operation("clip", "picture", {{"start", frames(2)}, {"duration", frames(8)}, {"offset", frames(2)}})})["isError"].toBool());
        for (const auto& clip : c.project.show.clips) { QCOMPARE(clip.start, frames(2)); QCOMPARE(clip.duration, frames(8)); QCOMPARE(clip.offset, frames(2)); }
        QCOMPARE(c.project.show.clips.front().alpha.points.front().value, .5);
        QCOMPARE(c.project.show.clips.front().alpha.points.back().time, frames(2));
        QVERIFY(!edits(server, {QJsonObject{{"type", "delete_clip"}, {"id", "picture"}}})["isError"].toBool());
        QVERIFY(c.project.show.clips.empty()); c.undo(); QCOMPARE(c.project.show.clips.size(), size_t(2));
        c.replace(demoProject());
        const auto original = c.project.assets.first().audio;
        QVERIFY(!edits(server, {operation("marker", "extra", {{"time", 0}})})["isError"].toBool());
        QCOMPARE(c.project.assets.size(), 3); QCOMPARE(c.project.assets.first().audio, original);
    }
    void asynchronousImportAndCommit() {
        Controller c; McpServer server(c); QVERIFY(server.start(0)); server.setEditingEnabled(true);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        QImage image(32, 32, QImage::Format_RGB32); image.fill(Qt::red); const auto path = dir.filePath("backdrop.png"); QVERIFY(image.save(path));
        const auto started = call(server, "import_media", {{"expected_revision", revision(server)}, {"paths", QJsonArray{path}}});
        QVERIFY(!started["isError"].toBool());
        const auto job = started["structuredContent"].toObject()["job_id"].toString(); QVERIFY(!job.isEmpty());
        QVERIFY(c.edit([](Project& p) { p.title = "Operator keeps editing"; }));
        QTRY_COMPARE(call(server, "get_import", {{"job_id", job}})["structuredContent"].toObject()["state"].toString(), QString("ready"));
        QVERIFY(c.project.assets.isEmpty()); const auto r = revision(server);
        QVERIFY(c.edit([](Project& p) { p.title = "More operator work"; }));
        QVERIFY(call(server, "commit_import", {{"job_id", job}, {"expected_revision", r}})["isError"].toBool());
        QVERIFY(!call(server, "commit_import", {{"job_id", job}, {"expected_revision", revision(server)}})["isError"].toBool());
        QCOMPARE(c.project.assets.size(), 1); QCOMPARE(c.project.title, QString("More operator work"));
        const auto id = c.project.assets.first().id;
        QVERIFY(!edits(server, {operation("clip", "backdrop", {{"track", 0}, {"asset", id}, {"duration", frames(10)}})})["isError"].toBool());
        QCOMPARE(c.project.show.clips.size(), size_t(1)); c.undo(); c.undo(); QVERIFY(c.project.assets.isEmpty());
        QFile file(dir.filePath("broken.wav")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("bad"); file.close();
        const auto failed = call(server, "import_media", {{"expected_revision", revision(server)}, {"paths", QJsonArray{path, file.fileName()}}});
        const auto badJob = failed["structuredContent"].toObject()["job_id"].toString();
        QTRY_COMPARE(call(server, "get_import", {{"job_id", badJob}})["structuredContent"].toObject()["state"].toString(), QString("failed"));
        QVERIFY(c.project.assets.isEmpty());
        QVERIFY(call(server, "commit_import", {{"job_id", badJob}, {"expected_revision", revision(server)}})["isError"].toBool());
        server.stop();
    }
    void operatorControlsAndPendingInspector() {
        MainWindow window; window.show();
        auto* button = window.findChild<QPushButton*>("agentCollaborationButton"); QVERIFY(button);
        bool visited = false;
        QTimer::singleShot(0, &window, [&] {
            auto* dialog = window.findChild<QDialog*>(); if (!dialog) return;
            auto* enabled = dialog->findChild<QCheckBox*>("agentServerEnabled");
            auto* editing = dialog->findChild<QCheckBox*>("agentEditingEnabled");
            if (!enabled || !editing) { dialog->reject(); return; }
            QTcpServer freePort;
            if (!freePort.listen(QHostAddress::LocalHost, 0)) { dialog->reject(); return; }
            const auto localPort = freePort.serverPort(); freePort.close();
            dialog->findChild<QSpinBox*>("agentServerPort")->setValue(localPort);
            enabled->setChecked(true);
            if (!window.mcpServer().running()) { dialog->reject(); return; }
            editing->setChecked(true); visited = window.mcpServer().editingEnabled();
            dialog->accept();
        });
        button->click(); QVERIFY(visited); QVERIFY(window.mcpServer().running());
        auto& server = window.mcpServer();
        const auto r = revision(server);
        // Type without leaving the field: the network request must flush the real inspector.
        QLineEdit* name = nullptr;
        for (auto* field : window.findChildren<QLineEdit*>())
            if (field->text() == "Background" && field->isVisible()) name = field;
        QVERIFY(name); name->setFocus(); name->selectAll(); QTest::keyClicks(name, "Operator background");
        QCOMPARE(window.controller().project.show.tracks.front().name, std::string("Background"));
        QVERIFY(edits(server, {operation("marker", "conflicting", {{"time", 0}})}, r)["isError"].toBool());
        QCOMPARE(window.controller().project.show.tracks.front().name, std::string("Operator background"));
        QVERIFY(window.controller().project.show.markers.empty());
        QVERIFY(!edits(server, {operation("marker", "rebased", {{"time", 0}})})["isError"].toBool());
        QCOMPARE(window.controller().project.show.tracks.front().name, std::string("Operator background"));
        QCOMPARE(window.controller().project.show.markers.size(), size_t(1));
        window.controller().dirty = false; server.stop();
    }
};
QTEST_MAIN(McpTests)
#include "mcp_tests.moc"
