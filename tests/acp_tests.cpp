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
        // Windows paths with backslashes must stay intact both quoted and unquoted.
        QCOMPARE(splitCommandLine("\"C:\\Program Files\\Agent\\agent.exe\" --cwd C:\\Shows\\Act1"),
                 QStringList({"C:\\Program Files\\Agent\\agent.exe", "--cwd", "C:\\Shows\\Act1"}));
        QCOMPARE(splitCommandLine("agent --msg \"say \\\"hi\\\"\""),
                 QStringList({"agent", "--msg", "say \"hi\""}));
        QCOMPARE(splitCommandLine("  "), QStringList());
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
        QVERIFY(message.contains("\"type\":\"stdio\""));
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

        // Disconnect stops auto-started MCP server cleanly
        connectButton->click();
        QTRY_COMPARE(panel->acpClient().state(), AcpState::Disconnected);
        QVERIFY(!window.mcpServer().running());
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
