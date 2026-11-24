#include "app/McpProxy.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSet>
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
}

int runMcpProxy(int argc, char** argv, const QString& endpointUrl) {
    QCoreApplication app(argc, argv);
    const QUrl url(endpointUrl);
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https")) {
        std::cerr << "Invalid MCP endpoint URL\n";
        return 2;
    }
    const QByteArray token = qgetenv("SAV_MCP_TOKEN");
    QNetworkAccessManager network;
    int inFlight = 0;
    bool eof = false;
    QMap<QString, QPointer<QNetworkReply>> inflightReplies;
    QSet<QString> locallyCancelled;

    auto maybeQuit = [&] {
        if (eof && inFlight == 0) app.quit();
    };

    auto handleLine = [&](const QByteArray& raw) {
        const QByteArray trimmed = raw.trimmed();
        if (trimmed.isEmpty()) return;
        QJsonParseError parseError;
        const auto doc = QJsonDocument::fromJson(trimmed, &parseError);
        const QJsonObject msgObj = (parseError.error == QJsonParseError::NoError && doc.isObject())
            ? doc.object() : QJsonObject{};
        const QJsonValue requestId = msgObj.contains("id") ? msgObj.value("id") : QJsonValue(QJsonValue::Undefined);
        const QString idKey = requestIdKey(requestId);

        // Honor MCP notifications/cancelled locally as well as forwarding it to the server so
        // the original HTTP socket closes immediately and in-flight workers abort before publishing.
        if (msgObj["method"].toString() == "notifications/cancelled") {
            const QString targetKey = requestIdKey(msgObj["params"].toObject().value("requestId"));
            if (!targetKey.isEmpty()) {
                locallyCancelled.insert(targetKey);
                if (auto it = inflightReplies.find(targetKey); it != inflightReplies.end()) {
                    auto reply = it.value();
                    inflightReplies.erase(it);
                    if (reply) reply->abort();
                }
            }
        }

        QNetworkRequest request(url);
        request.setTransferTimeout(isLongFileToolCall(msgObj) ? (30 * 60 * 1000) : 60000);
        request.setRawHeader("Content-Type", "application/json");
        request.setRawHeader("Accept", "application/json, text/event-stream");
        request.setRawHeader("MCP-Protocol-Version", "2025-11-25");
        if (!token.isEmpty()) request.setRawHeader("Authorization", "Bearer " + token);

        ++inFlight;
        auto* reply = network.post(request, trimmed);
        if (!idKey.isEmpty()) inflightReplies.insert(idKey, reply);
        QObject::connect(reply, &QNetworkReply::finished, &app, [&, reply, requestId, idKey] {
            if (!idKey.isEmpty() && inflightReplies.value(idKey) == reply) inflightReplies.remove(idKey);
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray body = reply->readAll().trimmed();
            const auto netError = reply->error();
            const QString errorText = reply->errorString();
            reply->deleteLater();
            --inFlight;

            if (!idKey.isEmpty() && locallyCancelled.remove(idKey)) {
                maybeQuit();
                return;
            }

            const bool isRequest = !requestId.isUndefined();
            if (status == 202) {
                if (isRequest) writeError(requestId, -32000, "MCP server returned HTTP 202 without a response for request");
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
                        writeLine(responseDoc.toJson(QJsonDocument::Compact));
                        maybeQuit();
                        return;
                    }
                    writeError(requestId, -32000, "MCP server returned an invalid or mismatched JSON-RPC response");
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
            maybeQuit();
        });
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
            maybeQuit();
        }, Qt::QueuedConnection);
    });

    const int exitCode = app.exec();
    if (reader.joinable()) reader.join();
    return exitCode;
}
}
