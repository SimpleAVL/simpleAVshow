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
struct Response { int status = 0; QJsonObject body; QByteArray sessionId; };
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
    return {reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), QJsonDocument::fromJson(reply->readAll()).object(),
        reply->rawHeader("Mcp-Session-Id")};
}
Response rpc(McpServer& server, QString method, QJsonObject params = {}) {
    return http(server, QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", "test"}, {"method", method}, {"params", params}}).toJson(QJsonDocument::Compact));
}
QJsonObject call(McpServer& server, const QString& name, QJsonObject arguments = {}) {
    return rpc(server, "tools/call", {{"name", name}, {"arguments", arguments}}).body["result"].toObject();
}
double revision(McpServer& server) { return call(server, "get_show")["structuredContent"].toObject()["revision"].toDouble(); }
// Qt 6.4 on Linux has no unambiguous std::int64_t JSON constructor.
QJsonValue frameJson(double time) { return QJsonValue(static_cast<double>(frames(time))); }
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
        QCOMPARE(rpc(server, "tools/list").body["result"].toObject()["tools"].toArray().size(), 18);
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
        const QJsonArray batch{operation("clip", "opening", {{"text", "Welcome"}, {"track", 0}, {"duration", frameJson(5)}}),
            operation("marker", "wait", {{"name", "Wait for actor"}, {"time", frameJson(5)}, {"message", QJsonArray{144, 60, 100}}})};
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
        QVERIFY(edits(server, {operation("marker", "would-add", {{"time", 0}}), operation("clip", "bad", {{"text", "overlap"}, {"track", 0}, {"duration", frameJson(3)}})})["isError"].toBool());
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
        QVERIFY(!edits(server, {operation("clip", "picture", {{"track", 0}, {"text", "Picture"}, {"link", "av"}, {"duration", frameJson(10)}, {"alpha", QJsonArray{QJsonArray{0, 0}, QJsonArray{frameJson(4), 1}}}}),
            operation("clip", "partner", {{"track", 1}, {"text", "Partner"}, {"link", "av"}, {"duration", frameJson(10)}})})["isError"].toBool());
        QVERIFY(!edits(server, {operation("clip", "picture", {{"start", frameJson(2)}, {"duration", frameJson(8)}, {"offset", frameJson(2)}})})["isError"].toBool());
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
        QVERIFY(!edits(server, {operation("clip", "backdrop", {{"track", 0}, {"asset", id}, {"duration", frameJson(10)}})})["isError"].toBool());
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
    void previewPreflightAndExtendedEditOperations() {
        Controller c;
        c.replace(demoProject());
        McpServer server(c);
        QVERIFY(server.start(0));

        // 1. render_preview is paused in Show mode to protect live output, and works read-only outside Show mode.
        const Frame beforePos = c.position();
        c.setShowMode(true);
        QVERIFY(call(server, "render_preview", {{"time", frameJson(6)}, {"width", 320}})["isError"].toBool());
        c.setShowMode(false);
        const auto preview = call(server, "render_preview", {{"time", frameJson(6)}, {"width", 320}});
        QVERIFY(!preview["isError"].toBool());
        QCOMPARE(c.position(), beforePos);
        const auto content = preview["content"].toArray();
        QCOMPARE(content.size(), 2);
        QCOMPARE(content[0].toObject()["type"].toString(), QString("image"));
        QCOMPARE(content[0].toObject()["mimeType"].toString(), QString("image/png"));
        const QByteArray pngBytes = QByteArray::fromBase64(content[0].toObject()["data"].toString().toLatin1());
        QImage rendered;
        QVERIFY(rendered.loadFromData(pngBytes, "PNG"));
        QCOMPARE(rendered.width(), 320);
        QVERIFY(rendered.pixelColor(160, 90) != QColor(Qt::black));
        const auto meta = preview["structuredContent"].toObject();
        QCOMPARE(meta["width"].toInt(), 320);
        QVERIFY(meta["active_clips"].toArray().size() >= 2);
        QVERIFY(call(server, "render_preview", {{"width", 50}})["isError"].toBool());
        QVERIFY(call(server, "render_preview", {{"time", frameJson(999)}})["isError"].toBool());

        // 2. get_preflight reports disconnected tracks with clips, unused assets, next cue, and summary.
        const auto preflight = call(server, "get_preflight");
        QVERIFY(!preflight["isError"].toBool());
        const auto pf = preflight["structuredContent"].toObject();
        QCOMPARE(pf["summary"].toObject()["title"].toString(), QString("The opening scene"));
        // Demo Click track (index 5) has a clip and starts disconnected (-1/-1).
        QVERIFY(pf["disconnected_audio_tracks"].toArray().size() >= 1);
        QCOMPARE(pf["next_cue"].toObject()["name"].toString(), QString("Wait for the actor"));

        // 3. split_clip preserves automation and splits linked partners atomically.
        server.setEditingEnabled(true);
        c.replace(newProject());
        QVERIFY(!edits(server, {
            operation("clip", "p1", {{"track", 0}, {"text", "Title"}, {"link", "av"}, {"start", frameJson(2)}, {"duration", frameJson(10)},
                {"alpha", QJsonArray{QJsonArray{0, 0}, QJsonArray{frameJson(10), 1}}}}),
            operation("clip", "p2", {{"track", 1}, {"text", "Sub"}, {"link", "av"}, {"start", frameJson(2)}, {"duration", frameJson(10)}})
        })["isError"].toBool());
        QVERIFY(!edits(server, {
            operation("split_clip", "p1", {{"time", frameJson(7)}, {"new_id", "p1-right"}})
        })["isError"].toBool());
        QCOMPARE(c.project.show.clips.size(), size_t(4));
        const auto* right = [&]() -> const Clip* {
            for (const auto& clip : c.project.show.clips) if (clip.id == "p1-right") return &clip;
            return nullptr;
        }();
        QVERIFY(right);
        QCOMPARE(right->start, frames(7));
        QCOMPARE(right->duration, frames(5));
        QCOMPARE(right->alpha.points.front().value, .5);
        c.undo();
        QCOMPARE(c.project.show.clips.size(), size_t(2));
        // Splitting strictly inside a shaped S-curve segment must be rejected rather than distorting the curve.
        QVERIFY(!edits(server, {
            operation("clip", "p1", {{"alpha", QJsonArray{QJsonArray{0, 0, "scurve", 0.5}, QJsonArray{frameJson(10), 1}}}})
        })["isError"].toBool());
        QVERIFY(edits(server, {operation("split_clip", "p1", {{"time", frameJson(7)}})})["isError"].toBool());

        // 4. delete_track removes clips on that track plus their linked partners, re-indexes higher tracks, and rejects deleting last track.
        QVERIFY(!edits(server, {
            operation("clip", "p3", {{"track", 1}, {"text", "Unlinked"}, {"start", frameJson(20)}, {"duration", frameJson(4)}})
        })["isError"].toBool());
        const QString bgTrackId = QString::fromStdString(c.project.show.tracks[0].id);
        QVERIFY(!edits(server, {QJsonObject{{"type", "delete_track"}, {"id", bgTrackId}}})["isError"].toBool());
        QCOMPARE(c.project.show.clips.size(), size_t(1));
        QCOMPARE(c.project.show.clips.front().id, std::string("p3"));
        QCOMPARE(c.project.show.clips.front().track, 0);
        c.undo();
        QCOMPARE(c.project.show.clips.size(), size_t(3));
        QJsonArray deleteAllTracks;
        for (const auto& t : c.project.show.tracks) deleteAllTracks.append(QJsonObject{{"type", "delete_track"}, {"id", QString::fromStdString(t.id)}});
        QVERIFY(edits(server, deleteAllTracks)["isError"].toBool());
        QCOMPARE(c.project.show.tracks.size(), size_t(6));

        // 5. delete_asset rejects in-use assets and deletes unused assets.
        c.replace(demoProject());
        QVERIFY(edits(server, {QJsonObject{{"type", "delete_asset"}, {"id", "demo-backdrop"}}})["isError"].toBool());
        QVERIFY(!edits(server, {
            QJsonObject{{"type", "delete_clip"}, {"id", QString::fromStdString(c.project.show.clips.front().id)}},
            QJsonObject{{"type", "delete_asset"}, {"id", "demo-backdrop"}}
        })["isError"].toBool());
        QVERIFY(!c.project.assets.contains("demo-backdrop"));
        c.undo();
        QVERIFY(c.project.assets.contains("demo-backdrop"));
        server.stop();
    }
    void knowledgeEnvironmentHistoryAndFullControlTools() {
        MainWindow window;
        window.controller().replace(demoProject());
        window.show();
        auto& server = window.mcpServer();
        QVERIFY(server.start(0));

        // 1. Embedded manual resources & get_app_guide (works read-only and in Show mode)
        window.controller().setShowMode(true);
        const auto resources = rpc(server, "resources/list").body["result"].toObject()["resources"].toArray();
        QVERIFY(resources.size() >= 13);
        const auto docRes = rpc(server, "resources/read", {{"uri", "simpleavshow://docs/audio-and-outputs"}}).body["result"].toObject();
        QVERIFY(docRes["contents"].toArray().first().toObject()["text"].toString().contains("ASIO"));
        const auto guideOverview = call(server, "get_app_guide")["structuredContent"].toObject();
        QVERIFY(guideOverview["topics"].toArray().size() >= 11);
        QVERIFY(guideOverview["content"].toString().contains("Seven rules"));
        const auto guideSearch = call(server, "get_app_guide", {{"query", "ASIO"}})["structuredContent"].toObject();
        QVERIFY(!guideSearch["matches"].toArray().isEmpty());
        QVERIFY(call(server, "get_app_guide", {{"topic", "nonexistent"}})["isError"].toBool());

        // 2. get_environment works read-only in Show mode without probing live audio/MIDI drivers
        const auto envLocked = call(server, "get_environment")["structuredContent"].toObject();
        QCOMPARE(envLocked["app"].toObject()["name"].toString(), QString("SimpleAVShow"));
        QCOMPARE(envLocked["audio"].toObject()["probed"].toBool(), false);
        QVERIFY(!envLocked["remote"].toObject().contains("links"));

        // 3. encode_midi covers musical types and SysEx
        const auto noteOn = call(server, "encode_midi", {{"type", "note_on"}, {"channel", 2}, {"note", 60}, {"velocity", 110}})["structuredContent"].toObject();
        QCOMPARE(noteOn["bytes"].toArray(), QJsonArray({0x91, 60, 110}));
        const auto pitch = call(server, "encode_midi", {{"type", "pitch_bend"}, {"channel", 1}, {"bend", 0}})["structuredContent"].toObject();
        QCOMPARE(pitch["bytes"].toArray(), QJsonArray({0xE0, 0, 64}));
        const auto sysex = call(server, "encode_midi", {{"type", "raw_hex"}, {"hex", "F0 7D 01 F7"}})["structuredContent"].toObject();
        QCOMPARE(sysex["bytes"].toArray(), QJsonArray({0xF0, 0x7D, 0x01, 0xF7}));
        QVERIFY(call(server, "encode_midi", {{"type", "raw_hex"}, {"hex", "F0 7D"}})["isError"].toBool());

        // 4. Show mode blocks every mutating/control tool (including list_media_files) even when editing is enabled
        server.setEditingEnabled(true);
        QVERIFY(call(server, "transport_control", {{"action", "seek"}, {"time", frameJson(4)}})["isError"].toBool());
        QVERIFY(call(server, "seek_and_select", {{"time", frameJson(4)}})["isError"].toBool());
        QVERIFY(call(server, "list_media_files", {{"directory", QDir::tempPath()}})["isError"].toBool());
        QVERIFY(call(server, "configure_remote", {{"enabled", true}})["isError"].toBool());
        QVERIFY(call(server, "project_action", {{"action", "new_show"}, {"discard_unsaved", true}})["isError"].toBool());
        window.controller().setShowMode(false);

        // Now stopped & out of Show mode: get_environment probes safely and never leaks remote links
        const auto envUnlocked = call(server, "get_environment")["structuredContent"].toObject();
        QCOMPARE(envUnlocked["audio"].toObject()["probed"].toBool(), true);
        QVERIFY(envUnlocked["displays"].toObject()["screens"].toArray().size() >= 1);
        QVERIFY(!envUnlocked["remote"].toObject().contains("links"));

        // 5. move_track reorders tracks and remaps clip indices, rejects oversized index, and history undoes/redoes
        const QString overlayId = QString::fromStdString(window.controller().project.show.tracks[1].id);
        QVERIFY(edits(server, {operation("move_track", overlayId, {{"index", 1e12}})})["isError"].toBool());
        QVERIFY(!edits(server, {operation("move_track", overlayId, {{"index", 0}})})["isError"].toBool());
        QCOMPARE(QString::fromStdString(window.controller().project.show.tracks[0].id), overlayId);
        // Backdrop clip (originally on track 0) shifted to track 1; Title clip (originally on track 1) shifted to track 0.
        QCOMPARE(window.controller().project.show.clips[0].track, 1);
        QCOMPARE(window.controller().project.show.clips[1].track, 0);
        const auto undone = call(server, "history", {{"expected_revision", revision(server)}, {"action", "undo"}});
        QVERIFY(!undone["isError"].toBool());
        QCOMPARE(QString::fromStdString(window.controller().project.show.tracks[1].id), overlayId);
        const auto redone = call(server, "history", {{"expected_revision", revision(server)}, {"action", "redo"}});
        QVERIFY(!redone["isError"].toBool());
        QCOMPARE(QString::fromStdString(window.controller().project.show.tracks[0].id), overlayId);

        // 6. seek_and_select and transport_control (seek / stop_all)
        const QString firstClipId = QString::fromStdString(window.controller().project.show.clips.front().id);
        const auto nav = call(server, "seek_and_select", {{"time", frameJson(5)}, {"clip_id", firstClipId}});
        QVERIFY(!nav["isError"].toBool());
        QCOMPARE(window.controller().position(), frames(5));
        QCOMPARE(nav["structuredContent"].toObject()["selection"].toObject()["selected_clip"].toString(), firstClipId);
        QVERIFY(!call(server, "transport_control", {{"action", "seek"}, {"time", frameJson(2)}})["isError"].toBool());
        QCOMPARE(window.controller().position(), frames(2));
        // Explicit pause on a Held transport must never resume the hold.
        float samples[2]{};
        window.controller().seek(frames(12)); window.controller().transport.render(samples, 1, 2);
        window.controller().transport.command({Action::Play}); window.controller().transport.render(samples, 1, 2);
        QCOMPARE(window.controller().transport.state(), State::Held);
        QVERIFY(!call(server, "transport_control", {{"action", "pause"}})["isError"].toBool());
        window.controller().transport.render(samples, 1, 2);
        QCOMPARE(window.controller().transport.state(), State::Held);
        QVERIFY(!call(server, "transport_control", {{"action", "stop_all"}})["isError"].toBool());
        window.controller().transport.render(samples, 1, 2);
        // Continue must only resume a Held transport, never restart a Stopped show.
        QVERIFY(call(server, "transport_control", {{"action", "continue"}})["isError"].toBool());
        // A pending StopAll must block an agent Play until the audio callback applies the stop.
        window.controller().transport.requestStopAll();
        QVERIFY(call(server, "transport_control", {{"action", "play"}})["isError"].toBool());
        window.controller().transport.render(samples, 1, 2);
        QCOMPARE(window.controller().transport.state(), State::Stopped);

        // 7. list_media_files, relink_media, and project_action (save_as, export_package, open_project, new_show)
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QImage img(16, 16, QImage::Format_RGB32); img.fill(Qt::blue);
        const QString imgPath = dir.filePath("stage.png");
        QVERIFY(img.save(imgPath));
        const auto listed = call(server, "list_media_files", {{"directory", dir.path()}});
        QVERIFY(!listed["isError"].toBool());
        QCOMPARE(listed["structuredContent"].toObject()["count"].toInt(), 1);
        QCOMPARE(listed["structuredContent"].toObject()["files"].toArray().first().toObject()["kind"].toString(), QString("image"));

        // Save, verify preflight flags closed audience display on a saved picture show, and verify failed Save As errors on a clean show
        const QString showPath = dir.filePath("agent-show.savshow");
        const QString packPath = dir.filePath("agent-show.savpack");
        QVERIFY(!call(server, "project_action", {{"action", "save_as"}, {"path", showPath}})["isError"].toBool());
        QVERIFY(QFileInfo::exists(showPath));
        QVERIFY(!window.controller().dirty);
        const auto savedPf = call(server, "get_preflight")["structuredContent"].toObject();
        QCOMPARE(savedPf["ready"].toBool(), false);
        bool sawClosedDisplay = false;
        for (const auto& p : savedPf["problems"].toArray())
            if (p.toString().contains("program output is not open")) sawClosedDisplay = true;
        QVERIFY(sawClosedDisplay);
        QVERIFY(call(server, "project_action", {{"action", "save_as"}, {"path", dir.filePath("no-such-subdir/fail.savshow")}})["isError"].toBool());
        // Existing files are protected from silent overwrite unless overwrite: true; ordinary save updates the current file.
        QVERIFY(call(server, "project_action", {{"action", "save_as"}, {"path", showPath}})["isError"].toBool());
        QVERIFY(!call(server, "project_action", {{"action", "save_as"}, {"path", showPath}, {"overwrite", true}})["isError"].toBool());
        QVERIFY(!call(server, "project_action", {{"action", "save"}})["isError"].toBool());
        // A different show's .autosave sidecar at the destination must not be deleted unless overwrite: true.
        const QString otherShowPath = dir.filePath("other-show.savshow");
        { QFile sidecar(otherShowPath + ".autosave"); QVERIFY(sidecar.open(QIODevice::WriteOnly)); sidecar.write("{}"); }
        QVERIFY(call(server, "project_action", {{"action", "save_as"}, {"path", otherShowPath}})["isError"].toBool());
        QVERIFY(QFileInfo::exists(otherShowPath + ".autosave"));
        QVERIFY(!QFileInfo::exists(otherShowPath));
        QVERIFY(!call(server, "project_action", {{"action", "save_as"}, {"path", otherShowPath}, {"overwrite", true}})["isError"].toBool());
        QVERIFY(QFileInfo::exists(otherShowPath));
        QVERIFY(!call(server, "project_action", {{"action", "save_as"}, {"path", showPath}, {"overwrite", true}})["isError"].toBool());
#ifndef Q_OS_WIN
        // Saving through a symlink in another directory must compute relative media paths against the canonical manifest directory.
        QVERIFY(QDir(dir.path()).mkpath("shortcuts"));
        const QString symlinkShowPath = dir.filePath("shortcuts/link.savshow");
        QVERIFY(QFile::link(showPath, symlinkShowPath));
        QVERIFY(!call(server, "project_action", {{"action", "save"}, {"path", symlinkShowPath}})["isError"].toBool());
        QCOMPARE(QFileInfo(window.controller().path).canonicalFilePath(), QFileInfo(showPath).canonicalFilePath());
        // A .savshow symlink pointing to a non-.savshow media file must be rejected even with overwrite: true.
        const QString mediaAlias = dir.filePath("alias.savshow");
        QVERIFY(QFile::link(imgPath, mediaAlias));
        QVERIFY(call(server, "project_action", {{"action", "save_as"}, {"path", mediaAlias}, {"overwrite", true}})["isError"].toBool());
        // Read-only destinations must be rejected, and private permissions (0600) must be preserved on save.
        const QString roShow = dir.filePath("readonly.savshow");
        QVERIFY(!call(server, "project_action", {{"action", "save_as"}, {"path", roShow}})["isError"].toBool());
        QVERIFY(QFile::setPermissions(roShow, QFileDevice::ReadOwner));
        QVERIFY(call(server, "project_action", {{"action", "save_as"}, {"path", roShow}, {"overwrite", true}})["isError"].toBool());
        QVERIFY(QFile::setPermissions(roShow, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        QVERIFY(!call(server, "project_action", {{"action", "save_as"}, {"path", roShow}, {"overwrite", true}})["isError"].toBool());
        QVERIFY(!(QFileInfo(roShow).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)));
        QVERIFY(!call(server, "project_action", {{"action", "save_as"}, {"path", showPath}, {"overwrite", true}})["isError"].toBool());
#endif
        QVERIFY(!call(server, "project_action", {{"action", "export_package"}, {"path", packPath}})["isError"].toBool());
        QVERIFY(QFileInfo::exists(packPath));
        QVERIFY(call(server, "project_action", {{"action", "export_package"}, {"path", packPath}})["isError"].toBool());
        QVERIFY(!call(server, "project_action", {{"action", "export_package"}, {"path", packPath}, {"overwrite", true}})["isError"].toBool());
#ifndef Q_OS_WIN
        // Read-only packages must not be overwritten, and private permissions (0600) must be preserved on overwrite.
        QVERIFY(QFile::setPermissions(packPath, QFileDevice::ReadOwner));
        QVERIFY(call(server, "project_action", {{"action", "export_package"}, {"path", packPath}, {"overwrite", true}})["isError"].toBool());
        QVERIFY(QFile::setPermissions(packPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        QVERIFY(!call(server, "project_action", {{"action", "export_package"}, {"path", packPath}, {"overwrite", true}})["isError"].toBool());
        QVERIFY(!(QFileInfo(packPath).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)));
#endif
        // Show-replacing actions require expected_revision and reject stale revisions even with discard_unsaved: true.
        QVERIFY(call(server, "project_action", {{"action", "new_show"}, {"discard_unsaved", true}})["isError"].toBool());
        QVERIFY(call(server, "project_action", {{"action", "new_show"}, {"expected_revision", revision(server) - 1}, {"discard_unsaved", true}})["isError"].toBool());
        QVERIFY(!call(server, "project_action", {{"action", "new_show"}, {"expected_revision", revision(server)}})["isError"].toBool());
        QVERIFY(window.controller().project.show.clips.empty());
        QVERIFY(!call(server, "project_action", {{"action", "open_project"}, {"path", showPath}, {"expected_revision", revision(server)}})["isError"].toBool());
        QCOMPARE(window.controller().project.title, QString("The opening scene"));

        // Simulate colliding basenames: unique relative suffix relinks, ambiguous stays offline and is reported.
        QVERIFY(QDir(dir.path()).mkpath("act1"));
        QVERIFY(QDir(dir.path()).mkpath("act2"));
        QVERIFY(img.save(dir.filePath("act1/dup.png")));
        QVERIFY(img.save(dir.filePath("act2/dup.png")));
        QVERIFY(window.controller().edit([&](Project& p) {
            MediaAsset off1; off1.id = "off-1"; off1.name = "stage.png"; off1.path = "/missing/stage.png"; off1.offline = true; off1.problem = "Missing";
            MediaAsset off2; off2.id = "off-2"; off2.name = "act1-dup"; off2.path = "/missing/act1/dup.png"; off2.offline = true; off2.problem = "Missing";
            MediaAsset off3; off3.id = "off-3"; off3.name = "other-dup"; off3.path = "/missing/other/dup.png"; off3.offline = true; off3.problem = "Missing";
            p.assets.insert(off1.id, off1);
            p.assets.insert(off2.id, off2);
            p.assets.insert(off3.id, off3);
        }));
        QVERIFY(call(server, "project_action", {{"action", "new_show"}, {"expected_revision", revision(server)}})["isError"].toBool()); // Dirty without discard_unsaved
        const auto relinked = call(server, "relink_media", {{"expected_revision", revision(server)}, {"directory", dir.path()}});
        QVERIFY(!relinked["isError"].toBool());
        const auto relinkInfo = relinked["structuredContent"].toObject()["relink"].toObject();
        QCOMPARE(relinkInfo["relinked"].toInt(), 2);
        QCOMPARE(relinkInfo["remaining_offline"].toInt(), 1);
        QCOMPARE(relinkInfo["ambiguous"].toArray().size(), 1);
        QVERIFY(!window.controller().project.assets["off-1"].offline);
        QVERIFY(!window.controller().project.assets["off-2"].offline);
        QVERIFY(window.controller().project.assets["off-2"].path.endsWith("act1/dup.png"));
        QVERIFY(window.controller().project.assets["off-3"].offline);

        // 8. configure_remote toggles cleanly, keeps existing listener & key alive on failed port change, and never leaks links
        QTcpServer probe, blocker;
        QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
        const int freeRemotePort = probe.serverPort();
        probe.close();
        QVERIFY(blocker.listen(QHostAddress::AnyIPv4, 0));
        const int occupiedPort = blocker.serverPort();
        const auto remOn = call(server, "configure_remote", {{"enabled", true}, {"port", freeRemotePort}});
        QVERIFY(!remOn["isError"].toBool());
        QVERIFY(window.remoteServer().running());
        const QString originalKey = window.remoteServer().key();
        QVERIFY(!remOn["structuredContent"].toObject().contains("links"));
        // Failed port change + rotate must leave existing remote running with its original port and key.
        QVERIFY(call(server, "configure_remote", {{"enabled", true}, {"port", occupiedPort}, {"change_key", true}})["isError"].toBool());
        QVERIFY(window.remoteServer().running());
        QCOMPARE(static_cast<int>(window.remoteServer().port()), freeRemotePort);
        QCOMPARE(window.remoteServer().key(), originalKey);
        blocker.close();
        // Malformed enabled type must fail without stopping the running remote
        QVERIFY(call(server, "configure_remote", {{"enabled", "false"}})["isError"].toBool());
        QVERIFY(window.remoteServer().running());
        const auto remOff = call(server, "configure_remote", {{"enabled", false}});
        QVERIFY(!remOff["isError"].toBool());
        QVERIFY(!window.remoteServer().running());

        // 9. configure_outputs validates nested schemas before side effects & guards editor display by name
        QVERIFY(call(server, "configure_outputs", {{"midi", QJsonObject{}}})["isError"].toBool());
        QVERIFY(call(server, "configure_outputs", {{"display", QJsonObject{}}})["isError"].toBool());
        QVERIFY(call(server, "configure_outputs", {{"display", QJsonObject{{"open", "yes"}}}})["isError"].toBool());
        QVERIFY(call(server, "configure_outputs", {{"display", QJsonObject{{"open", true}}}})["isError"].toBool());
        QVERIFY(call(server, "configure_outputs", {{"display", QJsonObject{{"open", true}, {"screen_name", "NoSuchDisplay"}}}})["isError"].toBool());
        const auto envScreens = call(server, "get_environment")["structuredContent"].toObject()["displays"].toObject()["screens"].toArray();
        if (!envScreens.isEmpty()) {
            const QString editorScreenName = envScreens.first().toObject()["name"].toString();
            QVERIFY(call(server, "configure_outputs", {{"display", QJsonObject{{"open", true}, {"screen_name", editorScreenName}}}})["isError"].toBool());
        }
        QVERIFY(!call(server, "configure_outputs", {{"midi", QJsonObject{{"port", "none"}}}})["isError"].toBool());
        // test_midi must be rejected when disconnecting MIDI or when no MIDI destination is connected.
        QVERIFY(call(server, "configure_outputs", {
            {"midi", QJsonObject{{"port", "none"}}},
            {"test_midi", QJsonObject{{"message", QJsonArray{0x90, 60, 100}}}}
        })["isError"].toBool());
        QVERIFY(call(server, "configure_outputs", {
            {"test_midi", QJsonObject{{"message", QJsonArray{0x90, 60, 100}}}}
        })["isError"].toBool());

        // 10. encode_midi must reject oversized integers that QJsonValue::toInt() would turn into 0.
        QVERIFY(call(server, "encode_midi", {{"type", "note_on"}, {"channel", 1}, {"note", 2147483648.0}, {"velocity", 100}})["isError"].toBool());

        // 11. split_clip must reassign linked partners wholly after the cut to the tail link.
        {
            auto& ctrl = window.controller();
            Project p = newProject();
            Clip vis; vis.id = "vis-link"; vis.track = 0; vis.text = "Hello"; vis.start = 0; vis.duration = frames(10); vis.link = "grp-1";
            Clip tailPartner; tailPartner.id = "tail-partner"; tailPartner.track = 1; tailPartner.text = "Tail"; tailPartner.start = frames(6); tailPartner.duration = frames(4); tailPartner.link = "grp-1";
            p.show.clips.push_back(vis);
            p.show.clips.push_back(tailPartner);
            ctrl.replace(std::move(p));
            const int rev = call(server, "get_show")["structuredContent"].toObject()["revision"].toInt();
            const auto splitRes = call(server, "apply_edits", {{"expected_revision", rev}, {"operations", QJsonArray{
                QJsonObject{{"type", "split_clip"}, {"id", "vis-link"}, {"fields", QJsonObject{
                    {"time", static_cast<double>(frames(5))}, {"new_id", "vis-tail"}
                }}}
            }}});
            QVERIFY(!splitRes["isError"].toBool());
            std::string headLink, tailLink, partnerLink;
            for (const auto& c : ctrl.project.show.clips) {
                if (c.id == "vis-link") headLink = c.link;
                else if (c.id == "vis-tail") tailLink = c.link;
                else if (c.id == "tail-partner") partnerLink = c.link;
            }
            QVERIFY(!headLink.empty() && !tailLink.empty() && headLink != tailLink);
            QCOMPARE(partnerLink, tailLink);
        }

        // 12. Exclusive busy lease rejects concurrent acquisition and wrong-owner release.
        {
            auto& ctrl = window.controller();
            const quint64 lease = ctrl.acquireBusy();
            QVERIFY(lease != 0);
            QVERIFY(ctrl.busy);
            QCOMPARE(ctrl.acquireBusy(), quint64(0));
            ctrl.releaseBusy(lease + 999);
            QVERIFY(ctrl.busy);
            ctrl.releaseBusy(lease);
            QVERIFY(!ctrl.busy);
        }

        // 13. Queued Play + Controller::pause() drain in arrival order, and idle pause() never fills the queue.
        {
            Controller c;
            c.replace(demoProject());
            c.transport.command({Action::Play});
            QVERIFY(c.pause());
            QCOMPARE(c.transport.state(), State::Paused);
            // 200 idle pause calls must never overflow the 128-slot transport queue.
            for (int i = 0; i < 200; ++i) QVERIFY(c.pause());
            QVERIFY(c.seek(frames(1)));
            QVERIFY(c.stopAll());
        }

        // 14. Opening a .savshow.autosave sidecar via MCP strips .autosave and marks dirty.
        {
            QTemporaryDir dir;
            QVERIFY(dir.isValid());
            const QString baseShow = dir.filePath("rehearsal.savshow");
            const QString autosaveFile = baseShow + ".autosave";
            writeProject(newProject(), autosaveFile);
            const auto openAuto = call(server, "project_action", {
                {"action", "open_project"}, {"path", autosaveFile}, {"discard_unsaved", true},
                {"expected_revision", revision(server)}
            });
            QVERIFY(!openAuto["isError"].toBool());
            QCOMPARE(window.controller().path, baseShow);
            QVERIFY(window.controller().dirty);
            QVERIFY(window.windowTitle().contains("*"));
        }

        // 15. get_preflight skips synchronous hardware probing during Show mode and reports disconnected audio.
        {
            window.controller().setShowMode(true);
            const auto pfShow = call(server, "get_preflight")["structuredContent"].toObject();
            QCOMPARE(pfShow["hardware_probed"].toBool(), false);
            QCOMPARE(pfShow["ready"].toBool(), false);
            window.controller().setShowMode(false);
        }

        // 16. Transport::forceStopAll() stops playback even if the command queue is full.
        {
            Controller c;
            c.replace(demoProject());
            c.transport.command({Action::Play});
            c.transport.render(nullptr, 0, 0);
            QCOMPARE(c.transport.state(), State::Playing);
            for (int i = 0; i < 200; ++i) c.transport.command({Action::Play});
            c.transport.forceStopAll();
            QCOMPARE(c.transport.state(), State::Stopped);
        }

        // 17. Revocable chat tokens authenticate independently and immediately fail with 401 on revoke,
        //     without disrupting the primary external MCP bearer token.
        {
            const QString chatTok = server.issueChatToken();
            QVERIFY(!chatTok.isEmpty());
            QVERIFY(chatTok != server.token());
            const QByteArray ping1 = QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", 501}, {"method", "ping"}}).toJson(QJsonDocument::Compact);
            const auto okChat = http(server, ping1, "POST", {{"Authorization", "Bearer " + chatTok.toUtf8()}});
            QCOMPARE(okChat.status, 200);
            server.revokeChatToken(chatTok);
            const auto revokedChat = http(server, ping1, "POST", {{"Authorization", "Bearer " + chatTok.toUtf8()}});
            QCOMPARE(revokedChat.status, 401);
            const auto okPrimary = http(server, ping1, "POST");
            QCOMPARE(okPrimary.status, 200);
        }

        // 18. Staged import_media is invalidated when the show is replaced so it cannot commit into another show.
        {
            QTemporaryDir dir;
            QVERIFY(dir.isValid());
            const QString imgPath = dir.filePath("card.png");
            QImage img(32, 32, QImage::Format_RGBA8888);
            img.fill(Qt::red);
            QVERIFY(img.save(imgPath, "PNG"));
            const auto imp = call(server, "import_media", {
                {"expected_revision", revision(server)},
                {"paths", QJsonArray{imgPath}}
            })["structuredContent"].toObject();
            const QString jobId = imp["job_id"].toString();
            QVERIFY(!jobId.isEmpty());
            QTRY_COMPARE_WITH_TIMEOUT(
                call(server, "get_import", {{"job_id", jobId}})["structuredContent"].toObject()["state"].toString(),
                QString("ready"), 5000);
            // Replacing the show must cancel the staged import and reject commit_import.
            window.controller().replace(newProject());
            const auto stAfter = call(server, "get_import", {{"job_id", jobId}})["structuredContent"].toObject();
            QCOMPARE(stAfter["state"].toString(), QString("cancelled"));
            const auto commitErr = call(server, "commit_import", {
                {"expected_revision", revision(server)},
                {"job_id", jobId}
            });
            QVERIFY(commitErr["isError"].toBool());
        }

        // 19. initialize issues a server-generated Mcp-Session-Id instead of echoing a caller-supplied value,
        //     unknown/evicted session IDs return HTTP 404, and per-bearer FIFO eviction protects other bearers.
        {
            const QByteArray initBody = QJsonDocument(QJsonObject{
                {"jsonrpc", "2.0"}, {"id", 601}, {"method", "initialize"},
                {"params", QJsonObject{
                    {"protocolVersion", "2025-11-25"},
                    {"capabilities", QJsonObject{}},
                    {"clientInfo", QJsonObject{{"name", "test"}, {"version", "1"}}}
                }}
            }).toJson(QJsonDocument::Compact);
            const QByteArray pingBody = QJsonDocument(QJsonObject{
                {"jsonrpc", "2.0"}, {"id", 602}, {"method", "ping"}
            }).toJson(QJsonDocument::Compact);
            const auto initRes = http(server, initBody, "POST", {{"Mcp-Session-Id", "caller-forged-scope"}});
            QCOMPARE(initRes.status, 200);
            QVERIFY(!initRes.sessionId.isEmpty());
            QVERIFY(initRes.sessionId != QByteArray("caller-forged-scope"));
            // Unknown / forged Mcp-Session-Id on non-initialize must return HTTP 404.
            QCOMPARE(http(server, pingBody, "POST", {{"Mcp-Session-Id", "caller-forged-scope"}}).status, 404);
            QCOMPARE(http(server, pingBody, "POST", {{"Mcp-Session-Id", initRes.sessionId}}).status, 200);
            // A second bearer churning >16 sessions evicts its own oldest session (404) without evicting primary's session.
            const QString churnTok = server.issueChatToken();
            QByteArray firstChurnSid;
            for (int i = 0; i < 18; ++i) {
                const auto r = http(server, initBody, "POST", {{"Authorization", "Bearer " + churnTok.toUtf8()}});
                QCOMPARE(r.status, 200);
                if (i == 0) firstChurnSid = r.sessionId;
            }
            QCOMPARE(http(server, pingBody, "POST", {
                {"Authorization", "Bearer " + churnTok.toUtf8()},
                {"Mcp-Session-Id", firstChurnSid}
            }).status, 404);
            QCOMPARE(http(server, pingBody, "POST", {{"Mcp-Session-Id", initRes.sessionId}}).status, 200);
            server.revokeChatToken(churnTok);
        }

        // 20. Revoking a chat token cancels a chat-owned staged import without disturbing an external client's import.
        {
            QTemporaryDir dir;
            QVERIFY(dir.isValid());
            const QString imgPath = dir.filePath("owner.png");
            QImage img(16, 16, QImage::Format_RGBA8888);
            img.fill(Qt::green);
            QVERIFY(img.save(imgPath, "PNG"));

            const QString chatTok = server.issueChatToken();
            const QByteArray startChatImport = QJsonDocument(QJsonObject{
                {"jsonrpc", "2.0"}, {"id", 701}, {"method", "tools/call"},
                {"params", QJsonObject{{"name", "import_media"}, {"arguments", QJsonObject{
                    {"expected_revision", revision(server)}, {"paths", QJsonArray{imgPath}}
                }}}}
            }).toJson(QJsonDocument::Compact);
            const auto chatImp = http(server, startChatImport, "POST", {{"Authorization", "Bearer " + chatTok.toUtf8()}});
            QCOMPARE(chatImp.status, 200);
            const QString chatJobId = chatImp.body["result"].toObject()["structuredContent"].toObject()["job_id"].toString();
            QVERIFY(!chatJobId.isEmpty());
            server.revokeChatToken(chatTok);
            const auto stAfterRevoke = call(server, "get_import", {{"job_id", chatJobId}})["structuredContent"].toObject();
            QCOMPARE(stAfterRevoke["state"].toString(), QString("cancelled"));

            // Now an external client starts an import; revoking an unrelated chat token must leave it ready.
            const QString otherChatTok = server.issueChatToken();
            const auto extImp = call(server, "import_media", {
                {"expected_revision", revision(server)}, {"paths", QJsonArray{imgPath}}
            })["structuredContent"].toObject();
            const QString extJobId = extImp["job_id"].toString();
            QTRY_COMPARE_WITH_TIMEOUT(
                call(server, "get_import", {{"job_id", extJobId}})["structuredContent"].toObject()["state"].toString(),
                QString("ready"), 5000);
            server.revokeChatToken(otherChatTok);
            QCOMPARE(call(server, "get_import", {{"job_id", extJobId}})["structuredContent"].toObject()["state"].toString(),
                     QString("ready"));
        }

        // 21. Queued Continue + Stop All (both normal and full-queue) discards Continue without firing resume MIDI,
        //     and invalidated agent transport commands are skipped before execution.
        {
            Controller c;
            Project p = newProject();
            Marker hold; hold.id = "hold-1"; hold.name = "Hold"; hold.time = 0; hold.hold = true;
            hold.timing = MidiTiming::Resume; hold.message = {0x90, 60, 100};
            p.show.markers.push_back(hold);
            c.replace(std::move(p));
            c.transport.command({Action::Play});
            float buf[2]{};
            c.transport.render(buf, 1, 2);
            QCOMPARE(c.transport.state(), State::Held);
            MidiEvent ev;
            while (c.transport.takeMidi(ev)) {}
            // Ordinary queued Continue followed by StopAll must not publish resume MIDI,
            // while a queued Seek followed by StopAll preserves the seeked position.
            QVERIFY(c.transport.command({Action::Seek, frames(2)}));
            QVERIFY(c.transport.command({Action::Play}));
            QVERIFY(c.transport.command({Action::StopAll}));
            c.transport.render(buf, 1, 2);
            QCOMPARE(c.transport.state(), State::Stopped);
            QCOMPARE(c.transport.position(), frames(2));
            QVERIFY(!c.transport.takeMidi(ev));
            // Re-enter Held and verify chat-agent invalidation skips queued ChatAgent Continue
            // without cancelling an external MCP client's accepted StopAll.
            c.transport.command({Action::Seek, 0});
            c.transport.command({Action::Play});
            c.transport.render(buf, 1, 2);
            QCOMPARE(c.transport.state(), State::Held);
            while (c.transport.takeMidi(ev)) {}
            QVERIFY(c.transport.command({Action::Play, 0, CommandSource::ChatAgent, c.transport.chatAgentEpoch()}));
            QVERIFY(c.transport.command({Action::StopAll, 0, CommandSource::ExternalAgent, c.transport.externalAgentEpoch()}));
            c.transport.invalidateChatAgentCommands();
            c.transport.render(buf, 1, 2);
            QCOMPARE(c.transport.state(), State::Stopped);
            QVERIFY(!c.transport.takeMidi(ev));
            // Full-queue emergency Stop All also discards queued Play without firing resume MIDI.
            for (int i = 0; i < 200; ++i) c.transport.command({Action::Play});
            c.transport.requestStopAll();
            QVERIFY(!c.transport.command({Action::Play}));
            c.transport.render(buf, 1, 2);
            QCOMPARE(c.transport.state(), State::Stopped);
            QVERIFY(!c.transport.takeMidi(ev));
        }

        // 22. open_project rejects non-project file extensions before reading the file,
        //     and preflightLock blocks MCP edits without blocking phone remote Stop All.
        {
            QTemporaryDir dir;
            QVERIFY(dir.isValid());
            const QString bogusMedia = dir.filePath("recording.wav");
            QFile f(bogusMedia);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("RIFF0000WAVE");
            f.close();
            const auto openBad = call(server, "project_action", {
                {"action", "open_project"}, {"path", bogusMedia}, {"discard_unsaved", true},
                {"expected_revision", revision(server)}
            });
            QVERIFY(openBad["isError"].toBool());

            // A valid .savshow with a UTF-8 BOM and >64 bytes of leading whitespace must still open cleanly.
            const QString bomShow = dir.filePath("bom_padded.savshow");
            writeProject(newProject(), bomShow);
            QFile sf(bomShow);
            QVERIFY(sf.open(QIODevice::ReadOnly));
            const QByteArray rawJson = sf.readAll();
            sf.close();
            QVERIFY(sf.open(QIODevice::WriteOnly | QIODevice::Truncate));
            sf.write("\xEF\xBB\xBF");
            sf.write(QByteArray(128, ' '));
            sf.write(rawJson);
            sf.close();
            const auto openBom = call(server, "project_action", {
                {"action", "open_project"}, {"path", bomShow}, {"discard_unsaved", true},
                {"expected_revision", revision(server)}
            });
            QVERIFY(!openBom["isError"].toBool());

            window.controller().preflightLock = true;
            const auto lockedEdit = call(server, "apply_edits", {
                {"expected_revision", revision(server)},
                {"operations", QJsonArray{QJsonObject{{"type", "project"}, {"fields", QJsonObject{{"title", "Blocked"}}}}}}
            });
            QVERIFY(lockedEdit["isError"].toBool());
            QVERIFY(window.remoteServer().command("stop"));
            window.controller().preflightLock = false;
        }

        // 23. Uncommitted numeric spinbox edit in the inspector must commit before MCP seek_and_select switches selection.
        {
            window.controller().replace(demoProject());
            window.show();
            QApplication::processEvents();
            QVERIFY(window.controller().project.show.clips.size() >= 2);
            // clips[1] is the title clip (start=2s, duration=36s in 70s show), so moving start to 7.5s fits cleanly.
            const QString clipA = QString::fromStdString(window.controller().project.show.clips[1].id);
            const QString clipB = QString::fromStdString(window.controller().project.show.clips[0].id);
            QVERIFY(!call(server, "seek_and_select", {{"clip_id", clipA}})["isError"].toBool());
            auto* startSpin = window.findChild<QDoubleSpinBox*>("clipStart");
            QVERIFY(startSpin && startSpin->isVisible());
            auto* lineEdit = startSpin->findChild<QLineEdit*>();
            QVERIFY(lineEdit);
            startSpin->setFocus();
            lineEdit->selectAll();
            QTest::keyClicks(lineEdit, "7.5"); // keyboardTracking(false): valueChanged has NOT fired yet.
            QVERIFY(!call(server, "seek_and_select", {{"clip_id", clipB}})["isError"].toBool());
            const auto itA = std::find_if(window.controller().project.show.clips.begin(),
                window.controller().project.show.clips.end(),
                [&](const Clip& c) { return c.id == clipA.toStdString(); });
            QVERIFY(itA != window.controller().project.show.clips.end());
            QCOMPARE(itA->start, frames(7.5));
        }

        // 24. MCP delete_track must also delete linked clip partners on other tracks.
        {
            window.controller().replace(newProject());
            auto& p = window.controller().project;
            Clip pic; pic.id = "linked-pic"; pic.track = 0; pic.text = "Slide"; pic.start = 0; pic.duration = frames(2); pic.link = "L1";
            Clip snd; snd.id = "linked-snd"; snd.track = 2; snd.start = 0; snd.duration = frames(2); snd.link = "L1";
            auto pcm = std::make_shared<AudioData>(); pcm->mono = true; pcm->samples.resize(frames(2) * 2, 0.f);
            MediaAsset a; a.id = "a1"; a.name = "Tone"; a.duration = 2; a.audio = pcm;
            p.assets.insert(a.id, a);
            snd.assetId = "a1"; snd.audio = pcm;
            p.show.clips = {pic, snd};
            window.controller().replace(p);
            const QString picTrackId = QString::fromStdString(window.controller().project.show.tracks[0].id);
            const auto delTrack = call(server, "apply_edits", {
                {"expected_revision", revision(server)},
                {"operations", QJsonArray{QJsonObject{{"type", "delete_track"}, {"id", picTrackId}}}}
            });
            QVERIFY(!delTrack["isError"].toBool());
            QVERIFY(window.controller().project.show.clips.empty());
        }

        // 25. Reopening the currently recovered alias autosave via MCP open_project with discard_unsaved: true
        //     must NOT delete that autosave file.
        {
            QTemporaryDir dir;
            QVERIFY(dir.isValid());
            const QString targetShow = dir.filePath("target.savshow");
            const QString aliasAuto = dir.filePath("alias.savshow.autosave");
            writeProject(newProject(), targetShow);
            writeProject(newProject(), aliasAuto);
            window.controller().replace(newProject(), targetShow, true);
            window.controller().setRecoveredAutosavePath(aliasAuto);
            const auto reopen = call(server, "project_action", {
                {"action", "open_project"}, {"path", aliasAuto},
                {"discard_unsaved", true}, {"expected_revision", revision(server)}
            });
            QVERIFY(!reopen["isError"].toBool());
            QVERIFY(QFileInfo::exists(aliasAuto));
        }

        window.controller().dirty = false;
        server.stop();
    }
};
QTEST_MAIN(McpTests)
#include "mcp_tests.moc"
