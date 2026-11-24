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
            QVERIFY(!msg2.contains("STALE_SESSION_OUTPUT"));
            client2.disconnectFromAgent();
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

        // Read-only chat transcript must NOT intercept Space (Space should reach Controller::toggle).
        transcript->setFocus();
        window.controller().status.clear();
        QTest::keyClick(transcript, Qt::Key_Space);
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
};

QTEST_MAIN(AcpTests)
#include "acp_tests.moc"
