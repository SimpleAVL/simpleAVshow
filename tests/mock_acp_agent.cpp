#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <iostream>
#include <string>

namespace {
void send(const QJsonObject& obj) {
    const QByteArray bytes = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    std::cout.write(bytes.constData(), static_cast<std::streamsize>(bytes.size()));
    std::cout.put('\n');
    std::cout.flush();
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    const bool httpMcp = args.contains("--http-mcp");
    const bool requirePermission = args.contains("--require-permission");
    const bool holdPrompt = args.contains("--hold-prompt");
    const bool badProtocol = args.contains("--bad-protocol");
    const bool missingProtocol = args.contains("--missing-protocol");
    const bool staleOnNewSession = args.contains("--stale-on-new-session");
    const bool longPermission = args.contains("--long-permission");
    const bool silentHandshake = args.contains("--silent-handshake");
    const bool ignoreCancel = args.contains("--ignore-cancel");
    const bool exitCleanAfterPermission = args.contains("--exit-clean-after-permission");
    const bool noSessionClose = args.contains("--no-session-close");
    const bool idOnlyPermission = args.contains("--id-only-permission");
    const bool summaryOnlyPermission = args.contains("--summary-only-permission");
    const bool diffPermission = args.contains("--diff-permission");
    const bool burstAndExit = args.contains("--burst-and-exit");

    int sessionCounter = 0;
    QString sessionId = "mock-session-1";
    QString lastClosedSession;
    QJsonArray lastMcpServers;
    bool waitingPermission = false;
    bool waitingCancel = false;
    QJsonValue pendingPromptId;
    QString pendingPromptSession;
    QString pendingPromptText;

    auto finishPrompt = [&](const QString& note) {
        send({{"jsonrpc", "2.0"}, {"method", "session/update"}, {"params", QJsonObject{
            {"sessionId", sessionId},
            {"update", QJsonObject{
                {"sessionUpdate", "agent_thought_chunk"},
                {"content", QJsonObject{{"type", "text"}, {"text", "Inspecting show..."}}}
            }}
        }}});
        send({{"jsonrpc", "2.0"}, {"method", "session/update"}, {"params", QJsonObject{
            {"sessionId", sessionId},
            {"update", QJsonObject{
                {"sessionUpdate", "tool_call"},
                {"toolCallId", "tc-1"},
                {"title", "get_show"},
                {"kind", "read"},
                {"status", "in_progress"}
            }}
        }}});
        const QString mcpSummary = QString::fromUtf8(QJsonDocument(lastMcpServers).toJson(QJsonDocument::Compact));
        send({{"jsonrpc", "2.0"}, {"method", "session/update"}, {"params", QJsonObject{
            {"sessionId", sessionId},
            {"update", QJsonObject{
                {"sessionUpdate", "tool_call_update"},
                {"toolCallId", "tc-1"},
                {"status", "completed"},
                {"rawOutput", QJsonObject{{"mcpServers", lastMcpServers}}}
            }}
        }}});
        send({{"jsonrpc", "2.0"}, {"method", "session/update"}, {"params", QJsonObject{
            {"sessionId", sessionId},
            {"update", QJsonObject{
                {"sessionUpdate", "agent_message_chunk"},
                {"content", QJsonObject{{"type", "text"}, {"text", "Mock reply: "}}}
            }}
        }}});
        send({{"jsonrpc", "2.0"}, {"method", "session/update"}, {"params", QJsonObject{
            {"sessionId", sessionId},
            {"update", QJsonObject{
                {"sessionUpdate", "agent_message_chunk"},
                {"content", QJsonObject{{"type", "text"}, {"text", note + " | pid=" + QString::number(QCoreApplication::applicationPid()) + " | closed=" + lastClosedSession + " | mcp=" + mcpSummary}}}
            }}
        }}});
        send({{"jsonrpc", "2.0"}, {"id", pendingPromptId}, {"result", QJsonObject{{"stopReason", "end_turn"}}}});
        pendingPromptId = QJsonValue();
        pendingPromptText.clear();
    };

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto doc = QJsonDocument::fromJson(QByteArray(line.data(), static_cast<qsizetype>(line.size())));
        if (!doc.isObject()) continue;
        const auto msg = doc.object();
        const QString method = msg["method"].toString();
        const QJsonValue id = msg["id"];
        const QJsonObject params = msg["params"].toObject();

        // Response to our session/request_permission
        if (method.isEmpty() && waitingPermission && id.toString() == "perm-1") {
            waitingPermission = false;
            const auto outcome = msg["result"].toObject()["outcome"].toObject();
            const QString kind = outcome["outcome"].toString();
            const QString opt = outcome["optionId"].toString();
            finishPrompt(QString("perm=%1:%2").arg(kind, opt));
            continue;
        }

        if (method == "initialize") {
            std::cerr << "mock-acp-agent initialized\n" << std::flush;
            if (silentHandshake) continue;
            QJsonObject agentCaps{
                {"mcpCapabilities", QJsonObject{{"http", httpMcp}, {"sse", false}}}
            };
            if (!noSessionClose)
                agentCaps.insert("sessionCapabilities", QJsonObject{{"close", QJsonObject{}}});
            QJsonObject initResult{
                {"agentInfo", QJsonObject{{"name", "MockACP"}, {"version", "1.0"}}},
                {"agentCapabilities", agentCaps}
            };
            if (badProtocol) initResult.insert("protocolVersion", 99);
            else if (!missingProtocol) initResult.insert("protocolVersion", 1);
            send({{"jsonrpc", "2.0"}, {"id", id}, {"result", initResult}});
        } else if (method == "session/close") {
            lastClosedSession = params["sessionId"].toString();
            send({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{}}});
        } else if (method == "session/new") {
            lastMcpServers = params["mcpServers"].toArray();
            const QString previousSession = pendingPromptSession;
            const QJsonValue stalePromptId = pendingPromptId;
            sessionId = QString("mock-session-%1").arg(++sessionCounter);
            send({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{{"sessionId", sessionId}}}});
            if (staleOnNewSession && !stalePromptId.isUndefined() && !stalePromptId.isNull()) {
                // Emit stale update and stale prompt completion for the superseded session.
                send({{"jsonrpc", "2.0"}, {"method", "session/update"}, {"params", QJsonObject{
                    {"sessionId", previousSession},
                    {"update", QJsonObject{
                        {"sessionUpdate", "agent_message_chunk"},
                        {"content", QJsonObject{{"type", "text"}, {"text", "STALE_SESSION_OUTPUT"}}}
                    }}
                }}});
                send({{"jsonrpc", "2.0"}, {"id", stalePromptId}, {"result", QJsonObject{{"stopReason", "cancelled"}}}});
                pendingPromptId = QJsonValue();
                pendingPromptSession.clear();
                waitingCancel = false;
            }
        } else if (method == "session/prompt") {
            pendingPromptId = id;
            pendingPromptSession = params["sessionId"].toString(sessionId);
            const auto blocks = params["prompt"].toArray();
            pendingPromptText = blocks.isEmpty() ? QString() : blocks.first().toObject()["text"].toString();
            if (requirePermission) {
                waitingPermission = true;
                QJsonObject rawInput{{"action", "apply_edits"}};
                if (longPermission) rawInput.insert("payload", QString(320, 'X') + "TAIL_MARKER_END");
                if (idOnlyPermission || summaryOnlyPermission) {
                    send({{"jsonrpc", "2.0"}, {"method", "session/update"}, {"params", QJsonObject{
                        {"sessionId", pendingPromptSession},
                        {"update", QJsonObject{
                            {"sessionUpdate", "tool_call"},
                            {"toolCallId", "tc-perm"},
                            {"title", "Apply edits via cached tool_call"},
                            {"kind", "edit"},
                            {"status", "pending"},
                            {"rawInput", rawInput}
                        }}
                    }}});
                    QJsonObject tc{{"toolCallId", "tc-perm"}};
                    if (summaryOnlyPermission)
                        tc.insert("content", QJsonObject{{"type", "text"}, {"text", "Short summary"}});
                    send({{"jsonrpc", "2.0"}, {"id", "perm-1"}, {"method", "session/request_permission"}, {"params", QJsonObject{
                        {"sessionId", pendingPromptSession},
                        {"toolCall", tc},
                        {"options", QJsonArray{
                            QJsonObject{{"optionId", "allow-once"}, {"name", "Allow once"}, {"kind", "allow_once"}},
                            QJsonObject{{"optionId", "reject-once"}, {"name", "Reject"}, {"kind", "reject_once"}}
                        }}
                    }}});
                } else if (diffPermission) {
                    send({{"jsonrpc", "2.0"}, {"id", "perm-1"}, {"method", "session/request_permission"}, {"params", QJsonObject{
                        {"sessionId", pendingPromptSession},
                        {"toolCall", QJsonObject{
                            {"toolCallId", "tc-diff"},
                            {"title", "Edit show script"},
                            {"kind", "edit"},
                            {"content", QJsonArray{QJsonObject{
                                {"type", "diff"},
                                {"path", "/tmp/show.txt"},
                                {"oldText", "old cue"},
                                {"newText", "new cue"}
                            }}}
                        }},
                        {"options", QJsonArray{
                            QJsonObject{{"optionId", "allow-once"}, {"name", "Allow once"}, {"kind", "allow_once"}},
                            QJsonObject{{"optionId", "reject-once"}, {"name", "Reject"}, {"kind", "reject_once"}}
                        }}
                    }}});
                } else {
                    send({{"jsonrpc", "2.0"}, {"id", "perm-1"}, {"method", "session/request_permission"}, {"params", QJsonObject{
                        {"sessionId", pendingPromptSession},
                        {"toolCall", QJsonObject{
                            {"toolCallId", "tc-perm"},
                            {"title", "<b>Apply edits to show</b>"},
                            {"kind", "edit"},
                            {"rawInput", rawInput}
                        }},
                        {"options", QJsonArray{
                            QJsonObject{{"optionId", "allow-once"}, {"name", "Allow once"}, {"kind", "allow_once"}},
                            QJsonObject{{"optionId", "reject-once"}, {"name", "Reject"}, {"kind", "reject_once"}}
                        }}
                    }}});
                }
                if (exitCleanAfterPermission) return 0;
            } else if (holdPrompt && (!staleOnNewSession || sessionCounter == 1)) {
                waitingCancel = true;
            } else if (burstAndExit) {
                for (int i = 0; i < 45; ++i) {
                    send({{"jsonrpc", "2.0"}, {"method", "session/update"}, {"params", QJsonObject{
                        {"sessionId", pendingPromptSession},
                        {"update", QJsonObject{
                            {"sessionUpdate", "agent_message_chunk"},
                            {"content", QJsonObject{{"type", "text"}, {"text", QString("B%1 ").arg(i)}}}
                        }}
                    }}});
                }
                send({{"jsonrpc", "2.0"}, {"id", pendingPromptId}, {"result", QJsonObject{{"stopReason", "end_turn"}}}});
                return 0;
            } else {
                finishPrompt("ok");
            }
        } else if (method == "session/cancel") {
            if (waitingCancel && !ignoreCancel && !staleOnNewSession && !pendingPromptId.isUndefined() && !pendingPromptId.isNull()) {
                waitingCancel = false;
                send({{"jsonrpc", "2.0"}, {"id", pendingPromptId}, {"result", QJsonObject{{"stopReason", "cancelled"}}}});
                pendingPromptId = QJsonValue();
                pendingPromptSession.clear();
            }
        }
    }
    return 0;
}
