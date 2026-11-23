#include "app/McpProxy.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
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

    auto maybeQuit = [&] {
        if (eof && inFlight == 0) app.quit();
    };

    auto handleLine = [&](const QByteArray& raw) {
        const QByteArray trimmed = raw.trimmed();
        if (trimmed.isEmpty()) return;
        QJsonParseError parseError;
        const auto doc = QJsonDocument::fromJson(trimmed, &parseError);
        const QJsonValue requestId = (parseError.error == QJsonParseError::NoError && doc.isObject())
            ? doc.object().value("id") : QJsonValue(QJsonValue::Undefined);

        QNetworkRequest request(url);
        request.setRawHeader("Content-Type", "application/json");
        request.setRawHeader("Accept", "application/json, text/event-stream");
        request.setRawHeader("MCP-Protocol-Version", "2025-11-25");
        if (!token.isEmpty()) request.setRawHeader("Authorization", "Bearer " + token);

        ++inFlight;
        auto* reply = network.post(request, trimmed);
        QObject::connect(reply, &QNetworkReply::finished, &app, [&, reply, requestId] {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray body = reply->readAll().trimmed();
            const auto netError = reply->error();
            const QString errorText = reply->errorString();
            reply->deleteLater();
            --inFlight;

            if (status == 202) {
                maybeQuit();
                return;
            }
            if (!body.isEmpty()) {
                QJsonParseError err;
                const auto responseDoc = QJsonDocument::fromJson(body, &err);
                if (err.error == QJsonParseError::NoError && !responseDoc.isNull()) {
                    writeLine(responseDoc.toJson(QJsonDocument::Compact));
                    maybeQuit();
                    return;
                }
            }
            if (netError != QNetworkReply::NoError || status >= 400) {
                writeError(requestId, -32000, status > 0
                    ? QString("MCP HTTP error %1: %2").arg(status).arg(errorText)
                    : QString("MCP proxy error: %1").arg(errorText));
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
