#pragma once
#include <QObject>
#include <QString>
#include <QUrl>

namespace sav {
// Runs a headless stdio-to-Streamable-HTTP MCP bridge for ACP agents that only
// support stdio MCP servers. Reads the bearer token from SAV_MCP_TOKEN.
int runMcpProxy(int argc, char** argv, const QString& endpointUrl);
}
