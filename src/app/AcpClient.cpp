#include "app/AcpClient.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QStandardPaths>

namespace sav {
namespace {
QStringList extraSearchPaths() {
    QStringList dirs;
    const QString home = QDir::homePath();
#ifdef Q_OS_WIN
    const QString appData = qEnvironmentVariable("APPDATA");
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
    if (!appData.isEmpty()) dirs << QDir(appData).filePath("npm");
    if (!localAppData.isEmpty()) dirs << QDir(localAppData).filePath("Programs");
    dirs << QDir(home).filePath(".local/bin")
         << QDir(home).filePath(".bun/bin")
         << QDir(home).filePath(".cargo/bin")
         << QDir(home).filePath(".opencode/bin");
#else
    dirs << "/opt/homebrew/bin"
         << "/usr/local/bin"
         << QDir(home).filePath(".local/bin")
         << QDir(home).filePath(".bun/bin")
         << QDir(home).filePath(".cargo/bin")
         << QDir(home).filePath(".opencode/bin")
         << QDir(home).filePath(".npm-global/bin");
#endif
    QStringList existing;
    for (const auto& dir : dirs) if (QDir(dir).exists() && !existing.contains(dir)) existing << dir;
    return existing;
}

QString extractContentText(const QJsonValue& content) {
    if (content.isString()) return content.toString();
    if (content.isObject()) {
        const auto obj = content.toObject();
        if (obj["type"].toString() == "text" || obj.contains("text")) return obj["text"].toString();
        if (obj.contains("content")) return extractContentText(obj["content"]);
    }
    if (content.isArray()) {
        QString combined;
        for (const auto& item : content.toArray()) combined += extractContentText(item);
        return combined;
    }
    return {};
}

QString summarizeJson(const QJsonValue& value) {
    if (value.isUndefined() || value.isNull()) return {};
    if (value.isString()) return value.toString();
    if (value.isObject()) return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    if (value.isArray()) return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    return value.toVariant().toString();
}
}

std::vector<AcpAgentPreset> defaultAcpPresets() {
    return {
        {"opencode", "OpenCode", "opencode", {"acp"}, "Install OpenCode from opencode.ai and run `opencode auth` once."},
        {"claude", "Claude Code (ACP)", "claude-agent-acp", {}, "Install with `npm install -g @zed-industries/claude-agent-acp`."},
        {"codex", "Codex (ACP)", "codex-acp", {}, "Install with `npm install -g @zed-industries/codex-acp`."},
        {"antigravity", "Antigravity (agy)", "agy", {"acp"}, "Install Google Antigravity CLI (`agy`) and run `agy acp --login` once."},
        {"pi", "Pi (pi-acp)", "pi-acp", {}, "Install with `npm install -g pi-acp` alongside Pi."},
        {"custom", "Custom ACP command…", {}, {}, "Enter any ACP-compatible executable and arguments (e.g. `my-agent acp`)."}
    };
}

QString findAcpExecutable(const QString& program) {
    const QString trimmed = program.trimmed();
    if (trimmed.isEmpty()) return {};
    if (trimmed.contains('/') || trimmed.contains('\\')) {
        const QFileInfo info(trimmed);
        return (info.exists() && info.isFile() && info.isExecutable()) ? info.absoluteFilePath() : QString();
    }
    if (const QString found = QStandardPaths::findExecutable(trimmed); !found.isEmpty()) return found;
    return QStandardPaths::findExecutable(trimmed, extraSearchPaths());
}

QStringList splitCommandLine(const QString& commandLine) {
    QStringList args;
    QString current;
    bool inToken = false;
    QChar quote;
    for (int i = 0; i < commandLine.size(); ++i) {
        const QChar c = commandLine[i];
        if (!quote.isNull()) {
            if (c == quote) {
                quote = QChar();
            } else if (c == '\\' && quote == '"' && i + 1 < commandLine.size()) {
                current += commandLine[++i];
            } else {
                current += c;
            }
            continue;
        }
        if (c.isSpace()) {
            if (inToken) { args << current; current.clear(); inToken = false; }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c; inToken = true;
            continue;
        }
        if (c == '\\' && i + 1 < commandLine.size()) {
            current += commandLine[++i]; inToken = true;
            continue;
        }
        current += c; inToken = true;
    }
    if (inToken) args << current;
    return args;
}

AcpClient::AcpClient(QObject* parent) : QObject(parent) {}

AcpClient::~AcpClient() {
    disconnectFromAgent();
}

void AcpClient::setState(AcpState next) {
    if (state_ == next) return;
    state_ = next;
    emit stateChanged(state_);
}

void AcpClient::connectToAgent(const QString& program, const QStringList& args, const QString& cwd, const AcpMcpConfig& mcp) {
    disconnectFromAgent();
    const QString executable = findAcpExecutable(program);
    if (executable.isEmpty()) {
        failConnection(QString("Could not find executable “%1”.").arg(program));
        return;
    }
    stopping_ = false;
    cwd_ = cwd.isEmpty() ? QDir::homePath() : cwd;
    mcp_ = mcp;
    agentName_.clear(); agentVersion_.clear(); sessionId_.clear(); lastError_.clear();
    agentSupportsHttpMcp_ = false; usedHttpMcp_ = false;
    stdoutBuffer_.clear(); pending_.clear(); pendingPermissions_.clear();
    nextId_ = 1;

    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::SeparateChannels);
    process_->setWorkingDirectory(cwd_);

    // Ensure GUI-launched apps (e.g. macOS .app bundles) still find Homebrew/npm/bun tools.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString sep = QDir::listSeparator();
    QStringList pathParts = env.value("PATH").split(sep, Qt::SkipEmptyParts);
    for (const auto& extra : extraSearchPaths()) if (!pathParts.contains(extra)) pathParts << extra;
    env.insert("PATH", pathParts.join(sep));
    process_->setProcessEnvironment(env);

    connect(process_, &QProcess::readyReadStandardOutput, this, &AcpClient::handleStdout);
    connect(process_, &QProcess::readyReadStandardError, this, [this] {
        if (!process_) return;
        const QString text = QString::fromUtf8(process_->readAllStandardError());
        if (!text.isEmpty()) emit stderrReceived(text);
    });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (stopping_ || !process_) return;
        if (error == QProcess::FailedToStart) failConnection("Failed to start ACP agent: " + process_->errorString());
        else if (state_ != AcpState::Disconnected) failConnection("ACP agent process error: " + process_->errorString());
    });
    connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
        [this](int exitCode, QProcess::ExitStatus exitStatus) {
            if (stopping_) return;
            const QString reason = exitStatus == QProcess::CrashExit
                ? QString("ACP agent crashed (exit code %1).").arg(exitCode)
                : QString("ACP agent exited (code %1).").arg(exitCode);
            const bool hadSession = state_ == AcpState::Ready || state_ == AcpState::Prompting || state_ == AcpState::Connecting;
            pending_.clear();
            pendingPermissions_.clear();
            sessionId_.clear();
            if (process_) { process_->deleteLater(); process_ = nullptr; }
            if (hadSession && exitCode != 0) failConnection(reason);
            else setState(AcpState::Disconnected);
        });

    setState(AcpState::Connecting);
    process_->start(executable, args);
    if (!process_->waitForStarted(5000)) {
        const QString err = process_ ? process_->errorString() : QString("Timed out starting process");
        disconnectFromAgent();
        failConnection("Could not start ACP agent: " + err);
        return;
    }

    QJsonObject clientCaps{
        {"fs", QJsonObject{{"readTextFile", false}, {"writeTextFile", false}}},
        {"terminal", false}
    };
    QJsonObject params{
        {"protocolVersion", 1},
        {"clientCapabilities", clientCaps},
        {"clientInfo", QJsonObject{
            {"name", "SimpleAVShow"},
            {"version", QCoreApplication::applicationVersion().isEmpty() ? QString("0.6.0") : QCoreApplication::applicationVersion()}
        }}
    };
    sendRequest("initialize", params, PendingKind::Initialize);
}

void AcpClient::startNewSession(const QString& cwd, const AcpMcpConfig& mcp) {
    if (!process_ || process_->state() != QProcess::Running) return;
    if (state_ == AcpState::Prompting) cancelPrompt();
    cwd_ = cwd.isEmpty() ? cwd_ : cwd;
    mcp_ = mcp;
    sessionId_.clear();
    setState(AcpState::Connecting);
    QJsonObject params{
        {"cwd", cwd_},
        {"mcpServers", buildMcpServers()}
    };
    sendRequest("session/new", params, PendingKind::SessionNew);
}

void AcpClient::sendPrompt(const QString& text) {
    if (!process_ || state_ != AcpState::Ready || sessionId_.isEmpty()) return;
    setState(AcpState::Prompting);
    QJsonObject params{
        {"sessionId", sessionId_},
        {"prompt", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}}
    };
    sendRequest("session/prompt", params, PendingKind::Prompt);
}

void AcpClient::cancelPrompt() {
    if (!process_ || sessionId_.isEmpty()) return;
    cancelAllPendingPermissions();
    if (state_ == AcpState::Prompting) {
        sendNotification("session/cancel", {{"sessionId", sessionId_}});
    }
}

void AcpClient::respondPermission(int requestKey, const QString& optionId) {
    if (!pendingPermissions_.contains(requestKey)) return;
    const QJsonValue id = pendingPermissions_.take(requestKey);
    QJsonObject outcome;
    if (optionId.isEmpty()) outcome = {{"outcome", "cancelled"}};
    else outcome = {{"outcome", "selected"}, {"optionId", optionId}};
    sendResponse(id, {{"outcome", outcome}});
}

void AcpClient::disconnectFromAgent() {
    stopping_ = true;
    cancelAllPendingPermissions();
    pending_.clear();
    pendingPermissions_.clear();
    stdoutBuffer_.clear();
    sessionId_.clear();
    if (process_) {
        process_->disconnect(this);
        if (process_->state() != QProcess::NotRunning) {
            process_->closeWriteChannel();
            process_->terminate();
            if (!process_->waitForFinished(400)) {
                process_->kill();
                process_->waitForFinished(400);
            }
        }
        process_->deleteLater();
        process_ = nullptr;
    }
    stopping_ = false;
    setState(AcpState::Disconnected);
}

void AcpClient::sendJson(const QJsonObject& payload) {
    if (!process_ || process_->state() != QProcess::Running) return;
    QByteArray line = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    line.append('\n');
    process_->write(line);
}

int AcpClient::sendRequest(const QString& method, const QJsonObject& params, PendingKind kind) {
    const int id = nextId_++;
    pending_.insert(id, kind);
    sendJson({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
    return id;
}

void AcpClient::sendNotification(const QString& method, const QJsonObject& params) {
    sendJson({{"jsonrpc", "2.0"}, {"method", method}, {"params", params}});
}

void AcpClient::sendResponse(const QJsonValue& id, const QJsonObject& result) {
    sendJson({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
}

void AcpClient::sendErrorResponse(const QJsonValue& id, int code, const QString& message) {
    sendJson({{"jsonrpc", "2.0"}, {"id", id}, {"error", QJsonObject{{"code", code}, {"message", message}}}});
}

void AcpClient::handleStdout() {
    if (!process_) return;
    stdoutBuffer_.append(process_->readAllStandardOutput());
    for (;;) {
        const qsizetype newline = stdoutBuffer_.indexOf('\n');
        if (newline < 0) break;
        QByteArray line = stdoutBuffer_.left(newline);
        stdoutBuffer_.remove(0, newline + 1);
        if (!line.isEmpty() && line.endsWith('\r')) line.chop(1);
        if (!line.trimmed().isEmpty()) handleLine(line);
    }
}

void AcpClient::handleLine(const QByteArray& line) {
    QJsonParseError err;
    const auto doc = QJsonDocument::fromJson(line, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        // Ignore stray non-JSON stdout noise and forward it to diagnostics.
        emit stderrReceived(QString::fromUtf8(line) + "\n");
        return;
    }
    const auto msg = doc.object();
    const bool hasMethod = msg["method"].isString();
    const bool hasId = msg.contains("id") && !msg["id"].isNull() && !msg["id"].isUndefined();
    if (hasMethod && hasId) {
        handleAgentRequest(msg["id"], msg["method"].toString(), msg["params"].toObject());
    } else if (hasMethod) {
        handleNotification(msg["method"].toString(), msg["params"].toObject());
    } else if (hasId && msg["id"].isDouble()) {
        handleResponse(msg["id"].toInt(), msg);
    }
}

void AcpClient::handleResponse(int id, const QJsonObject& msg) {
    if (!pending_.contains(id)) return;
    const PendingKind kind = pending_.take(id);
    if (msg.contains("error") && msg["error"].isObject()) {
        const auto errObj = msg["error"].toObject();
        const int code = errObj["code"].toInt();
        const QString errMessage = errObj["message"].toString("ACP request failed");
        if (kind == PendingKind::Prompt) {
            setState(AcpState::Ready);
            emit errorOccurred(errMessage);
            emit promptFinished("error");
            return;
        }
        if (code == -32000 || errMessage.contains("auth", Qt::CaseInsensitive)) {
            failConnection("Authentication required by the ACP agent: " + errMessage + ". Sign in via its CLI first, then reconnect.");
        } else {
            failConnection(errMessage);
        }
        return;
    }
    const auto result = msg["result"].toObject();
    if (kind == PendingKind::Initialize) {
        const auto info = result["agentInfo"].toObject();
        agentName_ = info["name"].toString();
        agentVersion_ = info["version"].toString();
        const auto caps = result["agentCapabilities"].toObject();
        const auto mcpCaps = caps.contains("mcpCapabilities") ? caps["mcpCapabilities"].toObject() : result["mcpCapabilities"].toObject();
        agentSupportsHttpMcp_ = mcpCaps["http"].toBool(false);
        QJsonObject params{
            {"cwd", cwd_},
            {"mcpServers", buildMcpServers()}
        };
        sendRequest("session/new", params, PendingKind::SessionNew);
    } else if (kind == PendingKind::SessionNew) {
        sessionId_ = result["sessionId"].toString();
        if (sessionId_.isEmpty()) {
            failConnection("ACP agent returned an empty sessionId.");
            return;
        }
        setState(AcpState::Ready);
        emit connected(agentName_.isEmpty() ? QString("ACP agent") : agentName_, sessionId_);
    } else if (kind == PendingKind::Prompt) {
        cancelAllPendingPermissions();
        setState(AcpState::Ready);
        emit promptFinished(result["stopReason"].toString("end_turn"));
    }
}

void AcpClient::handleNotification(const QString& method, const QJsonObject& params) {
    if (method != "session/update") return;
    const auto update = params["update"].toObject();
    const QString kind = update.contains("sessionUpdate") ? update["sessionUpdate"].toString() : update["type"].toString();
    if (kind == "agent_message_chunk") {
        const QString text = extractContentText(update["content"]);
        if (!text.isEmpty()) emit messageChunk(text);
    } else if (kind == "agent_thought_chunk") {
        const QString text = extractContentText(update["content"]);
        if (!text.isEmpty()) emit thoughtChunk(text);
    } else if (kind == "tool_call" || kind == "tool_call_update") {
        AcpToolCallUpdate call;
        call.toolCallId = update["toolCallId"].toString();
        call.title = update["title"].toString();
        call.kind = update["kind"].toString();
        call.status = update["status"].toString();
        call.detail = extractContentText(update["content"]);
        if (call.detail.isEmpty() && update.contains("rawOutput")) call.detail = summarizeJson(update["rawOutput"]);
        if (call.detail.isEmpty() && update.contains("rawInput")) call.detail = summarizeJson(update["rawInput"]);
        emit toolCallUpdated(call);
    } else if (kind == "plan") {
        QStringList items;
        for (const auto& entry : update["entries"].toArray()) {
            const auto obj = entry.toObject();
            const QString content = obj["content"].toString();
            const QString status = obj["status"].toString();
            if (!content.isEmpty()) items << (status.isEmpty() ? content : QString("[%1] %2").arg(status, content));
        }
        if (!items.isEmpty()) emit planUpdated(items);
    }
}

void AcpClient::handleAgentRequest(const QJsonValue& id, const QString& method, const QJsonObject& params) {
    if (method == "session/request_permission") {
        AcpPermissionRequest request;
        request.requestKey = nextPermissionKey_++;
        const auto toolCall = params["toolCall"].toObject();
        request.toolCallId = toolCall["toolCallId"].toString();
        request.title = toolCall["title"].toString("Agent permission request");
        request.kind = toolCall["kind"].toString();
        request.detail = summarizeJson(toolCall["rawInput"]);
        if (request.detail.isEmpty()) request.detail = extractContentText(toolCall["content"]);
        for (const auto& val : params["options"].toArray()) {
            const auto opt = val.toObject();
            if (!opt["optionId"].toString().isEmpty()) {
                request.options.push_back({opt["optionId"].toString(), opt["name"].toString(opt["optionId"].toString()), opt["kind"].toString()});
            }
        }
        pendingPermissions_.insert(request.requestKey, id);
        emit permissionRequested(request);
        return;
    }
    sendErrorResponse(id, -32601, "Method not supported by SimpleAVShow ACP client: " + method);
}

QJsonArray AcpClient::buildMcpServers() {
    QJsonArray servers;
    usedHttpMcp_ = false;
    if (mcp_.url.isEmpty()) return servers;
    if (agentSupportsHttpMcp_) {
        usedHttpMcp_ = true;
        QJsonArray headers;
        if (!mcp_.token.isEmpty()) headers.append(QJsonObject{{"name", "Authorization"}, {"value", "Bearer " + mcp_.token}});
        servers.append(QJsonObject{
            {"type", "http"},
            {"name", mcp_.name},
            {"url", mcp_.url},
            {"headers", headers}
        });
    } else {
        const QString exe = mcp_.proxyExecutable.isEmpty() ? QCoreApplication::applicationFilePath() : mcp_.proxyExecutable;
        QJsonArray env;
        if (!mcp_.token.isEmpty()) env.append(QJsonObject{{"name", "SAV_MCP_TOKEN"}, {"value", mcp_.token}});
        servers.append(QJsonObject{
            {"type", "stdio"},
            {"name", mcp_.name},
            {"command", exe},
            {"args", QJsonArray{"--mcp-proxy", mcp_.url}},
            {"env", env}
        });
    }
    return servers;
}

void AcpClient::cancelAllPendingPermissions() {
    const auto keys = pendingPermissions_.keys();
    for (int key : keys) respondPermission(key, {});
}

void AcpClient::failConnection(const QString& message) {
    lastError_ = message;
    setState(AcpState::Error);
    emit errorOccurred(message);
}

}
