#include "app/AcpClient.h"
#include "app/AgentChatPanel.h"
#include "app/MainWindow.h"
#include <QtTest>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextBrowser>
#include <QToolButton>

using namespace sav;

class AcpTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("SimpleAVShowTests");
        QCoreApplication::setApplicationName("AcpTests");
        QStandardPaths::setTestModeEnabled(true);
        QSettings().clear();
    }

    void commandLineSplittingAndPresets() {
        const auto presets = defaultAcpPresets();
        QVERIFY(presets.size() >= 6);
        bool hasAntigravity = false;
        for (const auto& p : presets) {
            QVERIFY(p.id != "gemini");
            if (p.id == "antigravity") {
                hasAntigravity = true;
                QCOMPARE(p.program, QString("agy"));
                QCOMPARE(p.args, QStringList({"acp"}));
            }
        }
        QVERIFY(hasAntigravity);
        QCOMPARE(splitCommandLine("opencode acp"), QStringList({"opencode", "acp"}));
        QCOMPARE(splitCommandLine("\"/path/with spaces/agent\" --flag 'hello world'"),
                 QStringList({"/path/with spaces/agent", "--flag", "hello world"}));
        // Windows paths with backslashes (including escaped trailing separator before closing quote) must stay intact.
        QCOMPARE(splitCommandLine("\"C:\\Program Files\\Agent\\agent.exe\" --cwd C:\\Shows\\Act1"),
                 QStringList({"C:\\Program Files\\Agent\\agent.exe", "--cwd", "C:\\Shows\\Act1"}));
        QCOMPARE(splitCommandLine("\"C:\\Shows\\\\\" --flag"),
                 QStringList({"C:\\Shows\\", "--flag"}));
        QCOMPARE(splitCommandLine("agent --msg \"say \\\"hi\\\"\""),
                 QStringList({"agent", "--msg", "say \"hi\""}));
        // Microsoft C doubled-quote escaping inside double-quoted arguments.
        QCOMPARE(splitCommandLine("agent --msg \"say \"\"hi\"\"\""),
                 QStringList({"agent", "--msg", "say \"hi\""}));
        QCOMPARE(splitCommandLine("agent \"\" arg"),
                 QStringList({"agent", "", "arg"}));
        // Unclosed quotes are rejected (including trailing doubled quote with no closing quote).
        QCOMPARE(splitCommandLine("\"C:\\Unclosed"), QStringList());
        QCOMPARE(splitCommandLine("\"foo\"\""), QStringList());
        QCOMPARE(splitCommandLine("  "), QStringList());
        // Windows .cmd/.bat wrapper argument guard rejects cmd.exe metacharacters.
        QString batchErr;
        QVERIFY(validateBatchWrapperArgs("C:\\npm\\codex-acp.cmd", {"--flag", "safe-value"}, batchErr));
        QVERIFY(!validateBatchWrapperArgs("C:\\npm\\codex-acp.cmd", {"--msg", "hello & calc"}, batchErr));
        QVERIFY(!validateBatchWrapperArgs("C:\\npm\\codex-acp.bat", {"%PATH%"}, batchErr));
        QVERIFY(validateBatchWrapperArgs("/usr/local/bin/codex-acp", {"--msg", "hello & world"}, batchErr));
    }

    void acpHttpMcpAndStreaming() {
        AcpClient client;
        QString connectedName, connectedSession;
        QString thoughts, message, finishedReason, stderrText;
        std::vector<AcpToolCallUpdate> toolUpdates;

        connect(&client, &AcpClient::connected, [&](const QString& name, const QString& sid) {
            connectedName = name; connectedSession = sid;
        });
        connect(&client, &AcpClient::thoughtChunk, [&](const QString& t) { thoughts += t; });
        connect(&client, &AcpClient::messageChunk, [&](const QString& m) { message += m; });
        connect(&client, &AcpClient::toolCallUpdated, [&](const AcpToolCallUpdate& u) { toolUpdates.push_back(u); });
        connect(&client, &AcpClient::promptFinished, [&](const QString& r) { finishedReason = r; });
        connect(&client, &AcpClient::stderrReceived, [&](const QString& s) { stderrText += s; });

        AcpMcpConfig mcp{"simpleavshow", "http://127.0.0.1:48123/mcp", "secret-token", SAV_APP_EXECUTABLE};
        client.connectToAgent(SAV_MOCK_ACP_AGENT, {"--http-mcp"}, QDir::tempPath(), mcp);
        QTRY_COMPARE(client.state(), AcpState::Ready);
        QCOMPARE(connectedName, QString("MockACP"));
        QCOMPARE(connectedSession, QString("mock-session-1"));
        QVERIFY(client.agentSupportsHttpMcp());
        QVERIFY(client.usedHttpMcp());
        QVERIFY(stderrText.contains("mock-acp-agent initialized"));

        client.sendPrompt("Hello show");
        QTRY_COMPARE(finishedReason, QString("end_turn"));
        QCOMPARE(client.state(), AcpState::Ready);
        QCOMPARE(thoughts, QString("Inspecting show..."));
        QVERIFY(toolUpdates.size() >= 2);
        QCOMPARE(toolUpdates.front().status, QString("in_progress"));
        QCOMPARE(toolUpdates.back().status, QString("completed"));
        QVERIFY(message.contains("Mock reply: ok"));
        QVERIFY(message.contains("\"type\":\"http\""));
        QVERIFY(message.contains("Bearer secret-token"));
        client.disconnectFromAgent();
        QCOMPARE(client.state(), AcpState::Disconnected);
    }

    void acpStdioFallbackAndMcpProxyEndToEnd() {
        Controller c;
        c.replace(demoProject());
        McpServer server(c);
        QVERIFY(server.start(0));

        // 1. Verify ACP falls back to stdio --mcp-proxy when agent does not advertise HTTP MCP.
        AcpClient client;
        QString message, finishedReason;
        connect(&client, &AcpClient::messageChunk, [&](const QString& m) { message += m; });
        connect(&client, &AcpClient::promptFinished, [&](const QString& r) { finishedReason = r; });

        AcpMcpConfig mcp{"simpleavshow", server.endpoint(), server.token(), SAV_APP_EXECUTABLE};
        client.connectToAgent(SAV_MOCK_ACP_AGENT, {}, QDir::tempPath(), mcp);
        QTRY_COMPARE(client.state(), AcpState::Ready);
        QVERIFY(!client.agentSupportsHttpMcp());
        QVERIFY(!client.usedHttpMcp());
        client.sendPrompt("Check stdio bridge");
        QTRY_COMPARE(finishedReason, QString("end_turn"));
        QVERIFY(!message.contains("\"type\":\"http\""));
        QVERIFY(!message.contains("\"type\":\"stdio\""));
        QVERIFY(message.contains("\"command\""));
        QVERIFY(message.contains("--mcp-proxy"));
        QVERIFY(message.contains("SAV_MCP_TOKEN"));
        client.disconnectFromAgent();

        // 2. Run the real simpleavshow --mcp-proxy against the live McpServer over stdio.
        QProcess proxy;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("SAV_MCP_TOKEN", server.token());
        proxy.setProcessEnvironment(env);
        proxy.start(SAV_APP_EXECUTABLE, {"--mcp-proxy", server.endpoint()});
        QVERIFY(proxy.waitForStarted(5000));

        auto sendLine = [&](const QJsonObject& obj) {
            proxy.write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n");
        };
        auto readLineJson = [&](QByteArray& buffer) -> QJsonObject {
            QElapsedTimer timer; timer.start();
            while (timer.elapsed() < 8000) {
                const qsizetype nl = buffer.indexOf('\n');
                if (nl >= 0) {
                    const QByteArray line = buffer.left(nl).trimmed();
                    buffer.remove(0, nl + 1);
                    if (!line.isEmpty()) return QJsonDocument::fromJson(line).object();
                }
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                buffer.append(proxy.readAllStandardOutput());
            }
            return {};
        };

        QByteArray outBuf;
        // Wait for initialize before issuing subsequent calls, then match concurrent replies by JSON-RPC id.
        sendLine({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", QJsonObject{
            {"protocolVersion", "2025-11-25"},
            {"clientInfo", QJsonObject{{"name", "proxy-test"}, {"version", "1"}}},
            {"capabilities", QJsonObject{}}
        }}});
        const auto initResp = readLineJson(outBuf);
        QCOMPARE(initResp["id"].toInt(), 1);
        QCOMPARE(initResp["result"].toObject()["serverInfo"].toObject()["name"].toString(), QString("simpleavshow"));

        sendLine({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
        sendLine({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"}, {"params", QJsonObject{
            {"name", "get_preflight"}, {"arguments", QJsonObject{}}
        }}});
        sendLine({{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"}, {"params", QJsonObject{
            {"name", "get_app_guide"}, {"arguments", QJsonObject{{"topic", "overview"}}}
        }}});
        proxy.closeWriteChannel();

        QMap<int, QJsonObject> replies;
        while (replies.size() < 2) {
            const auto obj = readLineJson(outBuf);
            QVERIFY(!obj.isEmpty());
            replies.insert(obj["id"].toInt(), obj);
        }
        QTRY_VERIFY_WITH_TIMEOUT(proxy.state() == QProcess::NotRunning, 8000);
        QCOMPARE(proxy.exitCode(), 0);
        QCOMPARE(replies[2]["result"].toObject()["structuredContent"].toObject()["summary"].toObject()["title"].toString(),
                 QString("The opening scene"));
        QVERIFY(replies[3]["result"].toObject()["structuredContent"].toObject()["content"].toString().contains("Seven rules"));
        server.stop();

        // 3. Verify --mcp-proxy returns an ID-correlated JSON-RPC error on an empty 200 OK body instead of dropping the reply.
        QTcpServer emptyServer;
        QVERIFY(emptyServer.listen(QHostAddress::LocalHost, 0));
        connect(&emptyServer, &QTcpServer::newConnection, &emptyServer, [&emptyServer] {
            auto* s = emptyServer.nextPendingConnection();
            connect(s, &QTcpSocket::readyRead, s, [s] {
                s->readAll();
                s->write("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                s->disconnectFromHost();
            });
        });
        QProcess emptyProxy;
        emptyProxy.start(SAV_APP_EXECUTABLE, {"--mcp-proxy", QString("http://127.0.0.1:%1/mcp").arg(emptyServer.serverPort())});
        QVERIFY(emptyProxy.waitForStarted(5000));
        emptyProxy.write("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\"}\n");
        emptyProxy.closeWriteChannel();
        QTRY_VERIFY_WITH_TIMEOUT(emptyProxy.state() == QProcess::NotRunning, 8000);
        const auto errLine = emptyProxy.readAllStandardOutput().trimmed();
        const auto errDoc = QJsonDocument::fromJson(errLine).object();
        QCOMPARE(errDoc["id"].toInt(), 7);
        QVERIFY(errDoc["error"].isObject());

        // 4. Verify --mcp-proxy rejects HTTP 202 for requests, empty JSON objects, and mismatched IDs with ID-correlated errors.
        int badSeq = 0;
        QTcpServer badServer;
        QVERIFY(badServer.listen(QHostAddress::LocalHost, 0));
        connect(&badServer, &QTcpServer::newConnection, &badServer, [&badServer, &badSeq] {
            auto* s = badServer.nextPendingConnection();
            connect(s, &QTcpSocket::readyRead, s, [s, &badSeq] {
                s->readAll();
                const int step = badSeq++;
                if (step == 0) {
                    s->write("HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                } else if (step == 1) {
                    const QByteArray body = "{}";
                    s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                } else {
                    const QByteArray body = "{\"jsonrpc\":\"2.0\",\"id\":999,\"result\":{}}";
                    s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                }
                s->disconnectFromHost();
            });
        });
        QProcess badProxy;
        badProxy.start(SAV_APP_EXECUTABLE, {"--mcp-proxy", QString("http://127.0.0.1:%1/mcp").arg(badServer.serverPort())});
        QVERIFY(badProxy.waitForStarted(5000));
        badProxy.write("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"ping\"}\n");
        badProxy.write("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"ping\"}\n");
        badProxy.write("{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"ping\"}\n");
        badProxy.closeWriteChannel();
        QTRY_VERIFY_WITH_TIMEOUT(badProxy.state() == QProcess::NotRunning, 8000);
        const auto badLines = badProxy.readAllStandardOutput().split('\n');
        QMap<int, QJsonObject> badReplies;
        for (const auto& l : badLines) {
            if (l.trimmed().isEmpty()) continue;
            const auto o = QJsonDocument::fromJson(l).object();
            badReplies.insert(o["id"].toInt(), o);
        }
        for (int reqId : {8, 9, 10}) {
            QVERIFY(badReplies.contains(reqId));
            QVERIFY(badReplies[reqId]["error"].isObject());
        }

        // 5. Verify --mcp-proxy automatically reinitializes (including notifications/initialized) and retries on HTTP 404.
        int reinitStep = 0;
        QTcpServer sessionServer;
        QVERIFY(sessionServer.listen(QHostAddress::LocalHost, 0));
        connect(&sessionServer, &QTcpServer::newConnection, &sessionServer, [&sessionServer, &reinitStep] {
            auto* s = sessionServer.nextPendingConnection();
            connect(s, &QTcpSocket::readyRead, s, [s, &reinitStep] {
                const QByteArray raw = s->readAll();
                const int step = reinitStep++;
                if (step == 0) {
                    const QByteArray body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"serverInfo\":{\"name\":\"t\",\"version\":\"1\"}}}";
                    s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nMcp-Session-Id: sid-1\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                } else if (step == 1 && raw.contains("sid-1")) {
                    const QByteArray body = "{\"jsonrpc\":\"2.0\",\"id\":2,\"error\":{\"code\":-32001,\"message\":\"Session expired\"}}";
                    s->write("HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                } else if (step == 2) {
                    const QByteArray body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"serverInfo\":{\"name\":\"t\",\"version\":\"1\"}}}";
                    s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nMcp-Session-Id: sid-2\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                } else if (step == 3 && raw.contains("sid-2") && raw.contains("notifications/initialized")) {
                    s->write("HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                } else if (step == 4 && raw.contains("sid-2")) {
                    const QByteArray body = "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{}}";
                    s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                } else {
                    s->write("HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                }
                s->disconnectFromHost();
            });
        });
        QProcess reinitProxy;
        reinitProxy.start(SAV_APP_EXECUTABLE, {"--mcp-proxy", QString("http://127.0.0.1:%1/mcp").arg(sessionServer.serverPort())});
        QVERIFY(reinitProxy.waitForStarted(5000));
        reinitProxy.write("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"t\",\"version\":\"1\"}}}\n");
        QTRY_VERIFY_WITH_TIMEOUT(reinitProxy.bytesAvailable() > 0, 5000);
        const auto firstInit = QJsonDocument::fromJson(reinitProxy.readAllStandardOutput().trimmed()).object();
        QCOMPARE(firstInit["id"].toInt(), 1);
        reinitProxy.write("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}\n");
        reinitProxy.closeWriteChannel();
        QTRY_VERIFY_WITH_TIMEOUT(reinitProxy.state() == QProcess::NotRunning, 8000);
        const auto pingOut = QJsonDocument::fromJson(reinitProxy.readAllStandardOutput().trimmed()).object();
        QCOMPARE(pingOut["id"].toInt(), 2);
        QVERIFY(pingOut["result"].isObject());
        QCOMPARE(reinitStep, 5);

        // 6. Verify --mcp-proxy does NOT replay a mutating write after stdin EOF during 404 session recovery.
        int eofStep = 0;
        bool sawReplayedWrite = false;
        QTcpServer eofServer;
        QVERIFY(eofServer.listen(QHostAddress::LocalHost, 0));
        connect(&eofServer, &QTcpServer::newConnection, &eofServer, [&eofServer, &eofStep, &sawReplayedWrite] {
            auto* s = eofServer.nextPendingConnection();
            connect(s, &QTcpSocket::readyRead, s, [s, &eofStep, &sawReplayedWrite] {
                const QByteArray raw = s->readAll();
                const int step = eofStep++;
                if (step == 0) {
                    const QByteArray body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"serverInfo\":{\"name\":\"t\",\"version\":\"1\"}}}";
                    s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nMcp-Session-Id: sid-a\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    s->disconnectFromHost();
                } else if (step == 1) {
                    const QByteArray body = "{\"jsonrpc\":\"2.0\",\"id\":2,\"error\":{\"code\":-32001,\"message\":\"Session expired\"}}";
                    s->write("HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    s->disconnectFromHost();
                } else if (step == 2) {
                    // Delay reinit slightly so stdin EOF arrives while reinit is in flight.
                    QTimer::singleShot(150, s, [s] {
                        if (s->state() != QAbstractSocket::ConnectedState) return;
                        const QByteArray body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"serverInfo\":{\"name\":\"t\",\"version\":\"1\"}}}";
                        s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nMcp-Session-Id: sid-b\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                        s->disconnectFromHost();
                    });
                } else {
                    if (raw.contains("project_action")) sawReplayedWrite = true;
                    s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
                    s->disconnectFromHost();
                }
            });
        });
        QProcess eofProxy;
        eofProxy.start(SAV_APP_EXECUTABLE, {"--mcp-proxy", QString("http://127.0.0.1:%1/mcp").arg(eofServer.serverPort())});
        QVERIFY(eofProxy.waitForStarted(5000));
        eofProxy.write("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"t\",\"version\":\"1\"}}}\n");
        QTRY_VERIFY_WITH_TIMEOUT(eofProxy.bytesAvailable() > 0, 5000);
        eofProxy.readAllStandardOutput();
        eofProxy.write("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"project_action\",\"arguments\":{\"action\":\"new_show\"}}}\n");
        eofProxy.closeWriteChannel();
        QTRY_VERIFY_WITH_TIMEOUT(eofProxy.state() == QProcess::NotRunning, 8000);
        QVERIFY(!sawReplayedWrite);

        // 7. Verify --mcp-proxy refuses further non-initialize requests when 404 reinitialization
        //    fails at notifications/initialized instead of falling back to sessionless requests.
        int failStep = 0;
        bool sawPostFailureCall = false;
        QTcpServer failServer;
        QVERIFY(failServer.listen(QHostAddress::LocalHost, 0));
        connect(&failServer, &QTcpServer::newConnection, &failServer, [&failServer, &failStep, &sawPostFailureCall] {
            auto* s = failServer.nextPendingConnection();
            connect(s, &QTcpSocket::readyRead, s, [s, &failStep, &sawPostFailureCall] {
                const QByteArray raw = s->readAll();
                const int step = failStep++;
                if (step == 0) {
                    const QByteArray body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"serverInfo\":{\"name\":\"t\",\"version\":\"1\"}}}";
                    s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nMcp-Session-Id: sid-x\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                } else if (step == 1) {
                    const QByteArray body = "{\"jsonrpc\":\"2.0\",\"id\":2,\"error\":{\"code\":-32001,\"message\":\"Session expired\"}}";
                    s->write("HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                } else if (step == 2) {
                    const QByteArray body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"serverInfo\":{\"name\":\"t\",\"version\":\"1\"}}}";
                    s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nMcp-Session-Id: sid-y\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                } else if (step == 3 && raw.contains("notifications/initialized")) {
                    s->write("HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                } else {
                    sawPostFailureCall = true;
                    s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
                }
                s->disconnectFromHost();
            });
        });
        QProcess failProxy;
        failProxy.start(SAV_APP_EXECUTABLE, {"--mcp-proxy", QString("http://127.0.0.1:%1/mcp").arg(failServer.serverPort())});
        QVERIFY(failProxy.waitForStarted(5000));
        failProxy.write("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"t\",\"version\":\"1\"}}}\n");
        QTRY_VERIFY_WITH_TIMEOUT(failProxy.bytesAvailable() > 0, 5000);
        failProxy.readAllStandardOutput();
        failProxy.write("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}\n");
        QTRY_VERIFY_WITH_TIMEOUT(failProxy.bytesAvailable() > 0, 5000);
        const auto err2 = QJsonDocument::fromJson(failProxy.readAllStandardOutput().trimmed()).object();
        QCOMPARE(err2["id"].toInt(), 2);
        QCOMPARE(err2["error"].toObject()["code"].toInt(), -32001);
        failProxy.write("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"project_action\",\"arguments\":{\"action\":\"new_show\"}}}\n");
        failProxy.closeWriteChannel();
        QTRY_VERIFY_WITH_TIMEOUT(failProxy.state() == QProcess::NotRunning, 8000);
        const auto err3 = QJsonDocument::fromJson(failProxy.readAllStandardOutput().trimmed()).object();
        QCOMPARE(err3["id"].toInt(), 3);
        QCOMPARE(err3["error"].toObject()["code"].toInt(), -32001);
        QVERIFY(!sawPostFailureCall);

        // 8. Verify --mcp-proxy emits JSON-RPC -32700 / -32600 with id: null on malformed stdio input,
        //    and serializes transport_control calls in strict FIFO order.
        QStringList controlOrder;
        QTcpServer orderServer;
        QVERIFY(orderServer.listen(QHostAddress::LocalHost, 0));
        connect(&orderServer, &QTcpServer::newConnection, &orderServer, [&orderServer, &controlOrder] {
            auto* s = orderServer.nextPendingConnection();
            connect(s, &QTcpSocket::readyRead, s, [s, &controlOrder] {
                const QByteArray raw = s->readAll();
                if (raw.contains("\"play\"")) {
                    controlOrder << "play-start";
                    QTimer::singleShot(120, s, [s, &controlOrder] {
                        if (s->state() != QAbstractSocket::ConnectedState) return;
                        controlOrder << "play-done";
                        const QByteArray body = "{\"jsonrpc\":\"2.0\",\"id\":10,\"result\":{}}";
                        s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                        s->disconnectFromHost();
                    });
                } else if (raw.contains("\"stop_all\"")) {
                    controlOrder << "stop_all";
                    const QByteArray body = "{\"jsonrpc\":\"2.0\",\"id\":11,\"result\":{}}";
                    s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    s->disconnectFromHost();
                } else {
                    const QByteArray body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"serverInfo\":{\"name\":\"t\",\"version\":\"1\"}}}";
                    s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nMcp-Session-Id: sid-ord\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    s->disconnectFromHost();
                }
            });
        });
        QProcess orderProxy;
        orderProxy.start(SAV_APP_EXECUTABLE, {"--mcp-proxy", QString("http://127.0.0.1:%1/mcp").arg(orderServer.serverPort())});
        QVERIFY(orderProxy.waitForStarted(5000));
        orderProxy.write("not valid json\n");
        orderProxy.write("{\"jsonrpc\":\"2.0\"}\n");
        orderProxy.write("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"t\",\"version\":\"1\"}}}\n");
        QByteArray ordBuf;
        auto readOrderOutput = [&] {
            ordBuf.append(orderProxy.readAllStandardOutput());
            return ordBuf;
        };
        QTRY_VERIFY_WITH_TIMEOUT(readOrderOutput().contains("\"id\":1"), 5000);
        orderProxy.write("{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\",\"params\":{\"name\":\"transport_control\",\"arguments\":{\"action\":\"play\"}}}\n");
        orderProxy.write("{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\",\"params\":{\"name\":\"transport_control\",\"arguments\":{\"action\":\"stop_all\"}}}\n");
        QTRY_VERIFY_WITH_TIMEOUT(readOrderOutput().contains("\"id\":11"), 5000);
        orderProxy.closeWriteChannel();
        QTRY_VERIFY_WITH_TIMEOUT(orderProxy.state() == QProcess::NotRunning, 8000);
        ordBuf.append(orderProxy.readAllStandardOutput());
        const auto ordLines = ordBuf.split('\n');
        bool sawParseErr = false, sawInvalidReq = false;
        for (const auto& l : ordLines) {
            if (l.trimmed().isEmpty()) continue;
            const auto o = QJsonDocument::fromJson(l).object();
            if (o["id"].isNull() && o["error"].toObject()["code"].toInt() == -32700) sawParseErr = true;
            if (o["id"].isNull() && o["error"].toObject()["code"].toInt() == -32600) sawInvalidReq = true;
        }
        QVERIFY(sawParseErr);
        QVERIFY(sawInvalidReq);
        QCOMPARE(controlOrder, QStringList({"play-start", "play-done", "stop_all"}));

        // 9. Verify --mcp-proxy rejects non-loopback URLs (token exfiltration guard), refuses redirects,
        //    rejects id:null requests, and forwards client JSON-RPC responses cleanly without echoing -32600.
        QProcess remoteProxy;
        remoteProxy.start(SAV_APP_EXECUTABLE, {"--mcp-proxy", "https://example.com/mcp"});
        QVERIFY(remoteProxy.waitForStarted(5000));
        QTRY_VERIFY_WITH_TIMEOUT(remoteProxy.state() == QProcess::NotRunning, 5000);
        QCOMPARE(remoteProxy.exitCode(), 2);

        bool sawRedirectTarget = false;
        QTcpServer redirectTarget;
        QVERIFY(redirectTarget.listen(QHostAddress::LocalHost, 0));
        connect(&redirectTarget, &QTcpServer::newConnection, &redirectTarget, [&redirectTarget, &sawRedirectTarget] {
            auto* s = redirectTarget.nextPendingConnection();
            sawRedirectTarget = true;
            s->disconnectFromHost();
        });
        QTcpServer redirectServer;
        QVERIFY(redirectServer.listen(QHostAddress::LocalHost, 0));
        connect(&redirectServer, &QTcpServer::newConnection, &redirectServer, [&redirectServer, &redirectTarget] {
            auto* s = redirectServer.nextPendingConnection();
            auto buf = std::make_shared<QByteArray>();
            connect(s, &QTcpSocket::readyRead, s, [s, buf, port = redirectTarget.serverPort()] {
                buf->append(s->readAll());
                const qsizetype sep = buf->indexOf("\r\n\r\n");
                if (sep < 0) return;
                const QByteArray loc = QString("http://127.0.0.1:%1/stolen").arg(port).toUtf8();
                s->write("HTTP/1.1 302 Found\r\nLocation: " + loc + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                s->disconnectFromHost();
            });
        });
        QProcess redirProxy;
        redirProxy.start(SAV_APP_EXECUTABLE, {"--mcp-proxy", QString("http://127.0.0.1:%1/mcp").arg(redirectServer.serverPort())});
        QVERIFY(redirProxy.waitForStarted(5000));
        redirProxy.write("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\n");
        redirProxy.closeWriteChannel();
        QTRY_VERIFY_WITH_TIMEOUT(redirProxy.state() == QProcess::NotRunning, 8000);
        QVERIFY(!sawRedirectTarget);

        bool sawClientResp = false;
        QTcpServer respServer;
        QVERIFY(respServer.listen(QHostAddress::LocalHost, 0));
        connect(&respServer, &QTcpServer::newConnection, &respServer, [&respServer, &sawClientResp] {
            auto* s = respServer.nextPendingConnection();
            auto buf = std::make_shared<QByteArray>();
            auto replied = std::make_shared<bool>(false);
            connect(s, &QTcpSocket::readyRead, s, [s, buf, replied, &sawClientResp] {
                if (*replied) return;
                buf->append(s->readAll());
                const qsizetype sep = buf->indexOf("\r\n\r\n");
                if (sep < 0) return;
                qint64 contentLength = 0;
                for (const auto& hdr : buf->left(sep).split('\n')) {
                    const qsizetype colon = hdr.indexOf(':');
                    if (colon > 0 && hdr.left(colon).trimmed().toLower() == "content-length")
                        contentLength = hdr.mid(colon + 1).trimmed().toLongLong();
                }
                if (buf->size() - sep - 4 < contentLength) return;
                *replied = true;
                const QByteArray payload = buf->mid(sep + 4, contentLength);
                if (payload.contains("\"result\":{}")) {
                    sawClientResp = true;
                    s->write("HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                } else {
                    const QByteArray body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"serverInfo\":{\"name\":\"t\",\"version\":\"1\"}}}";
                    s->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nMcp-Session-Id: sid-resp\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                }
                s->disconnectFromHost();
            });
        });
        QProcess respProxy;
        respProxy.start(SAV_APP_EXECUTABLE, {"--mcp-proxy", QString("http://127.0.0.1:%1/mcp").arg(respServer.serverPort())});
        QVERIFY(respProxy.waitForStarted(5000));
        respProxy.write("{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"ping\"}\n");
        respProxy.write("{\"jsonrpc\":\"2.0\",\"id\":42,\"result\":{}}\n");
        respProxy.closeWriteChannel();
        QTRY_VERIFY_WITH_TIMEOUT(respProxy.state() == QProcess::NotRunning, 8000);
        QVERIFY(sawClientResp);
        const auto respLines = respProxy.readAllStandardOutput().split('\n');
        int errorCount = 0;
        bool sawNullIdRejected = false;
        for (const auto& l : respLines) {
            if (l.trimmed().isEmpty()) continue;
            const auto o = QJsonDocument::fromJson(l).object();
            if (o.contains("error")) ++errorCount;
            if (o["id"].isNull() && o["error"].toObject()["code"].toInt() == -32600) sawNullIdRejected = true;
        }
        QVERIFY(sawNullIdRejected);
        QCOMPARE(errorCount, 1); // Client response (id:42) must NOT produce a -32600 error.
    }

    void acpPermissionAndCancel() {
        AcpClient client;
        AcpPermissionRequest receivedPerm;
        QString message, finishedReason;
        connect(&client, &AcpClient::permissionRequested, [&](const AcpPermissionRequest& req) { receivedPerm = req; });
        connect(&client, &AcpClient::messageChunk, [&](const QString& m) { message += m; });
        connect(&client, &AcpClient::promptFinished, [&](const QString& r) { finishedReason = r; });

        client.connectToAgent(SAV_MOCK_ACP_AGENT, {"--http-mcp", "--require-permission"}, QDir::tempPath(), {});
        QTRY_COMPARE(client.state(), AcpState::Ready);
        client.sendPrompt("Need approval");
        QTRY_VERIFY(receivedPerm.requestKey > 0);
        QCOMPARE(receivedPerm.title, QString("<b>Apply edits to show</b>"));
        QCOMPARE(receivedPerm.options.size(), size_t(2));
        client.respondPermission(receivedPerm.requestKey, "allow-once");
        QTRY_COMPARE(finishedReason, QString("end_turn"));
        QVERIFY(message.contains("perm=selected:allow-once"));
        client.disconnectFromAgent();

        // Incompatible / missing protocolVersion must fail before session/new
        {
            AcpClient bad;
            bad.connectToAgent(SAV_MOCK_ACP_AGENT, {"--bad-protocol"}, QDir::tempPath(), {});
            QTRY_COMPARE(bad.state(), AcpState::Error);
            QVERIFY(bad.sessionId().isEmpty());
            bad.disconnectFromAgent();

            AcpClient missing;
            missing.connectToAgent(SAV_MOCK_ACP_AGENT, {"--missing-protocol"}, QDir::tempPath(), {});
            QTRY_COMPARE(missing.state(), AcpState::Error);
            QVERIFY(missing.sessionId().isEmpty());
            missing.disconnectFromAgent();
        }

        // Switching sessions during an active prompt must ignore stale session output/completion
        {
            AcpClient client2;
            QString msg2, finished2;
            connect(&client2, &AcpClient::messageChunk, [&](const QString& m) { msg2 += m; });
            connect(&client2, &AcpClient::promptFinished, [&](const QString& r) { finished2 = r; });
            client2.connectToAgent(SAV_MOCK_ACP_AGENT, {"--http-mcp", "--hold-prompt", "--stale-on-new-session"}, QDir::tempPath(), {});
            QTRY_COMPARE(client2.state(), AcpState::Ready);
            QCOMPARE(client2.sessionId(), QString("mock-session-1"));
            client2.sendPrompt("Old prompt");
            QCOMPARE(client2.state(), AcpState::Prompting);
            client2.startNewSession(QDir::tempPath(), {});
            QTRY_COMPARE(client2.sessionId(), QString("mock-session-2"));
            QTRY_COMPARE(client2.state(), AcpState::Ready);
            QVERIFY(!msg2.contains("STALE_SESSION_OUTPUT"));
            QVERIFY(finished2.isEmpty());
            client2.sendPrompt("Fresh prompt");
            QTRY_COMPARE(finished2, QString("end_turn"));
            QVERIFY(msg2.contains("Mock reply: ok"));
            QVERIFY(msg2.contains("closed=mock-session-1"));
            QVERIFY(!msg2.contains("STALE_SESSION_OUTPUT"));
            client2.disconnectFromAgent();

            // Agents that do NOT advertise sessionCapabilities.close must restart cleanly on startNewSession.
            AcpClient noClose;
            QString msgA, msgB, fin;
            int phase = 0;
            connect(&noClose, &AcpClient::messageChunk, [&](const QString& m) { (phase == 0 ? msgA : msgB) += m; });
            connect(&noClose, &AcpClient::promptFinished, [&](const QString& r) { fin = r; });
            noClose.connectToAgent(SAV_MOCK_ACP_AGENT, {"--http-mcp", "--no-session-close"}, QDir::tempPath(), {});
            QTRY_COMPARE(noClose.state(), AcpState::Ready);
            noClose.sendPrompt("First");
            QTRY_COMPARE(fin, QString("end_turn"));
            phase = 1;
            fin.clear();
            noClose.startNewSession(QDir::tempPath(), {});
            QTRY_COMPARE(noClose.state(), AcpState::Ready);
            noClose.sendPrompt("Second");
            QTRY_COMPARE(fin, QString("end_turn"));
            auto extractPid = [](const QString& s) {
                const int p = s.indexOf("pid=");
                const int end = p >= 0 ? s.indexOf(' ', p) : -1;
                return p >= 0 ? s.mid(p + 4, end > p ? end - (p + 4) : -1) : QString();
            };
            QVERIFY(!extractPid(msgA).isEmpty());
            QVERIFY(!extractPid(msgB).isEmpty());
            QVERIFY(extractPid(msgA) != extractPid(msgB));
            noClose.disconnectFromAgent();
        }

        // Cancellation test
        finishedReason.clear();
        client.connectToAgent(SAV_MOCK_ACP_AGENT, {"--http-mcp", "--hold-prompt"}, QDir::tempPath(), {});
        QTRY_COMPARE(client.state(), AcpState::Ready);
        client.sendPrompt("Wait forever");
        QCOMPARE(client.state(), AcpState::Prompting);
        client.cancelPrompt();
        QTRY_COMPARE(finishedReason, QString("cancelled"));
        QCOMPARE(client.state(), AcpState::Ready);
        client.disconnectFromAgent();

        // Silent handshake and ignored-cancel deadlines must fail cleanly instead of hanging forever.
        {
            AcpClient silent;
            silent.setTimeoutsForTesting(1000, 150, 150);
            silent.connectToAgent(SAV_MOCK_ACP_AGENT, {"--silent-handshake"}, QDir::tempPath(), {});
            QTRY_COMPARE_WITH_TIMEOUT(silent.state(), AcpState::Error, 3000);
            QVERIFY(silent.lastError().contains("Timed out"));
            silent.disconnectFromAgent();

            AcpClient stuckCancel;
            stuckCancel.setTimeoutsForTesting(1000, 2000, 150);
            stuckCancel.connectToAgent(SAV_MOCK_ACP_AGENT, {"--http-mcp", "--hold-prompt", "--ignore-cancel"}, QDir::tempPath(), {});
            QTRY_COMPARE(stuckCancel.state(), AcpState::Ready);
            stuckCancel.sendPrompt("Will ignore cancel");
            QCOMPARE(stuckCancel.state(), AcpState::Prompting);
            stuckCancel.cancelPrompt();
            QTRY_COMPARE_WITH_TIMEOUT(stuckCancel.state(), AcpState::Error, 3000);
            QVERIFY(stuckCancel.lastError().contains("cancel"));
            stuckCancel.disconnectFromAgent();

            // Starting a new session while an old prompt ignores cancellation must NOT bypass the cancel deadline.
            AcpClient stuckNewSession;
            stuckNewSession.setTimeoutsForTesting(1000, 2000, 150);
            stuckNewSession.connectToAgent(SAV_MOCK_ACP_AGENT, {"--http-mcp", "--hold-prompt", "--ignore-cancel"}, QDir::tempPath(), {});
            QTRY_COMPARE(stuckNewSession.state(), AcpState::Ready);
            stuckNewSession.sendPrompt("Old prompt ignoring cancel");
            QCOMPARE(stuckNewSession.state(), AcpState::Prompting);
            stuckNewSession.startNewSession(QDir::tempPath(), {});
            QTRY_COMPARE_WITH_TIMEOUT(stuckNewSession.state(), AcpState::Error, 3000);
            QVERIFY(stuckNewSession.lastError().contains("cancel"));
            stuckNewSession.disconnectFromAgent();
        }
    }

    void embeddedChatPanelAndShortcutSafety() {
        MainWindow window;
        window.controller().replace(demoProject());
        window.show();

        auto* chatButton = window.findChild<QPushButton*>("agentChatButton");
        auto* panel = window.findChild<AgentChatPanel*>("agentChatPanel");
        QVERIFY(chatButton);
        QVERIFY(panel);
        QVERIFY(!panel->isVisible());

        // Toggle open
        chatButton->click();
        QVERIFY(panel->isVisible());
        QVERIFY(chatButton->isChecked());

        // Typing Space and Backspace inside the prompt box must never play or delete clips.
        auto* promptInput = panel->findChild<QPlainTextEdit*>("agentPromptInput");
        QVERIFY(promptInput);
        promptInput->setEnabled(true);
        promptInput->setFocus();
        const auto clipsBefore = window.controller().project.show.clips.size();
        QTest::keyClick(promptInput, Qt::Key_A);
        QTest::keyClick(promptInput, Qt::Key_Space);
        QTest::keyClick(promptInput, Qt::Key_B);
        QTest::keyClick(promptInput, Qt::Key_Backspace);
        QCOMPARE(window.controller().transport.state(), State::Stopped);
        QCOMPARE(window.controller().project.show.clips.size(), clipsBefore);
        QCOMPARE(promptInput->toPlainText(), QString("a "));
        promptInput->clear();

        // Configure custom ACP command to our mock agent with permission prompt
        auto* presetCombo = panel->findChild<QComboBox*>("agentPresetCombo");
        auto* customCommand = panel->findChild<QLineEdit*>("agentCustomCommand");
        auto* connectButton = panel->findChild<QPushButton*>("agentConnectButton");
        auto* allowEditing = panel->findChild<QCheckBox*>("agentChatAllowEditing");
        auto* sendButton = panel->findChild<QPushButton*>("agentSendButton");
        auto* transcript = panel->findChild<QTextBrowser*>("agentChatTranscript");
        auto* permissionBox = panel->findChild<QWidget*>("agentPermissionBox");
        QVERIFY(presetCombo && customCommand && connectButton && allowEditing && sendButton && transcript && permissionBox);

        const int customIdx = presetCombo->findData("custom");
        QVERIFY(customIdx >= 0);
        presetCombo->setCurrentIndex(customIdx);
        customCommand->setText(QString("\"%1\" --http-mcp --require-permission --long-permission").arg(SAV_MOCK_ACP_AGENT));

        QVERIFY(!window.mcpServer().running());
        connectButton->click();
        QTRY_COMPARE(panel->acpClient().state(), AcpState::Ready);
        QVERIFY(window.mcpServer().running());
        QVERIFY(!window.mcpServer().editingEnabled());

        // Allow edits toggle syncs with McpServer and locks in Show mode
        allowEditing->setChecked(true);
        QVERIFY(window.mcpServer().editingEnabled());
        window.controller().setShowMode(true);
        QVERIFY(!allowEditing->isEnabled());
        window.controller().setShowMode(false);
        QVERIFY(allowEditing->isEnabled());

        // Revoking write access externally must NOT be silently restored on reconnect
        window.mcpServer().setEditingEnabled(false);
        QVERIFY(!allowEditing->isChecked());
        connectButton->click(); // Disconnect (stops auto-started MCP)
        QTRY_COMPARE(panel->acpClient().state(), AcpState::Disconnected);
        connectButton->click(); // Reconnect
        QTRY_COMPARE(panel->acpClient().state(), AcpState::Ready);
        QVERIFY(!window.mcpServer().editingEnabled());
        QVERIFY(!allowEditing->isChecked());

        // Send prompt and verify permission card shows full untruncated plain-text request
        promptInput->setPlainText("Inspect the show");
        sendButton->click();
        QTRY_VERIFY(permissionBox->isVisible());
        auto* detailView = permissionBox->findChild<QPlainTextEdit*>("permissionDetailText");
        QVERIFY(detailView);
        QVERIFY(detailView->toPlainText().contains("TAIL_MARKER_END"));
        QPushButton* allowBtn = nullptr;
        for (auto* btn : permissionBox->findChildren<QPushButton*>())
            if (btn->property("optionId").toString() == "allow-once") { allowBtn = btn; break; }
        QVERIFY(allowBtn);
        allowBtn->click();
        QTRY_COMPARE(panel->acpClient().state(), AcpState::Ready);
        QTRY_VERIFY(transcript->toPlainText().contains("perm=selected:allow-once"));

        // Permission cards must clear when MCP stops externally or the agent exits cleanly.
        promptInput->setPlainText("Trigger permission then external MCP stop");
        sendButton->click();
        QTRY_VERIFY(permissionBox->isVisible());
        window.mcpServer().stop();
        QTRY_COMPARE(panel->acpClient().state(), AcpState::Disconnected);
        QVERIFY(!permissionBox->isVisible());

        customCommand->setText(QString("\"%1\" --http-mcp --require-permission --exit-clean-after-permission").arg(SAV_MOCK_ACP_AGENT));
        connectButton->click();
        QTRY_COMPARE(panel->acpClient().state(), AcpState::Ready);
        promptInput->setPlainText("Trigger permission then clean exit");
        sendButton->click();
        QTRY_COMPARE(panel->acpClient().state(), AcpState::Disconnected);
        QVERIFY(!permissionBox->isVisible());
        QVERIFY(!window.mcpServer().running());

        // Unexpected protocol error on a chat-owned connection must also stop the auto-started MCP server.
        customCommand->setText(QString("\"%1\" --bad-protocol").arg(SAV_MOCK_ACP_AGENT));
        connectButton->click();
        QTRY_COMPARE(panel->acpClient().state(), AcpState::Error);
        QVERIFY(!window.mcpServer().running());

        // Connecting to an already-running external MCP server keeps the external server alive on Disconnect
        // while keeping Undo hidden whenever undoCount() == 0 (e.g. after show replacement).
        QVERIFY(window.mcpServer().start(0));
        window.mcpServer().setEditingEnabled(true);
        customCommand->setText(QString("\"%1\" --http-mcp").arg(SAV_MOCK_ACP_AGENT));
        connectButton->click();
        QTRY_COMPARE(panel->acpClient().state(), AcpState::Ready);
        QVERIFY(allowEditing->isChecked());
        auto* undoButton = panel->findChild<QPushButton*>("agentChatUndoButton");
        QVERIFY(undoButton);
        // Simulate a show-replacing edit that clears undo history while emitting showModified.
        window.controller().replace(newProject());
        QCOMPARE(window.controller().undoCount(), std::size_t(0));
        QVERIFY(!undoButton->isVisible());
        connectButton->click(); // Disconnect chat
        QTRY_COMPARE(panel->acpClient().state(), AcpState::Disconnected);
        QVERIFY(window.mcpServer().running());
        window.mcpServer().stop();

        // If chat auto-starts the MCP server and the operator later shares it externally,
        // disconnecting chat must keep the listener running for external clients.
        QVERIFY(!window.mcpServer().running());
        customCommand->setText(QString("\"%1\" --http-mcp").arg(SAV_MOCK_ACP_AGENT));
        connectButton->click();
        QTRY_COMPARE(panel->acpClient().state(), AcpState::Ready);
        QVERIFY(window.mcpServer().running());
        QVERIFY(!window.mcpServer().externallyShared());
        window.mcpServer().markExternallyShared();
        connectButton->click(); // Disconnect chat
        QTRY_COMPARE(panel->acpClient().state(), AcpState::Disconnected);
        QVERIFY(window.mcpServer().running());
        window.mcpServer().stop();

        // Space on a focused chat control button must activate the button, NOT trigger playback,
        // and ignored Backspace/Delete on a focused chat button must NOT delete a selected timeline clip.
        window.controller().replace(demoProject());
        auto* timeline = window.findChild<Timeline*>();
        QVERIFY(timeline);
        QVERIFY(!window.controller().project.show.clips.empty());
        timeline->selectedClip = QString::fromStdString(window.controller().project.show.clips.front().id);
        const auto clipsBeforeButtonKeys = window.controller().project.show.clips.size();
        connectButton->setFocus();
        QTest::keyClick(connectButton, Qt::Key_Backspace);
        QTest::keyClick(connectButton, Qt::Key_Delete);
        QCOMPARE(window.controller().project.show.clips.size(), clipsBeforeButtonKeys);
        QTest::keyClick(connectButton, Qt::Key_Space);
        QTRY_COMPARE(panel->acpClient().state(), AcpState::Ready);
        QCOMPARE(window.controller().transport.state(), State::Stopped);
        connectButton->setFocus();
        QTest::keyClick(connectButton, Qt::Key_Backspace);
        QCOMPARE(window.controller().project.show.clips.size(), clipsBeforeButtonKeys);
        QTest::keyClick(connectButton, Qt::Key_Space);
        QTRY_COMPARE(panel->acpClient().state(), AcpState::Disconnected);
        QCOMPARE(window.controller().transport.state(), State::Stopped);

        // Clicking a chat toolbar button (e.g. Logs) while a timeline clip is selected must move
        // focus into chat so Space/Delete do not start playback or delete the selected clip.
        auto* logsToggle = panel->findChild<QToolButton*>("agentLogsToggle");
        QVERIFY(logsToggle);
        timeline->setFocus();
        QTest::mouseClick(logsToggle, Qt::LeftButton);
        QVERIFY(panel->isAncestorOf(QApplication::focusWidget()));
        window.controller().status.clear();
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Delete);
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Space);
        QCOMPARE(window.controller().project.show.clips.size(), clipsBeforeButtonKeys);
        QCOMPARE(window.controller().transport.state(), State::Stopped);

        // Space and Backspace inside the Agent Chat drawer (including read-only transcript/logs)
        // must NOT trigger live playback or delete selected timeline clips.
        const auto clipsBeforeTranscript = window.controller().project.show.clips.size();
        transcript->setFocus();
        window.controller().status.clear();
        QTest::keyClick(transcript, Qt::Key_Space);
        QTest::keyClick(transcript, Qt::Key_Backspace);
        QCOMPARE(window.controller().transport.state(), State::Stopped);
        QCOMPARE(window.controller().project.show.clips.size(), clipsBeforeTranscript);
        QVERIFY(window.controller().status.isEmpty());
        // Returning focus to the main editor outside chat restores Space for transport toggle.
        window.setFocus();
        QTest::keyClick(&window, Qt::Key_Space);
        QVERIFY(!window.controller().status.isEmpty() || window.controller().transport.state() == State::Playing);
        window.controller().stopAll();
        window.controller().dirty = false;
    }

    void destroyingConnectedWindowIsSafe() {
        auto* window = new MainWindow;
        window->show();
        auto* chatButton = window->findChild<QPushButton*>("agentChatButton");
        auto* panel = window->findChild<AgentChatPanel*>("agentChatPanel");
        QVERIFY(chatButton && panel);
        chatButton->click();
        auto* presetCombo = panel->findChild<QComboBox*>("agentPresetCombo");
        auto* customCommand = panel->findChild<QLineEdit*>("agentCustomCommand");
        auto* connectButton = panel->findChild<QPushButton*>("agentConnectButton");
        presetCombo->setCurrentIndex(presetCombo->findData("custom"));
        customCommand->setText(QString("\"%1\" --http-mcp").arg(SAV_MOCK_ACP_AGENT));
        connectButton->click();
        QTRY_COMPARE(panel->acpClient().state(), AcpState::Ready);
        QVERIFY(window->mcpServer().running());
        // Direct deletion skips closeEvent; destructor must shut down panel before members die.
        delete window;
    }

    void saveAfterRelinkPreservesUndoHistory() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString replacementPath = dir.filePath("replacement.wav");
        // Write a tiny valid mono 48kHz PCM WAV file.
        {
            QFile f(replacementPath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            const quint32 dataBytes = 48000 * 2;
            const quint32 riffSize = 36 + dataBytes;
            QByteArray wav;
            wav.append("RIFF", 4);
            wav.append(reinterpret_cast<const char*>(&riffSize), 4);
            wav.append("WAVEfmt ", 8);
            const quint32 fmtSize = 16; const quint16 audioFmt = 1, channels = 1;
            const quint32 sampleRate = 48000, byteRate = 96000;
            const quint16 blockAlign = 2, bitsPerSample = 16;
            wav.append(reinterpret_cast<const char*>(&fmtSize), 4);
            wav.append(reinterpret_cast<const char*>(&audioFmt), 2);
            wav.append(reinterpret_cast<const char*>(&channels), 2);
            wav.append(reinterpret_cast<const char*>(&sampleRate), 4);
            wav.append(reinterpret_cast<const char*>(&byteRate), 4);
            wav.append(reinterpret_cast<const char*>(&blockAlign), 2);
            wav.append(reinterpret_cast<const char*>(&bitsPerSample), 2);
            wav.append("data", 4);
            wav.append(reinterpret_cast<const char*>(&dataBytes), 4);
            wav.append(QByteArray(static_cast<qsizetype>(dataBytes), '\0'));
            QCOMPARE(f.write(wav), wav.size());
        }

        Controller c;
        Project proj = newProject();
        MediaAsset offlineAsset;
        offlineAsset.id = "asset-relink-1";
        offlineAsset.name = "missing.wav";
        offlineAsset.path = dir.filePath("missing/missing.wav");
        offlineAsset.video = false;
        offlineAsset.offline = true;
        offlineAsset.duration = 1.0;
        proj.assets.insert(offlineAsset.id, offlineAsset);
        Clip clip;
        clip.id = "clip-relink-1";
        clip.track = 2;
        clip.assetId = offlineAsset.id.toStdString();
        clip.start = 0;
        clip.duration = frames(1.0);
        proj.show.clips.push_back(clip);
        c.replace(proj);

        // Relink the offline asset with the replacement WAV, retaining the same asset ID.
        MediaAsset relinked = loadMedia(replacementPath);
        relinked.id = offlineAsset.id;
        QVERIFY(c.edit([&](Project& p) { p.assets.insert(relinked.id, relinked); }));
        QVERIFY(!c.project.assets[offlineAsset.id].offline);
        QCOMPARE(c.project.assets[offlineAsset.id].path, replacementPath);

        // Saving after relink must NOT overwrite the offline asset's historical path/audio in Undo history.
        QVERIFY(c.save(dir.filePath("relinked.savshow")));
        c.undo();
        QVERIFY(c.project.assets[offlineAsset.id].offline);
        QCOMPARE(c.project.assets[offlineAsset.id].path, offlineAsset.path);
        QVERIFY(!c.project.assets[offlineAsset.id].audio);

        // Saving TWICE after deleting a package-extracted asset must preserve the Undo snapshot's packageDirectory.
        Project pkgSrc = newProject();
        MediaAsset pkgAsset = loadMedia(replacementPath);
        pkgAsset.id = "pkg-asset-1";
        pkgSrc.assets.insert(pkgAsset.id, pkgAsset);
        Clip pkgClip;
        pkgClip.id = "pkg-clip-1";
        pkgClip.track = 2;
        pkgClip.assetId = pkgAsset.id.toStdString();
        pkgClip.start = 0;
        pkgClip.duration = frames(1.0);
        pkgSrc.show.clips.push_back(pkgClip);
        bindAudio(pkgSrc);
        const QString pkgPath = dir.filePath("double_save.savpack");
        writeProjectPackage(pkgSrc, pkgPath);

        Controller c2;
        c2.replace(readProject(pkgPath));
        QVERIFY(c2.project.packageDirectory != nullptr);
        QVERIFY(c2.edit([&](Project& p) {
            p.show.clips.clear();
            p.assets.remove(pkgAsset.id);
        }));
        const QString savedShow = dir.filePath("double_save.savshow");
        QVERIFY(c2.save(savedShow));
        QVERIFY(c2.save(savedShow)); // Second save must not delete the Undo snapshot's extraction dir.
        c2.undo();
        QVERIFY(c2.project.assets.contains(pkgAsset.id));
        QVERIFY(QFileInfo::exists(c2.project.assets[pkgAsset.id].path));
    }

    void permissionMergesCachedToolCallDetails() {
        AcpClient client;
        AcpPermissionRequest received;
        bool gotPermission = false;
        QObject::connect(&client, &AcpClient::permissionRequested, &client, [&](const AcpPermissionRequest& req) {
            received = req;
            gotPermission = true;
            client.respondPermission(req.requestKey, "allow-once");
        });
        QString stopReason;
        QObject::connect(&client, &AcpClient::promptFinished, &client, [&](const QString& reason) { stopReason = reason; });
        client.connectToAgent(QString::fromUtf8(SAV_MOCK_ACP_AGENT), {"--http-mcp", "--require-permission", "--id-only-permission"},
            QDir::tempPath(), {});
        QTRY_COMPARE(client.state(), AcpState::Ready);
        client.sendPrompt("test id-only permission");
        QTRY_VERIFY(gotPermission);
        QCOMPARE(received.toolCallId, QString("tc-perm"));
        QCOMPARE(received.title, QString("Apply edits via cached tool_call"));
        QCOMPARE(received.kind, QString("edit"));
        QVERIFY(received.detail.contains("apply_edits"));
        QTRY_COMPARE(stopReason, QString("end_turn"));
        client.disconnectFromAgent();
    }
};

QTEST_MAIN(AcpTests)
#include "acp_tests.moc"
