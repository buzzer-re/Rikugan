#pragma once
// A client for one MCP server over streamable HTTP: JSON-RPC requests POSTed to the server's URL, answered with
// JSON or an event stream. Plain http only: it talks to the host's own server on this machine (Binary Ninja's).
//
// Calls block the calling thread (a tool worker or a probe thread, never the UI thread), poll `cancel` and give up
// at `timeoutMs`. Failures throw ToolError. One call at a time: the caller serializes.
#include <optional>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include "common/tools/Tool.h"

namespace rk {

struct McpEndpoint {
    QString host;
    quint16 port = 0;
    QString path = QStringLiteral("/");
    QString token;  // sent as "Authorization: Bearer <token>" when set

    // "http://host:port/path". nullopt, with *error, for anything else.
    static std::optional<McpEndpoint> parse(const QString& url, const QString& token, QString* error);
    QString url() const;
    bool operator==(const McpEndpoint&) const = default;
};

class McpClient {
public:
    explicit McpClient(McpEndpoint endpoint) : endpoint_(std::move(endpoint)) {}
    const McpEndpoint& endpoint() const { return endpoint_; }

    // initialize, then notifications/initialized. Returns the initialize result (serverInfo, instructions ...).
    QJsonObject initialize(const CancelToken* cancel, int timeoutMs);
    // tools/list, every page.
    QJsonArray listTools(const CancelToken* cancel, int timeoutMs);
    // tools/call: the result object ({content, isError, structuredContent}).
    QJsonObject callTool(const QString& name, const QJsonObject& arguments, const CancelToken* cancel, int timeoutMs);
    // The text of a tools/call result: its text blocks, or structuredContent as JSON when it has none.
    static QString resultText(const QJsonObject& result);

private:
    struct Reply {
        int status = 0;
        QByteArray contentType;
        QByteArray sessionId;
        QByteArray body;  // de-chunked
    };
    // One JSON-RPC request. A server that forgot the session (restarted) gets a new one, once.
    QJsonObject request(const QString& method, const QJsonObject& params, const CancelToken* cancel, int timeoutMs);
    QJsonObject send(const QString& method, const QJsonObject& params, const CancelToken* cancel, qint64 deadline,
                     int* status);
    Reply post(const QByteArray& json, qint64 id, const CancelToken* cancel, qint64 deadline) const;

    McpEndpoint endpoint_;
    QByteArray sessionId_;
    QByteArray version_;
    bool initialized_ = false;
    qint64 nextId_ = 1;
};

// One plain-http GET (on this machine: a login's callback server). Its status code; failures throw ToolError.
int httpGet(const QString& url, int timeoutMs);

}  // namespace rk
