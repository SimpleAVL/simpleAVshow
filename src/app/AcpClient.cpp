#include "app/AcpClient.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <chrono>
#include <cmath>
#include <thread>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#elif defined(Q_OS_UNIX)
#include <cerrno>
#include <csignal>
#include <unistd.h>
#ifdef Q_OS_LINUX
#include <sys/prctl.h>
#endif
#endif

namespace sav {

struct AcpClient::ProcessTreeHandle {
    qint64 pid = 0;
#ifdef Q_OS_WIN
    HANDLE job = nullptr;
#elif defined(Q_OS_UNIX)
    bool ownsProcessGroup = false;
#endif
    bool closed = false;

    void attach(qint64 childPid) {
        pid = childPid;
#if defined(Q_OS_UNIX)
        // Verify setpgid(0, 0) succeeded in the child before ever signaling -pid.
        // Note: on POSIX, process-group signals tear down the agent and any children remaining
        // in its process group (e.g. shell wrappers and --mcp-proxy), while a helper that
        // explicitly detaches into a new session/process group (setsid/setpgid) is outside the
        // group; its SimpleAVShow MCP access is still terminated immediately by bearer revocation.
        ownsProcessGroup = pid > 0 && ::getpgid(static_cast<pid_t>(pid)) == static_cast<pid_t>(pid);
#endif
    }

#ifdef Q_OS_WIN
    bool attachAndResume(qint64 childPid, QString& error) {
        pid = childPid;
        if (pid <= 0) {
            error = "Invalid ACP agent process ID.";
            return false;
        }
        auto terminateSuspended = [this] {
            if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid))) {
                TerminateProcess(h, 1);
                CloseHandle(h);
            }
        };
        if (!job) {
            HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
            if (!hJob) {
                error = QString("Could not create Windows Job Object for ACP agent (error %1).").arg(GetLastError());
                terminateSuspended();
                return false;
            }
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
                error = QString("Could not configure Windows Job Object for ACP agent (error %1).").arg(GetLastError());
                CloseHandle(hJob);
                terminateSuspended();
                return false;
            }
            HANDLE hProc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
            if (!hProc) {
                error = QString("Could not open ACP agent process for Job Object assignment (error %1).").arg(GetLastError());
                CloseHandle(hJob);
                terminateSuspended();
                return false;
            }
            const BOOL assigned = AssignProcessToJobObject(hJob, hProc);
            const DWORD err = assigned ? 0 : GetLastError();
            if (!assigned) {
                TerminateProcess(hProc, 1);
                CloseHandle(hProc);
                CloseHandle(hJob);
                error = QString("Could not assign ACP agent to Windows Job Object (error %1).").arg(err);
                return false;
            }
            CloseHandle(hProc);
            job = hJob;
        }
        // Child was spawned with CREATE_SUSPENDED so it cannot spawn helpers before job assignment.
        int resumed = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            if (Thread32First(snap, &te)) {
                do {
                    if (te.dwSize >= FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(te.th32OwnerProcessID) &&
                        te.th32OwnerProcessID == static_cast<DWORD>(pid)) {
                        HANDLE hThread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                        if (hThread) {
                            if (ResumeThread(hThread) != static_cast<DWORD>(-1)) ++resumed;
                            CloseHandle(hThread);
                        }
                    }
                    te.dwSize = sizeof(te);
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
        }
        if (resumed == 0) {
            error = "Could not resume suspended ACP agent process.";
            killTree();
            close();
            return false;
        }
        return true;
    }
#endif

    bool groupAlive() const {
#if defined(Q_OS_UNIX)
        if (closed || !ownsProcessGroup || pid <= 0) return false;
        return ::kill(-static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#else
        return false;
#endif
    }

    void terminateTree() {
#if defined(Q_OS_UNIX)
        if (!closed && ownsProcessGroup && pid > 0) ::kill(-static_cast<pid_t>(pid), SIGTERM);
#endif
    }

    void killTree() {
#ifdef Q_OS_WIN
        if (job && !closed) TerminateJobObject(job, 1);
#elif defined(Q_OS_UNIX)
        if (!closed && ownsProcessGroup && pid > 0) ::kill(-static_cast<pid_t>(pid), SIGKILL);
#endif
    }

    void close() {
        if (closed) return;
        closed = true;
#ifdef Q_OS_WIN
        if (job) {
            CloseHandle(job);
            job = nullptr;
        }
#endif
    }

    ~ProcessTreeHandle() { close(); }
};
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

QString summarizeJson(const QJsonValue& value) {
    if (value.isUndefined() || value.isNull()) return {};
    if (value.isString()) return value.toString();
    if (value.isObject()) return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    if (value.isArray()) return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    return value.toVariant().toString();
}

QString extractContentText(const QJsonValue& content) {
    if (content.isString()) return content.toString();
    if (content.isObject()) {
        const auto obj = content.toObject();
        const QString type = obj["type"].toString();
        if (type == "text" || obj.contains("text")) return obj["text"].toString();
        if (type == "diff" || (obj.contains("path") && (obj.contains("newText") || obj.contains("oldText")))) {
            QString out = "Diff: " + obj["path"].toString();
            if (obj.contains("oldText") && !obj["oldText"].isNull())
                out += "\n--- old\n" + obj["oldText"].toString();
            if (obj.contains("newText") && !obj["newText"].isNull())
                out += "\n+++ new\n" + obj["newText"].toString();
            return out;
        }
        if (obj.contains("content")) return extractContentText(obj["content"]);
        if (!obj.isEmpty()) return summarizeJson(obj);
    }
    if (content.isArray()) {
        QString combined;
        for (const auto& item : content.toArray()) {
            const QString part = extractContentText(item);
            if (part.isEmpty()) continue;
            const bool structured = item.isObject() && item.toObject()["type"].toString() != "text";
            if (!combined.isEmpty() && structured && !combined.endsWith('\n')) combined += '\n';
            combined += part;
            if (structured && !combined.endsWith('\n')) combined += '\n';
        }
        return combined;
    }
    return {};
}
}

std::vector<AcpAgentPreset> defaultAcpPresets() {
    return {
        {"opencode", "OpenCode", "opencode", {"acp"}, "Install OpenCode from opencode.ai and run `opencode auth` once."},
        {"claude", "Claude Code (ACP)", "claude-agent-acp", {}, "Install with `npm install -g @zed-industries/claude-agent-acp`."},
        {"codex", "Codex (ACP)", "codex-acp", {}, "Install with `npm install -g @agentclientprotocol/codex-acp`."},
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
        // Handle runs of backslashes immediately preceding a double quote (standard Win32/argv rules):
        // 2N backslashes + " => N literal backslashes + quote delimiter;
        // 2N+1 backslashes + " => N literal backslashes + literal ".
        if (c == '\\' && quote != '\'') {
            int bs = 0;
            while (i + bs < commandLine.size() && commandLine[i + bs] == '\\') ++bs;
            if (i + bs < commandLine.size() && commandLine[i + bs] == '"') {
                current += QString(bs / 2, '\\');
                inToken = true;
                if (bs % 2 == 1) {
                    current += '"';
                    i += bs;
                } else {
                    // Advance to the last backslash; the next iteration processes the quote delimiter.
                    i += bs - 1;
                }
                continue;
            }
            // Backslashes not preceding a double quote are literal (preserves Windows paths).
            current += QString(bs, '\\');
            inToken = true;
            i += bs - 1;
            continue;
        }
        if (!quote.isNull()) {
            if (quote == '"' && c == '"' && i + 1 < commandLine.size() && commandLine[i + 1] == '"') {
                current += '"';
                ++i;
            } else if (c == quote) {
                quote = QChar();
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
        current += c; inToken = true;
    }
    if (!quote.isNull()) return {}; // Reject unclosed quotes.
    if (inToken) args << current;
    return args;
}

bool validateBatchWrapperArgs(const QString& executable, const QStringList& args, QString& error) {
    if (!executable.endsWith(".cmd", Qt::CaseInsensitive) && !executable.endsWith(".bat", Qt::CaseInsensitive))
        return true;
    static const QString forbidden = QStringLiteral("&|<>^%!\"\r\n");
    for (const auto& arg : args) {
        for (const QChar ch : arg) {
            if (forbidden.contains(ch)) {
                error = "Arguments containing Windows cmd.exe metacharacters (& | < > ^ % ! \") cannot be safely passed to a .cmd/.bat wrapper; invoke the underlying executable directly instead.";
                return false;
            }
        }
    }
    return true;
}

AcpClient::AcpClient(QObject* parent) : QObject(parent) {
    deadlineTimer_.setSingleShot(true);
    connect(&deadlineTimer_, &QTimer::timeout, this, [this] {
        if (state_ == AcpState::Connecting || state_ == AcpState::Prompting) {
            failConnection(deadlineReason_.isEmpty() ? QString("ACP agent timed out.") : deadlineReason_);
        }
    });
    cancelTimer_.setSingleShot(true);
    connect(&cancelTimer_, &QTimer::timeout, this, [this] {
        if (state_ != AcpState::Disconnected && state_ != AcpState::Error) {
            failConnection("Timed out waiting for ACP agent to cancel the active turn.");
        }
    });
}

AcpClient::~AcpClient() {
    stopping_ = true;
    deadlineTimer_.stop();
    cancelTimer_.stop();
    teardownProcess(true);
    for (auto& entry : retiringProcesses_) {
        if (entry.process && entry.process->state() != QProcess::NotRunning) {
            if (entry.tree) entry.tree->killTree();
            entry.process->kill();
            entry.process->waitForFinished(400);
        }
        if (entry.tree) {
            if (entry.tree->groupAlive()) entry.tree->killTree();
            entry.tree->close();
        }
        if (entry.process) delete entry.process.data();
    }
    retiringProcesses_.clear();
    for (auto& tree : retiringTrees_) {
        if (!tree) continue;
        if (tree->groupAlive()) tree->killTree();
        tree->close();
    }
    retiringTrees_.clear();
}

void AcpClient::scheduleTreeCleanup(std::shared_ptr<ProcessTreeHandle> tree) {
    if (!tree || tree->closed) return;
    retiringTrees_.erase(
        std::remove_if(retiringTrees_.begin(), retiringTrees_.end(),
            [](const std::shared_ptr<ProcessTreeHandle>& t) { return !t || t->closed; }),
        retiringTrees_.end());
    retiringTrees_.push_back(tree);
    // Owned by `this` (not the retiring QProcess) so group cleanup survives an early leader exit.
    QTimer::singleShot(400, this, [this, tree] {
        if (!tree->closed) {
            if (tree->groupAlive()) tree->killTree();
            tree->close();
        }
        std::erase(retiringTrees_, tree);
    });
}

void AcpClient::teardownProcess(bool blocking) {
    auto tree = std::move(processTree_);
    if (!process_) {
        if (tree) {
            if (tree->groupAlive()) {
                tree->terminateTree();
                if (blocking) {
                    for (int i = 0; i < 15 && tree->groupAlive(); ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    if (tree->groupAlive()) tree->killTree();
                    tree->close();
                } else {
                    scheduleTreeCleanup(std::move(tree));
                }
            } else {
                tree->close();
            }
        }
        return;
    }
    QProcess* proc = process_;
    process_ = nullptr;
    proc->disconnect(this);
    if (proc->state() == QProcess::NotRunning) {
        if (tree) {
            if (tree->groupAlive()) {
                tree->terminateTree();
                if (blocking) {
                    for (int i = 0; i < 15 && tree->groupAlive(); ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    if (tree->groupAlive()) tree->killTree();
                    tree->close();
                } else {
                    scheduleTreeCleanup(std::move(tree));
                }
            } else {
                tree->close();
            }
        }
        proc->deleteLater();
        return;
    }
    if (tree && tree->pid <= 0) tree->attach(proc->processId());
    proc->closeWriteChannel();
    if (tree) tree->terminateTree();
    proc->terminate();
    if (blocking) {
        const bool exited = proc->waitForFinished(400);
        if (!exited) {
            if (tree) tree->killTree();
            proc->kill();
            proc->waitForFinished(400);
        } else if (tree && tree->groupAlive()) {
            // Leader exited on SIGTERM, but a child helper in its process group is still alive.
            for (int i = 0; i < 15 && tree->groupAlive(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (tree->groupAlive()) tree->killTree();
        }
        if (tree) tree->close();
        proc->deleteLater();
        return;
    }
    // Never block the GUI thread during live performances: escalate to kill asynchronously.
    // Keep the process-group supervisor alive on `this` even if the leader exits quickly.
    proc->setParent(nullptr);
    retiringProcesses_.erase(
        std::remove_if(retiringProcesses_.begin(), retiringProcesses_.end(),
            [](const RetiringProcess& entry) { return entry.process.isNull(); }),
        retiringProcesses_.end());
    retiringProcesses_.push_back({proc, tree});
    scheduleTreeCleanup(tree);
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), proc, [proc, tree] {
#ifdef Q_OS_WIN
        // On Windows, closing JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE terminates all surviving descendants.
        if (tree) tree->close();
#endif
        proc->deleteLater();
    });
    QTimer::singleShot(400, proc, [p = QPointer<QProcess>(proc), tree] {
        if (!p) return;
        if (p->state() != QProcess::NotRunning) {
            if (tree) tree->killTree();
            p->kill();
        }
    });
    QTimer::singleShot(2000, proc, [p = QPointer<QProcess>(proc), tree] {
        if (p && p->state() != QProcess::NotRunning) {
            if (tree) tree->killTree();
            p->kill();
        }
        if (tree) tree->close();
        if (p) p->deleteLater();
    });
}

void AcpClient::setTimeoutsForTesting(int startupMs, int handshakeMs, int cancelMs) {
    startupTimeoutMs_ = std::max(10, startupMs);
    handshakeTimeoutMs_ = std::max(10, handshakeMs);
    cancelTimeoutMs_ = std::max(10, cancelMs);
}

void AcpClient::armDeadline(int ms, const QString& reason) {
    deadlineReason_ = reason;
    deadlineTimer_.start(ms);
}

void AcpClient::setState(AcpState next) {
    if (state_ == next) return;
    state_ = next;
    emit stateChanged(state_);
}

void AcpClient::connectToAgent(const QString& program, const QStringList& args, const QString& cwd, const AcpMcpConfig& mcp) {
    program_ = program;
    args_ = args;
    cwd_ = cwd.isEmpty() ? QDir::homePath() : cwd;
    mcp_ = mcp;
    launchProcess(true);
}

void AcpClient::launchProcess(bool emitDisconnectedOnReset) {
    if (emitDisconnectedOnReset) {
        disconnectFromAgent();
    } else {
        stopping_ = true;
        deadlineTimer_.stop();
        cancelTimer_.stop();
        cancelAllPendingPermissions();
        pending_.clear();
        sessionId_.clear();
        teardownProcess(false);
    }
    const QString executable = findAcpExecutable(program_);
    if (executable.isEmpty()) {
        failConnection(QString("Could not find executable “%1”.").arg(program_));
        return;
    }
#ifdef Q_OS_WIN
    QString batchErr;
    if (!validateBatchWrapperArgs(executable, args_, batchErr)) {
        failConnection(batchErr);
        return;
    }
#endif
    stopping_ = false;
    cancelling_ = false;
    awaitingOldPromptCancel_ = false;
    stdoutDrainScheduled_ = false;
    stderrDrainScheduled_ = false;
    agentName_.clear(); agentVersion_.clear(); sessionId_.clear(); lastError_.clear();
    agentSupportsHttpMcp_ = false; agentSupportsSessionClose_ = false; usedHttpMcp_ = false;
    stdoutBuffer_.clear(); stderrDecoder_.resetState(); pending_.clear(); pendingPermissions_.clear();
    knownToolCalls_.clear(); knownToolCallOrder_.clear(); knownToolCallsBytes_ = 0; hadToolCallEviction_ = false;
    nextId_ = 1;

    process_ = new QProcess(this);
    processTree_ = std::make_shared<ProcessTreeHandle>();
    process_->setProcessChannelMode(QProcess::SeparateChannels);
#ifdef Q_OS_WIN
    // Start suspended so the child is assigned to the kill-on-close Job Object before it can spawn helpers.
    process_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* createArgs) {
        createArgs->flags |= CREATE_SUSPENDED;
    });
#elif defined(Q_OS_UNIX)
    // Put the agent in its own process group so Disconnect/shutdown tears down wrapper/grandchild processes too.
    process_->setChildProcessModifier([] {
        ::setpgid(0, 0);
#ifdef Q_OS_LINUX
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
    });
#endif
    // Launch the agent executable from a trusted application directory so Windows .cmd shims
    // cannot resolve runtimes from an untrusted show folder; pass the show directory via ACP session/new cwd.
    const QString launchDir = QCoreApplication::applicationDirPath().isEmpty()
        ? QDir::homePath() : QCoreApplication::applicationDirPath();
    process_->setWorkingDirectory(launchDir);

    // Ensure GUI-launched apps (e.g. macOS .app bundles) still find Homebrew/npm/bun tools.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString sep = QDir::listSeparator();
    QStringList pathParts = env.value("PATH").split(sep, Qt::SkipEmptyParts);
    for (const auto& extra : extraSearchPaths()) if (!pathParts.contains(extra)) pathParts << extra;
    env.insert("PATH", pathParts.join(sep));
#ifdef Q_OS_WIN
    env.insert("NoDefaultCurrentDirectoryInExePath", "1");
#endif
    process_->setProcessEnvironment(env);

    QProcess* const spawnedProcess = process_;
    connect(spawnedProcess, &QProcess::started, this, [this, spawnedProcess] {
        if (!process_ || process_ != spawnedProcess) return;
#ifdef Q_OS_WIN
        QString jobError;
        if (!processTree_ || !processTree_->attachAndResume(process_->processId(), jobError)) {
            if (!jobError.isEmpty()) emit stderrReceived(jobError + "\n");
            failConnection(jobError.isEmpty() ? QString("Failed to start ACP agent process.") : jobError);
            return;
        }
#else
        if (processTree_) processTree_->attach(process_->processId());
#endif
        if (stopping_ || state_ != AcpState::Connecting) return;
        armDeadline(handshakeTimeoutMs_, "Timed out waiting for ACP agent handshake.");
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
    });
    connect(process_, &QProcess::readyReadStandardOutput, this, &AcpClient::handleStdout);
    connect(process_, &QProcess::readyReadStandardError, this, &AcpClient::handleStderr);
    connect(spawnedProcess, &QProcess::errorOccurred, this, [this, spawnedProcess](QProcess::ProcessError error) {
        if (stopping_ || !process_ || process_ != spawnedProcess) return;
        if (error == QProcess::FailedToStart) failConnection("Failed to start ACP agent: " + process_->errorString());
        else if (state_ != AcpState::Disconnected) failConnection("ACP agent process error: " + process_->errorString());
    });
    connect(spawnedProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
        [this, spawnedProcess](int exitCode, QProcess::ExitStatus exitStatus) {
            if (stopping_ || process_ != spawnedProcess) return;
            deadlineTimer_.stop();
            cancelTimer_.stop();
            // Drain any remaining buffered stdout/stderr lines before tearing down session state so a
            // clean agent exit after a burst >32 lines never drops the tail of a reply or prompt result.
            // Guard against handleLine() launching a replacement process (e.g. session/close fallback).
            if (process_ == spawnedProcess) {
                constexpr qsizetype maxStdoutBufferBytes = 16 * 1024 * 1024;
                constexpr qint64 maxStderrTailBytes = 64 * 1024;
                constexpr int maxExitDrainLines = 2048;
                spawnedProcess->setReadChannel(QProcess::StandardError);
                const QByteArray stderrTail = spawnedProcess->read(maxStderrTailBytes);
                spawnedProcess->setReadChannel(QProcess::StandardOutput);
                if (!stderrTail.isEmpty()) {
                    const QString decoded = stderrDecoder_(stderrTail);
                    if (!decoded.isEmpty()) emit stderrReceived(decoded);
                }
                stderrDecoder_.resetState();
                const qint64 remainingBudget = std::max<qint64>(0, maxStdoutBufferBytes * 2 - stdoutBuffer_.size());
                if (remainingBudget > 0) stdoutBuffer_.append(spawnedProcess->read(remainingBudget));
                int drained = 0;
                while (process_ == spawnedProcess && !stopping_ && drained < maxExitDrainLines) {
                    const qsizetype newline = stdoutBuffer_.indexOf('\n');
                    if (newline < 0 || newline > maxStdoutBufferBytes) break;
                    QByteArray line = stdoutBuffer_.left(newline);
                    stdoutBuffer_.remove(0, newline + 1);
                    ++drained;
                    if (!line.isEmpty() && line.endsWith('\r')) line.chop(1);
                    if (!line.trimmed().isEmpty()) handleLine(line);
                }
                if (stopping_ || process_ != spawnedProcess) return;
            }
            const bool duringConnect = state_ == AcpState::Connecting;
            const QString reason = exitStatus == QProcess::CrashExit
                ? QString("ACP agent crashed (exit code %1).").arg(exitCode)
                : duringConnect && exitCode == 0
                    ? QString("ACP agent exited before completing handshake (code 0).")
                    : QString("ACP agent exited (code %1).").arg(exitCode);
            const bool hadSession = state_ == AcpState::Ready || state_ == AcpState::Prompting || duringConnect;
            pending_.clear();
            pendingPermissions_.clear();
            awaitingOldPromptCancel_ = false;
            knownToolCalls_.clear();
            knownToolCallOrder_.clear();
            knownToolCallsBytes_ = 0;
            hadToolCallEviction_ = false;
            stdoutBuffer_.clear();
            sessionId_.clear();
            if (auto tree = std::move(processTree_)) {
#if defined(Q_OS_UNIX)
                if (tree->groupAlive()) {
                    tree->terminateTree();
                    scheduleTreeCleanup(std::move(tree));
                } else {
                    tree->close();
                }
#else
                tree->close();
#endif
            }
            if (process_) { process_->deleteLater(); process_ = nullptr; }
            if (hadSession && (exitCode != 0 || exitStatus == QProcess::CrashExit || duringConnect)) failConnection(reason);
            else setState(AcpState::Disconnected);
        });

    setState(AcpState::Connecting);
    armDeadline(startupTimeoutMs_, "Timed out starting ACP agent process.");
    // Do not call waitForStarted() or dereference process_ after start(): on Windows
    // QProcess::start() can emit FailedToStart synchronously and clear process_.
    process_->start(executable, args_);
}

void AcpClient::startNewSession(const QString& cwd, const AcpMcpConfig& mcp) {
    const QString prevSession = sessionId_;
    cwd_ = cwd.isEmpty() ? cwd_ : cwd;
    mcp_ = mcp;
    if (!process_ || process_->state() != QProcess::Running) {
        if (!program_.isEmpty()) launchProcess(false);
        else failConnection("ACP agent process is not running.");
        return;
    }
    // Agents that lack ACP session/close keep old sessions and their MCP/stdio helpers alive
    // indefinitely if we only send session/new; restart the agent process cleanly instead.
    if (!agentSupportsSessionClose_ && !prevSession.isEmpty() && !program_.isEmpty()) {
        launchProcess(false);
        return;
    }
    if (state_ == AcpState::Prompting) cancelPrompt();
    else cancelAllPendingPermissions();
    cancelling_ = false;
    awaitingOldPromptCancel_ = false;
    knownToolCalls_.clear();
    knownToolCallOrder_.clear();
    knownToolCallsBytes_ = 0;
    hadToolCallEviction_ = false;
    sessionId_.clear();
    setState(AcpState::Connecting);
    armDeadline(handshakeTimeoutMs_, "Timed out waiting for ACP session creation.");
    if (agentSupportsSessionClose_ && !prevSession.isEmpty()) {
        sendRequest("session/close", {{"sessionId", prevSession}}, PendingKind::SessionClose, prevSession);
        return;
    }
    QJsonObject params{
        {"cwd", cwd_},
        {"mcpServers", buildMcpServers()}
    };
    sendRequest("session/new", params, PendingKind::SessionNew);
}

void AcpClient::sendPrompt(const QString& text) {
    if (!process_ || state_ != AcpState::Ready || sessionId_.isEmpty()) return;
    cancelling_ = false;
    setState(AcpState::Prompting);
    QJsonObject params{
        {"sessionId", sessionId_},
        {"prompt", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}}
    };
    sendRequest("session/prompt", params, PendingKind::Prompt, sessionId_);
}

void AcpClient::cancelPrompt() {
    if (!process_ || sessionId_.isEmpty()) return;
    cancelling_ = true;
    cancelAllPendingPermissions(sessionId_);
    if (state_ == AcpState::Prompting) {
        sendNotification("session/cancel", {{"sessionId", sessionId_}});
        cancelTimer_.start(cancelTimeoutMs_);
    }
}

void AcpClient::respondPermission(int requestKey, const QString& optionId) {
    if (!pendingPermissions_.contains(requestKey)) return;
    const PendingPermission pending = pendingPermissions_.take(requestKey);
    QJsonObject outcome;
    if (optionId.isEmpty()) outcome = {{"outcome", "cancelled"}};
    else outcome = {{"outcome", "selected"}, {"optionId", optionId}};
    sendResponse(pending.rpcId, {{"outcome", outcome}});
}

void AcpClient::disconnectFromAgent() {
    stopping_ = true;
    deadlineTimer_.stop();
    cancelTimer_.stop();
    cancelling_ = false;
    awaitingOldPromptCancel_ = false;
    cancelAllPendingPermissions();
    pending_.clear();
    pendingPermissions_.clear();
    knownToolCalls_.clear();
    knownToolCallOrder_.clear();
    knownToolCallsBytes_ = 0;
    hadToolCallEviction_ = false;
    stdoutBuffer_.clear();
    sessionId_.clear();
    teardownProcess(false);
    stopping_ = false;
    setState(AcpState::Disconnected);
}

void AcpClient::sendJson(const QJsonObject& payload) {
    if (!process_ || process_->state() != QProcess::Running) return;
    QByteArray line = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    line.append('\n');
    process_->write(line);
}

int AcpClient::sendRequest(const QString& method, const QJsonObject& params, PendingKind kind, const QString& sessionId) {
    const int id = nextId_++;
    pending_.insert(id, {kind, sessionId});
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

void AcpClient::handleStderr() {
    if (!process_) return;
    constexpr qint64 maxStderrReadPerTurn = 16 * 1024;
    process_->setReadChannel(QProcess::StandardError);
    const QByteArray chunk = process_->read(maxStderrReadPerTurn);
    const bool hasMore = process_->bytesAvailable() > 0;
    process_->setReadChannel(QProcess::StandardOutput);
    if (!chunk.isEmpty()) {
        const QString decoded = stderrDecoder_(chunk);
        if (!decoded.isEmpty()) emit stderrReceived(decoded);
    }
    if (hasMore && !stderrDrainScheduled_) {
        stderrDrainScheduled_ = true;
        QTimer::singleShot(16, this, [this] {
            stderrDrainScheduled_ = false;
            handleStderr();
        });
    }
}

void AcpClient::handleStdout() {
    if (!process_) return;
    // Allow up to 16 MiB per JSON-RPC line so full-resolution 1280x1280 base64 PNG preview
    // tool results echoed inside tool_call_update.rawOutput do not disconnect the ACP chat.
    constexpr qsizetype maxStdoutBufferBytes = 16 * 1024 * 1024;
    constexpr qint64 maxReadPerTurn = 64 * 1024;
    constexpr int maxLinesPerTurn = 32;
    process_->setReadChannel(QProcess::StandardOutput);
    const QByteArray incoming = process_->read(maxReadPerTurn);
    if (!incoming.isEmpty()) stdoutBuffer_.append(incoming);
    if (stdoutBuffer_.size() > maxStdoutBufferBytes * 2) {
        failConnection("ACP agent output exceeded maximum buffer size.");
        return;
    }
    int processedLines = 0;
    while (processedLines < maxLinesPerTurn) {
        const qsizetype newline = stdoutBuffer_.indexOf('\n');
        if (newline < 0) {
            if (stdoutBuffer_.size() > maxStdoutBufferBytes) {
                failConnection("ACP agent output exceeded maximum line size (16 MiB).");
            }
            break;
        }
        if (newline > maxStdoutBufferBytes) {
            failConnection("ACP agent output exceeded maximum line size (16 MiB).");
            break;
        }
        QByteArray line = stdoutBuffer_.left(newline);
        stdoutBuffer_.remove(0, newline + 1);
        ++processedLines;
        if (!line.isEmpty() && line.endsWith('\r')) line.chop(1);
        if (!line.trimmed().isEmpty()) {
            handleLine(line);
            if (!process_) return;
        }
    }
    if (process_ && (stdoutBuffer_.contains('\n') || process_->bytesAvailable() > 0) && !stdoutDrainScheduled_) {
        stdoutDrainScheduled_ = true;
        // Yield back to the Qt event loop so Stop All, audio timers, and Program output stay responsive.
        QTimer::singleShot(0, this, [this] {
            stdoutDrainScheduled_ = false;
            handleStdout();
        });
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
    if (msg["jsonrpc"] != "2.0") {
        emit stderrReceived(QString::fromUtf8(line) + "\n");
        return;
    }
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
    const PendingRequest req = pending_.take(id);
    const PendingKind kind = req.kind;
    auto clearCancelTimerIfDone = [this] {
        const bool anyPromptLeft = std::any_of(pending_.cbegin(), pending_.cend(),
            [](const PendingRequest& p) { return p.kind == PendingKind::Prompt; });
        if (!anyPromptLeft) cancelTimer_.stop();
    };
    const bool hasErrorKey = msg.contains("error");
    const bool hasResultKey = msg.contains("result");
    if (hasErrorKey == hasResultKey || (hasErrorKey && !msg["error"].isObject()) || (hasResultKey && !msg["result"].isObject())) {
        failConnection("ACP agent returned a malformed JSON-RPC response.");
        return;
    }
    const bool hasError = hasErrorKey;
    if (kind == PendingKind::Prompt) clearCancelTimerIfDone();
    if (kind == PendingKind::Prompt && (sessionId_.isEmpty() || req.sessionId != sessionId_)) {
        // Ignore stale prompt completion from a superseded session; if a new session was waiting for
        // this cancel to settle before becoming Ready, promote it now.
        cancelAllPendingPermissions(req.sessionId);
        if (awaitingOldPromptCancel_ && !cancelTimer_.isActive() && !sessionId_.isEmpty()) {
            awaitingOldPromptCancel_ = false;
            deadlineTimer_.stop();
            setState(AcpState::Ready);
            emit connected(agentName_.isEmpty() ? QString("ACP agent") : agentName_, sessionId_);
        }
        return;
    }
    if (hasError) {
        const auto errObj = msg["error"].toObject();
        const int code = errObj["code"].toInt();
        const QString errMessage = errObj["message"].toString("ACP request failed");
        if (kind == PendingKind::Prompt) {
            deadlineTimer_.stop();
            cancelling_ = false;
            cancelAllPendingPermissions(req.sessionId);
            setState(AcpState::Ready);
            emit errorOccurred(errMessage);
            emit promptFinished("error");
            return;
        }
        if (kind == PendingKind::SessionClose) {
            // Per ACP session/close, -32002 (Resource not found) means the session is already closed
            // and should proceed directly to session/new without restarting the agent process.
            if (code == -32002) {
                QJsonObject params{
                    {"cwd", cwd_},
                    {"mcpServers", buildMcpServers()}
                };
                sendRequest("session/new", params, PendingKind::SessionNew);
                return;
            }
            // Closing the previous session failed: restart the agent process so no orphaned session lingers.
            if (!program_.isEmpty()) launchProcess(false);
            else failConnection(errMessage);
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
        const auto protoVal = result["protocolVersion"];
        if (!protoVal.isDouble() || std::floor(protoVal.toDouble()) != protoVal.toDouble() || protoVal.toInt() != 1) {
            failConnection(protoVal.isUndefined() || protoVal.isNull()
                ? QString("ACP agent did not return a supported protocolVersion.")
                : QString("Incompatible ACP protocolVersion (%1); SimpleAVShow supports protocolVersion 1.").arg(protoVal.toVariant().toString()));
            return;
        }
        const auto info = result["agentInfo"].toObject();
        agentName_ = info["name"].toString();
        agentVersion_ = info["version"].toString();
        const auto caps = result["agentCapabilities"].toObject();
        const auto mcpCaps = caps.contains("mcpCapabilities") ? caps["mcpCapabilities"].toObject() : result["mcpCapabilities"].toObject();
        agentSupportsHttpMcp_ = mcpCaps["http"].toBool(false);
        const auto sessionCaps = caps["sessionCapabilities"].toObject();
        agentSupportsSessionClose_ = sessionCaps.contains("close") &&
            (sessionCaps["close"].isObject() || sessionCaps["close"].toBool(false));
        QJsonObject params{
            {"cwd", cwd_},
            {"mcpServers", buildMcpServers()}
        };
        sendRequest("session/new", params, PendingKind::SessionNew);
    } else if (kind == PendingKind::SessionClose) {
        QJsonObject params{
            {"cwd", cwd_},
            {"mcpServers", buildMcpServers()}
        };
        sendRequest("session/new", params, PendingKind::SessionNew);
    } else if (kind == PendingKind::SessionNew) {
        knownToolCalls_.clear();
        knownToolCallOrder_.clear();
        knownToolCallsBytes_ = 0;
        hadToolCallEviction_ = false;
        sessionId_ = result["sessionId"].toString();
        if (sessionId_.isEmpty()) {
            failConnection("ACP agent returned an empty sessionId.");
            return;
        }
        if (cancelTimer_.isActive()) {
            // Do not declare the replacement session Ready while an old cancelled prompt is still
            // unsettled and its cancel timer could still terminate the connection.
            awaitingOldPromptCancel_ = true;
            return;
        }
        deadlineTimer_.stop();
        setState(AcpState::Ready);
        emit connected(agentName_.isEmpty() ? QString("ACP agent") : agentName_, sessionId_);
    } else if (kind == PendingKind::Prompt) {
        deadlineTimer_.stop();
        cancelling_ = false;
        cancelAllPendingPermissions(req.sessionId);
        setState(AcpState::Ready);
        emit promptFinished(result["stopReason"].toString("end_turn"));
    }
}

void AcpClient::handleNotification(const QString& method, const QJsonObject& params) {
    if (method != "session/update") return;
    const auto sessionVal = params["sessionId"];
    if (sessionId_.isEmpty() || !sessionVal.isString() || sessionVal.toString() != sessionId_) return;
    const auto update = params["update"].toObject();
    const QString kind = update.contains("sessionUpdate") ? update["sessionUpdate"].toString() : update["type"].toString();
    if (kind == "agent_message_chunk") {
        const QString text = extractContentText(update["content"]);
        if (!text.isEmpty()) emit messageChunk(text);
    } else if (kind == "agent_thought_chunk") {
        const QString text = extractContentText(update["content"]);
        if (!text.isEmpty()) emit thoughtChunk(text);
    } else if (kind == "tool_call" || kind == "tool_call_update") {
        constexpr qsizetype maxPermissionDetailChars = 128 * 1024;
        constexpr qsizetype maxTrackedTotalChars = 2 * 1024 * 1024; // ~4 MiB UTF-16 budget across all cached tool calls
        constexpr int maxTrackedToolCalls = 256;
        auto clampStr = [](const QString& s, qsizetype limit) {
            return s.size() > limit ? (s.left(limit) + "…") : s;
        };
        auto entrySize = [](const QString& idKey, const KnownToolCall& tc) {
            return idKey.size() + tc.title.size() + tc.kind.size() + tc.rawInput.size() + tc.content.size();
        };
        AcpToolCallUpdate call;
        call.toolCallId = clampStr(update["toolCallId"].toString(), 256);
        call.title = clampStr(update["title"].toString(), 256);
        call.kind = clampStr(update["kind"].toString(), 64);
        call.status = clampStr(update["status"].toString(), 64);
        const QString contentDetail = extractContentText(update["content"]);
        const QString rawInputDetail = update.contains("rawInput") ? summarizeJson(update["rawInput"]) : QString();
        call.detail = clampStr(contentDetail, 4096);
        if (call.detail.isEmpty() && update.contains("rawOutput")) call.detail = clampStr(summarizeJson(update["rawOutput"]), 4096);
        if (call.detail.isEmpty() && !rawInputDetail.isEmpty()) call.detail = clampStr(rawInputDetail, 4096);
        const QString rawToolCallId = update["toolCallId"].toString();
        if (!rawToolCallId.isEmpty() && rawToolCallId.size() <= 512) {
            const bool isNew = !knownToolCalls_.contains(rawToolCallId);
            auto& tracked = knownToolCalls_[rawToolCallId];
            if (!isNew) knownToolCallsBytes_ = std::max<qsizetype>(0, knownToolCallsBytes_ - entrySize(rawToolCallId, tracked));
            if (isNew) knownToolCallOrder_.append(rawToolCallId);
            if (!call.title.isEmpty()) tracked.title = call.title;
            if (!call.kind.isEmpty()) tracked.kind = call.kind;
            if (call.status == "completed" || call.status == "failed" || call.status == "cancelled")
                tracked.finished = true;
            if (!rawInputDetail.isEmpty()) {
                if (rawInputDetail.size() > maxPermissionDetailChars) {
                    tracked.rawInputTooLarge = true;
                    tracked.rawInput.clear();
                } else {
                    tracked.rawInputTooLarge = false;
                    tracked.rawInput = rawInputDetail;
                }
            }
            if (!contentDetail.isEmpty()) {
                if (contentDetail.size() > maxPermissionDetailChars) {
                    tracked.contentTooLarge = true;
                    tracked.content.clear();
                } else {
                    tracked.contentTooLarge = false;
                    tracked.content = contentDetail;
                }
            }
            knownToolCallsBytes_ += entrySize(rawToolCallId, tracked);
            while (knownToolCallOrder_.size() > maxTrackedToolCalls ||
                   (knownToolCallsBytes_ > maxTrackedTotalChars && knownToolCallOrder_.size() > 1)) {
                // Prefer evicting completed/failed tool calls before pending ones.
                qsizetype evictIdx = 0;
                for (qsizetype idx = 0; idx < knownToolCallOrder_.size() - 1; ++idx) {
                    const auto it = knownToolCalls_.constFind(knownToolCallOrder_[idx]);
                    if (it != knownToolCalls_.cend() && it->finished) { evictIdx = idx; break; }
                }
                const QString evictId = knownToolCallOrder_.takeAt(evictIdx);
                if (auto it = knownToolCalls_.find(evictId); it != knownToolCalls_.end()) {
                    if (!it->finished || !it->rawInput.isEmpty() || it->rawInputTooLarge)
                        hadToolCallEviction_ = true;
                    knownToolCallsBytes_ = std::max<qsizetype>(0, knownToolCallsBytes_ - entrySize(evictId, it.value()));
                    knownToolCalls_.erase(it);
                }
            }
        }
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
        constexpr int maxPendingPermissions = 8;
        constexpr int maxPermissionOptions = 8;
        const auto sessionVal = params["sessionId"];
        const QString reqSession = sessionVal.isString() ? sessionVal.toString() : QString();
        if (sessionId_.isEmpty() || reqSession.isEmpty() || reqSession != sessionId_ || cancelling_ ||
            pendingPermissions_.size() >= maxPendingPermissions) {
            sendResponse(id, {{"outcome", QJsonObject{{"outcome", "cancelled"}}}});
            return;
        }
        auto clamp = [](const QString& s, qsizetype limit) {
            return s.size() > limit ? (s.left(limit) + "…") : s;
        };
        constexpr qsizetype maxPermissionDetailChars = 128 * 1024;
        const auto toolCall = params["toolCall"].toObject();
        const QString rawToolCallId = toolCall["toolCallId"].toString();
        const auto trackedIt = rawToolCallId.isEmpty() ? knownToolCalls_.cend() : knownToolCalls_.constFind(rawToolCallId);
        const bool hasTracked = trackedIt != knownToolCalls_.cend();
        const QString reqTitle = toolCall["title"].toString();
        const QString reqRawInput = summarizeJson(toolCall["rawInput"]);
        const QString reqContent = extractContentText(toolCall["content"]);
        // Fail closed if the request omits arguments/details and its toolCallId is missing from cache
        // (either never provided or evicted), so the operator is never asked to Allow an opaque operation.
        if (!hasTracked && reqRawInput.isEmpty() &&
            ((!rawToolCallId.isEmpty() && hadToolCallEviction_) || (reqContent.isEmpty() && reqTitle.isEmpty()))) {
            sendResponse(id, {{"outcome", QJsonObject{{"outcome", "cancelled"}}}});
            emit stderrReceived(QString("Auto-cancelled permission request “%1” because its operation arguments were missing or evicted from cache.\n")
                .arg(clamp(reqTitle.isEmpty() ? rawToolCallId : reqTitle, 128)));
            return;
        }
        QString resolvedTitle = reqTitle;
        if (resolvedTitle.isEmpty() && hasTracked) resolvedTitle = trackedIt->title;
        if (resolvedTitle.isEmpty()) resolvedTitle = "Agent permission request";
        QString resolvedKind = toolCall["kind"].toString();
        if (resolvedKind.isEmpty() && hasTracked) resolvedKind = trackedIt->kind;
        bool detailTooLarge = reqRawInput.size() > maxPermissionDetailChars || reqContent.size() > maxPermissionDetailChars;
        QString resolvedRawInput = reqRawInput;
        if (resolvedRawInput.isEmpty() && hasTracked) {
            if (trackedIt->rawInputTooLarge) detailTooLarge = true;
            else resolvedRawInput = trackedIt->rawInput;
        }
        QString resolvedContent = reqContent;
        if (resolvedContent.isEmpty() && hasTracked) {
            if (trackedIt->contentTooLarge && resolvedRawInput.isEmpty()) detailTooLarge = true;
            else resolvedContent = trackedIt->content;
        }
        QString fullDetail;
        if (!resolvedContent.isEmpty() && !resolvedRawInput.isEmpty() && resolvedContent != resolvedRawInput)
            fullDetail = resolvedContent + "\n\nArguments:\n" + resolvedRawInput;
        else if (!resolvedRawInput.isEmpty())
            fullDetail = resolvedRawInput;
        else
            fullDetail = resolvedContent;
        if (fullDetail.size() > maxPermissionDetailChars) detailTooLarge = true;
        if (detailTooLarge) {
            // Never present a truncated permission request that could hide arguments behind Allow.
            sendResponse(id, {{"outcome", QJsonObject{{"outcome", "cancelled"}}}});
            emit stderrReceived(QString("Auto-cancelled permission request “%1” because its arguments exceeded 128 KB.\n")
                .arg(clamp(resolvedTitle, 128)));
            return;
        }
        if (fullDetail.isEmpty()) {
            // Never offer Allow for a title-only or opaque permission request that provides no operation details.
            sendResponse(id, {{"outcome", QJsonObject{{"outcome", "cancelled"}}}});
            emit stderrReceived(QString("Auto-cancelled permission request “%1” because no operation arguments or content details were provided.\n")
                .arg(clamp(resolvedTitle, 128)));
            return;
        }
        AcpPermissionRequest request;
        request.requestKey = nextPermissionKey_++;
        request.toolCallId = clamp(rawToolCallId, 128);
        request.title = clamp(resolvedTitle, 256);
        request.kind = clamp(resolvedKind, 64);
        request.detail = std::move(fullDetail);
        for (const auto& val : params["options"].toArray()) {
            if (static_cast<int>(request.options.size()) >= maxPermissionOptions) break;
            const auto opt = val.toObject();
            const QString optionId = opt["optionId"].toString();
            if (!optionId.isEmpty()) {
                // Keep the opaque protocol optionId intact; only clamp display labels.
                request.options.push_back({optionId,
                    clamp(opt["name"].toString(optionId), 64),
                    clamp(opt["kind"].toString(), 64)});
            }
        }
        pendingPermissions_.insert(request.requestKey, {id, reqSession});
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
        // ACP v1 stdio transport schema omits a `type` discriminator field.
        const QString exe = mcp_.proxyExecutable.isEmpty() ? QCoreApplication::applicationFilePath() : mcp_.proxyExecutable;
        QJsonArray env;
        if (!mcp_.token.isEmpty()) env.append(QJsonObject{{"name", "SAV_MCP_TOKEN"}, {"value", mcp_.token}});
        servers.append(QJsonObject{
            {"name", mcp_.name},
            {"command", exe},
            {"args", QJsonArray{"--mcp-proxy", mcp_.url}},
            {"env", env}
        });
    }
    return servers;
}

void AcpClient::cancelAllPendingPermissions(const QString& onlySessionId) {
    const auto keys = pendingPermissions_.keys();
    for (int key : keys) {
        if (!onlySessionId.isEmpty() && pendingPermissions_[key].sessionId != onlySessionId) continue;
        respondPermission(key, {});
    }
}

void AcpClient::failConnection(const QString& message) {
    // Tear down any running child process (e.g. after incompatible protocolVersion) while preserving Error state.
    stopping_ = true;
    deadlineTimer_.stop();
    cancelTimer_.stop();
    cancelling_ = false;
    pending_.clear();
    pendingPermissions_.clear();
    knownToolCalls_.clear();
    knownToolCallOrder_.clear();
    knownToolCallsBytes_ = 0;
    hadToolCallEviction_ = false;
    stdoutBuffer_.clear();
    sessionId_.clear();
    teardownProcess(false);
    stopping_ = false;
    lastError_ = message;
    setState(AcpState::Error);
    emit errorOccurred(message);
}

}
