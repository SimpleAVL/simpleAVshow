#include "app/McpProxy.h"
#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSet>
#include <QUuid>
#include <deque>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

namespace sav {
namespace {
void writeLine(const QByteArray& bytes) {
    std::cout.write(bytes.constData(), static_cast<std::streamsize>(bytes.size()));
    std::cout.put('\n');
    std::cout.flush();
}
void writeError(const QJsonValue& id, int code, const QString& message) {
    if (id.isUndefined()) return;
    const QJsonObject payload{{"jsonrpc", "2.0"}, {"id", id},
        {"error", QJsonObject{{"code", code}, {"message", message}}}};
    writeLine(QJsonDocument(payload).toJson(QJsonDocument::Compact));
}
QString requestIdKey(const QJsonValue& id) {
    if (id.isString()) return "s:" + id.toString();
    if (id.isDouble()) return "n:" + QString::number(id.toDouble(), 'f', 0);
    return {};
}
bool isLongFileToolCall(const QJsonObject& obj) {
    if (obj["method"].toString() != "tools/call") return false;
    const QString name = obj["params"].toObject()["name"].toString();
    return name == "project_action" || name == "relink_media" || name == "import_media";
}
bool isMutatingToolCall(const QJsonObject& obj) {
    if (obj["method"].toString() != "tools/call") return false;
    const QString name = obj["params"].toObject()["name"].toString();
    return name == "apply_edits" || name == "history" || name == "seek_and_select" ||
        name == "import_media" || name == "commit_import" || name == "relink_media" ||
        name == "configure_outputs" || name == "configure_remote" ||
        name == "project_action" || name == "transport_control";
}
bool isOrderedControlCall(const QJsonObject& obj) {
    if (obj["method"].toString() != "tools/call") return false;
    const QString name = obj["params"].toObject()["name"].toString();
    return name == "transport_control" || name == "seek_and_select";
}
}

int runMcpProxy(int argc, char** argv, const QString& endpointUrl) {
    QCoreApplication app(argc, argv);
    const QUrl url(endpointUrl);
    auto isLoopbackHost = [](const QString& host) {
        if (host.compare("localhost", Qt::CaseInsensitive) == 0) return true;
        QHostAddress addr;
        return addr.setAddress(host) && addr.isLoopback();
    };
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https") || !isLoopbackHost(url.host())) {
        std::cerr << "Invalid MCP endpoint URL (only loopback http/https endpoints are allowed)\n";
        return 2;
    }
    const QByteArray token = qgetenv("SAV_MCP_TOKEN");
    // Server-issued Mcp-Session-Id and negotiated MCP-Protocol-Version captured from initialize response.
    QByteArray sessionId;
    QByteArray initPayload;
    QByteArray protocolVersion = "2025-11-25";
    QNetworkAccessManager network;
    int inFlight = 0;
    bool eof = false;
    struct InflightEntry {
        QPointer<QNetworkReply> reply;
        bool mutating = false;
    };
    QMap<QString, InflightEntry> inflightReplies;
    QSet<QString> locallyCancelled;
    bool reinitializing = false;
    bool sessionInvalid = false;
    quint64 sessionGeneration = 0;
    struct PendingRequest {
        QByteArray trimmed;
        QJsonObject msgObj;
        QJsonValue requestId;
        QString idKey;
        bool isRetry = false;
    };
    std::deque<PendingRequest> deferredQueue;
    std::deque<PendingRequest> controlQueue;
    bool controlInFlight = false;

    auto maybeQuit = [&] {
        if (eof && inFlight == 0 && !reinitializing && deferredQueue.empty() && controlQueue.empty()) app.quit();
    };

    std::function<void(const QByteArray&, const QJsonObject&, const QJsonValue&, const QString&, bool)> sendRequest;
    std::function<void()> startReinit;
    std::function<void()> pumpControlQueue;

    pumpControlQueue = [&] {
        if (controlInFlight || reinitializing) return;
        while (!controlQueue.empty()) {
            PendingRequest item = std::move(controlQueue.front());
            controlQueue.pop_front();
            if (!item.idKey.isEmpty() && locallyCancelled.remove(item.idKey)) continue;
            if (eof && isMutatingToolCall(item.msgObj)) continue;
            if (sessionInvalid) {
                writeError(item.requestId, -32001, "MCP session expired and reinitialization failed; reinitialize before sending further requests");
                continue;
            }
            controlInFlight = true;
            sendRequest(item.trimmed, item.msgObj, item.requestId, item.idKey, item.isRetry);
            return;
        }
        maybeQuit();
    };

    auto finishReinit = [&](quint64 reinitStartGen, bool ok, const QByteArray& establishedSid = {}, const QByteArray& establishedProto = {}) {
        reinitializing = false;
        const bool superseded = sessionGeneration != reinitStartGen;
        if (!superseded) {
            if (ok) {
                sessionId = establishedSid;
                if (!establishedProto.isEmpty()) protocolVersion = establishedProto;
                sessionInvalid = false;
                ++sessionGeneration;
            } else {
                sessionId.clear();
                sessionInvalid = true;
                ++sessionGeneration;
            }
        }
        const bool usable = superseded ? (!sessionInvalid && !sessionId.isEmpty()) : ok;
        auto queued = std::move(deferredQueue);
        deferredQueue.clear();
        for (auto& item : queued) {
            if (!item.idKey.isEmpty() && locallyCancelled.remove(item.idKey)) continue;
            if (eof && isMutatingToolCall(item.msgObj)) continue;
            if (usable) sendRequest(item.trimmed, item.msgObj, item.requestId, item.idKey, item.isRetry);
            else writeError(item.requestId, -32001, "MCP session expired and reinitialization failed");
        }
        pumpControlQueue();
        maybeQuit();
    };

    startReinit = [&] {
        if (reinitializing || initPayload.isEmpty()) return;
        reinitializing = true;
        const quint64 reinitStartGen = sessionGeneration;
        QNetworkRequest reinitReq(url);
        reinitReq.setTransferTimeout(15000);
        reinitReq.setRawHeader("Content-Type", "application/json");
        reinitReq.setRawHeader("Accept", "application/json, text/event-stream");
        reinitReq.setRawHeader("MCP-Protocol-Version", protocolVersion);
        if (!token.isEmpty()) reinitReq.setRawHeader("Authorization", "Bearer " + token);
        ++inFlight;
        auto* reinitReply = network.post(reinitReq, initPayload);
        QObject::connect(reinitReply, &QNetworkReply::finished, &app, [&, reinitReply, reinitStartGen] {
            const QByteArray newSid = reinitReply->rawHeader("Mcp-Session-Id").trimmed();
            const int reinitStatus = reinitReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray reinitBody = reinitReply->readAll().trimmed();
            reinitReply->deleteLater();
            --inFlight;
            if (reinitStatus >= 200 && reinitStatus < 300 && !newSid.isEmpty()) {
                const auto reinitDoc = QJsonDocument::fromJson(reinitBody);
                const QString negotiated = reinitDoc.object()["result"].toObject()["protocolVersion"].toString().trimmed();
                const QByteArray nextProto = negotiated.isEmpty() ? protocolVersion : negotiated.toUtf8();
                // Complete the MCP lifecycle handshake with notifications/initialized BEFORE publishing
                // newSid to sessionId so a failed notification never leaves a half-initialized session active.
                QNetworkRequest notifReq(url);
                notifReq.setTransferTimeout(15000);
                notifReq.setRawHeader("Content-Type", "application/json");
                notifReq.setRawHeader("Accept", "application/json, text/event-stream");
                notifReq.setRawHeader("MCP-Protocol-Version", nextProto);
                notifReq.setRawHeader("Mcp-Session-Id", newSid);
                if (!token.isEmpty()) notifReq.setRawHeader("Authorization", "Bearer " + token);
                ++inFlight;
                auto* notifReply = network.post(notifReq, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
                QObject::connect(notifReply, &QNetworkReply::finished, &app, [&, notifReply, reinitStartGen, newSid, nextProto] {
                    const int notifStatus = notifReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    const auto notifErr = notifReply->error();
                    notifReply->deleteLater();
                    --inFlight;
                    finishReinit(reinitStartGen, notifErr == QNetworkReply::NoError && notifStatus >= 200 && notifStatus < 300, newSid, nextProto);
                });
            } else {
                finishReinit(reinitStartGen, false);
            }
        });
    };

    sendRequest = [&](const QByteArray& trimmed, const QJsonObject& msgObj, const QJsonValue& requestId, const QString& idKey, bool isRetry) {
        QNetworkRequest request(url);
        request.setTransferTimeout(isLongFileToolCall(msgObj) ? (30 * 60 * 1000) : 60000);
        request.setRawHeader("Content-Type", "application/json");
        request.setRawHeader("Accept", "application/json, text/event-stream");
        request.setRawHeader("MCP-Protocol-Version", protocolVersion);
        const QByteArray sentSession = sessionId;
        const quint64 sentGeneration = sessionGeneration;
        if (!sentSession.isEmpty() && msgObj["method"].toString() != "initialize")
            request.setRawHeader("Mcp-Session-Id", sentSession);
        if (!token.isEmpty()) request.setRawHeader("Authorization", "Bearer " + token);

        ++inFlight;
        const bool orderedControl = isOrderedControlCall(msgObj);
        auto* reply = network.post(request, trimmed);
        if (!idKey.isEmpty()) inflightReplies.insert(idKey, {reply, isMutatingToolCall(msgObj)});
        QObject::connect(reply, &QNetworkReply::finished, &app, [&, reply, trimmed, msgObj, requestId, idKey, sentSession, sentGeneration, isRetry, orderedControl] {
            if (!idKey.isEmpty() && inflightReplies.value(idKey).reply == reply) inflightReplies.remove(idKey);
            const QByteArray serverSession = reply->rawHeader("Mcp-Session-Id").trimmed();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray body = reply->readAll().trimmed();
            const auto netError = reply->error();
            const QString errorText = reply->errorString();
            reply->deleteLater();
            --inFlight;
            auto finishControlTurn = [&] {
                if (orderedControl) {
                    controlInFlight = false;
                    pumpControlQueue();
                }
            };
            const bool isInit = msgObj["method"].toString() == "initialize";
            if (netError == QNetworkReply::NoError && status >= 200 && status < 300 && !serverSession.isEmpty() &&
                (isInit || sentGeneration == sessionGeneration)) {
                if (sessionId != serverSession) {
                    sessionId = serverSession;
                    ++sessionGeneration;
                }
            }

            if (!idKey.isEmpty() && locallyCancelled.remove(idKey)) {
                finishControlTurn();
                maybeQuit();
                return;
            }

            // Per MCP Streamable HTTP session management, HTTP 404 for an expired/evicted Mcp-Session-Id
            // drops the stale session ID and reinitializes once while holding newer requests in order.
            // Never automatically replay mutating tool calls (e.g. Play) so an expired mutation cannot
            // execute out-of-order after a newer Stop All or edit.
            // Only replies from the current session generation may invalidate or trigger recovery.
            if (status == 404 && !sentSession.isEmpty() && !isInit) {
                const bool mutating = isMutatingToolCall(msgObj);
                if (sentGeneration != sessionGeneration) {
                    // Late 404 from an already-superseded session: never disturb the active replacement session.
                    if (!isRetry && !mutating) {
                        if (reinitializing) {
                            deferredQueue.push_back({trimmed, msgObj, requestId, idKey, true});
                            finishControlTurn();
                            return;
                        }
                        if (!sessionInvalid && !sessionId.isEmpty()) {
                            sendRequest(trimmed, msgObj, requestId, idKey, true);
                            finishControlTurn();
                            return;
                        }
                    }
                    writeError(requestId, -32001, "MCP session expired; retry after checking current state");
                    finishControlTurn();
                    maybeQuit();
                    return;
                }
                if (sessionId == sentSession) sessionId.clear();
                if (!isRetry && !initPayload.isEmpty()) {
                    if (mutating) {
                        if (!eof) startReinit();
                        else { sessionInvalid = true; ++sessionGeneration; }
                        writeError(requestId, -32001, "MCP session expired; reinitializing session — retry after checking current state");
                        finishControlTurn();
                        maybeQuit();
                        return;
                    }
                    deferredQueue.push_back({trimmed, msgObj, requestId, idKey, true});
                    startReinit();
                    finishControlTurn();
                    return;
                }
                sessionInvalid = true;
                ++sessionGeneration;
            }

            const bool isRequest = !requestId.isUndefined();
            if (status == 202) {
                if (isRequest) writeError(requestId, -32000, "MCP server returned HTTP 202 without a response for request");
                finishControlTurn();
                maybeQuit();
                return;
            }
            if (netError == QNetworkReply::NoError && status >= 200 && status < 300 && !body.isEmpty()) {
                QJsonParseError err;
                const auto responseDoc = QJsonDocument::fromJson(body, &err);
                if (err.error == QJsonParseError::NoError && responseDoc.isObject()) {
                    const auto obj = responseDoc.object();
                    const bool validEnvelope = obj["jsonrpc"] == "2.0" &&
                        (obj.contains("result") != obj.contains("error")) &&
                        (!isRequest || obj["id"] == requestId);
                    if (validEnvelope) {
                        if (isInit && obj.contains("result")) {
                            const QString negotiated = obj["result"].toObject()["protocolVersion"].toString().trimmed();
                            if (!negotiated.isEmpty()) protocolVersion = negotiated.toUtf8();
                            sessionInvalid = false;
                            pumpControlQueue();
                        }
                        writeLine(responseDoc.toJson(QJsonDocument::Compact));
                        finishControlTurn();
                        maybeQuit();
                        return;
                    }
                    writeError(requestId, -32000, "MCP server returned an invalid or mismatched JSON-RPC response");
                    finishControlTurn();
                    maybeQuit();
                    return;
                }
            }
            if (netError != QNetworkReply::NoError || status >= 400) {
                writeError(requestId, -32000, status > 0
                    ? QString("MCP HTTP error %1: %2").arg(status).arg(errorText)
                    : QString("MCP proxy error: %1").arg(errorText));
            } else {
                writeError(requestId, -32000, body.isEmpty()
                    ? QString("MCP server returned an empty response (HTTP %1)").arg(status)
                    : QString("MCP server returned invalid JSON (HTTP %1)").arg(status));
            }
            finishControlTurn();
            if (sessionInvalid) pumpControlQueue();
            maybeQuit();
        });
    };

    auto handleLine = [&](const QByteArray& raw) {
        const QByteArray trimmed = raw.trimmed();
        if (trimmed.isEmpty()) return;
        QJsonParseError parseError;
        const auto doc = QJsonDocument::fromJson(trimmed, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            writeError(QJsonValue(QJsonValue::Null), -32700, "Parse error");
            maybeQuit();
            return;
        }
        if (!doc.isObject()) {
            writeError(QJsonValue(QJsonValue::Null), -32600, "Invalid Request");
            maybeQuit();
            return;
        }
        const QJsonObject msgObj = doc.object();
        const bool hasId = msgObj.contains("id");
        const QJsonValue rawId = hasId ? msgObj.value("id") : QJsonValue(QJsonValue::Undefined);
        const bool hasMethod = msgObj.contains("method");
        const bool isClientResponse = msgObj["jsonrpc"].toString() == "2.0" && !hasMethod && hasId &&
            (rawId.isString() || rawId.isDouble() || rawId.isNull()) &&
            (msgObj.contains("result") != msgObj.contains("error"));
        if (isClientResponse) {
            // Client-to-server JSON-RPC response (e.g. reply to ping/roots/sampling):
            // forward cleanly without expecting a response body (HTTP 202 is valid) and never echo -32600.
            if (sessionInvalid) { maybeQuit(); return; }
            if (reinitializing) {
                deferredQueue.push_back({trimmed, msgObj, QJsonValue(QJsonValue::Undefined), {}, false});
                return;
            }
            sendRequest(trimmed, msgObj, QJsonValue(QJsonValue::Undefined), {}, false);
            return;
        }
        // Per MCP 2025-11-25 JSON-RPC schema, request IDs must be string or number (never null).
        const bool validRequestId = !hasId || rawId.isString() || rawId.isDouble();
        const QJsonValue methodVal = msgObj.value("method");
        if (msgObj["jsonrpc"].toString() != "2.0" || !methodVal.isString() || methodVal.toString().isEmpty() || !validRequestId) {
            writeError(validRequestId && hasId ? rawId : QJsonValue(QJsonValue::Null), -32600, "Invalid Request");
            maybeQuit();
            return;
        }
        const QJsonValue requestId = rawId;
        const QString idKey = requestIdKey(requestId);
        const QString method = methodVal.toString();
        if (method == "initialize") {
            initPayload = trimmed;
        } else if (sessionInvalid) {
            writeError(requestId, -32001, "MCP session expired and reinitialization failed; reinitialize before sending further requests");
            maybeQuit();
            return;
        }

        // Honor MCP notifications/cancelled locally as well as forwarding it to the server so
        // the original HTTP socket closes immediately and in-flight workers abort before publishing.
        if (method == "notifications/cancelled") {
            const QString targetKey = requestIdKey(msgObj["params"].toObject().value("requestId"));
            if (!targetKey.isEmpty()) {
                if (auto it = inflightReplies.find(targetKey); it != inflightReplies.end()) {
                    locallyCancelled.insert(targetKey);
                    auto reply = it.value().reply;
                    inflightReplies.erase(it);
                    if (reply) reply->abort();
                }
                const auto beforeDeferred = deferredQueue.size();
                std::erase_if(deferredQueue, [&](const PendingRequest& item) { return item.idKey == targetKey; });
                const auto beforeControl = controlQueue.size();
                std::erase_if(controlQueue, [&](const PendingRequest& item) { return item.idKey == targetKey; });
                if (deferredQueue.size() != beforeDeferred || controlQueue.size() != beforeControl) { maybeQuit(); return; }
            }
        }

        // Serialize transport/seek control calls in strict FIFO order so Play and Stop All can
        // never overtake each other across separate HTTP connections, while reads and cancels stay independent.
        if (isOrderedControlCall(msgObj)) {
            controlQueue.push_back({trimmed, msgObj, requestId, idKey, false});
            pumpControlQueue();
            return;
        }

        if (reinitializing && method != "initialize") {
            deferredQueue.push_back({trimmed, msgObj, requestId, idKey, false});
            return;
        }
        sendRequest(trimmed, msgObj, requestId, idKey, false);
    };

    std::thread reader([&] {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const QByteArray bytes(line.data(), static_cast<qsizetype>(line.size()));
            QMetaObject::invokeMethod(&app, [&, bytes] { handleLine(bytes); }, Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(&app, [&] {
            eof = true;
            // On stdin EOF (e.g. chat disconnected), abort any in-flight mutating tool calls so
            // they cannot publish to disk or modify the show after the agent disconnected.
            const auto keys = inflightReplies.keys();
            for (const auto& key : keys) {
                const auto entry = inflightReplies.value(key);
                if (entry.mutating && entry.reply) {
                    locallyCancelled.insert(key);
                    inflightReplies.remove(key);
                    entry.reply->abort();
                }
            }
            std::erase_if(deferredQueue, [](const PendingRequest& item) { return isMutatingToolCall(item.msgObj); });
            std::erase_if(controlQueue, [](const PendingRequest& item) { return isMutatingToolCall(item.msgObj); });
            maybeQuit();
        }, Qt::QueuedConnection);
    });

    const int exitCode = app.exec();
    if (reader.joinable()) reader.join();
    return exitCode;
}
}
