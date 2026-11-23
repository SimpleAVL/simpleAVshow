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

    QString sessionId = "mock-session-1";
    QJsonArray lastMcpServers;
    bool waitingPermission = false;
    bool waitingCancel = false;
    QJsonValue pendingPromptId;
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
                {"content", QJsonObject{{"type", "text"}, {"text", note + " | mcp=" + mcpSummary}}}
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
            send({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{
                {"protocolVersion", 1},
                {"agentInfo", QJsonObject{{"name", "MockACP"}, {"version", "1.0"}}},
                {"agentCapabilities", QJsonObject{
                    {"mcpCapabilities", QJsonObject{{"http", httpMcp}, {"sse", false}}}
                }}
            }}});
        } else if (method == "session/new") {
            lastMcpServers = params["mcpServers"].toArray();
            send({{"jsonrpc", "2.0"}, {"id", id}, {"result", QJsonObject{{"sessionId", sessionId}}}});
        } else if (method == "session/prompt") {
            pendingPromptId = id;
            const auto blocks = params["prompt"].toArray();
            pendingPromptText = blocks.isEmpty() ? QString() : blocks.first().toObject()["text"].toString();
            if (requirePermission) {
                waitingPermission = true;
                send({{"jsonrpc", "2.0"}, {"id", "perm-1"}, {"method", "session/request_permission"}, {"params", QJsonObject{
                    {"sessionId", sessionId},
                    {"toolCall", QJsonObject{
                        {"toolCallId", "tc-perm"},
                        {"title", "Apply edits to show"},
                        {"kind", "edit"},
                        {"rawInput", QJsonObject{{"action", "apply_edits"}}}
                    }},
                    {"options", QJsonArray{
                        QJsonObject{{"optionId", "allow-once"}, {"name", "Allow once"}, {"kind", "allow_once"}},
                        QJsonObject{{"optionId", "reject-once"}, {"name", "Reject"}, {"kind", "reject_once"}}
                    }}
                }}});
            } else if (holdPrompt) {
                waitingCancel = true;
            } else {
                finishPrompt("ok");
            }
        } else if (method == "session/cancel") {
            if (waitingCancel && !pendingPromptId.isUndefined() && !pendingPromptId.isNull()) {
                waitingCancel = false;
                send({{"jsonrpc", "2.0"}, {"id", pendingPromptId}, {"result", QJsonObject{{"stopReason", "cancelled"}}}});
                pendingPromptId = QJsonValue();
            }
        }
    }
    return 0;
}
