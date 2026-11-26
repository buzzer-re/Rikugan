#pragma once
// The host's own MCP server (Binary Ninja's). When the user agrees for a database, the server's tools replace
// Rikugan's analysis tools in that database's chats. They are host tools like the others: every call goes through
// the ToolDispatcher (profile, phase gate, approval of changes, injection-marker strip, IOC filter, result cap).
//
// One server serves the whole process and its active view is global, so calls take turns and each one first points
// the server at the calling chat's database (Hooks::select).
#include <functional>
#include <memory>
#include <mutex>

#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QString>

#include "common/mcp/McpClient.h"

namespace rk {

class Host;
class ToolRegistry;

class HostMcp {
public:
    struct Location {
        McpEndpoint endpoint;
        bool enabled = false;  // the host's setting that runs the server
        QString problem;       // why there is no address to try ("" when there is one)
    };
    // What the host plugin provides. Unset on hosts without an MCP server (IDA).
    struct Hooks {
        QString serverName;                // "Binary Ninja"
        std::function<Location()> locate;  // where the server listens, from the host's settings. Any thread.
        std::function<bool()> start;       // starts a server that is enabled but stopped. UI thread.
        // Points the server at `host`'s database. Called before every call, with the calls serialized. Throws ToolError.
        std::function<void(Host& host, McpClient& client, const CancelToken* cancel)> select;
        // A tool that changes nothing, for servers whose tools carry no readOnlyHint.
        std::function<bool(const QJsonObject& tool)> readOnly;
        // A tool the model never sees (the server's file and view switching: Rikugan selects the view itself).
        std::function<bool(const QString& name)> hidden;
    };
    struct Probe {
        bool ok = false;
        bool enabled = false;  // Location::enabled
        int tools = 0;
        QString url;
        QString error;
    };

    static HostMcp& instance();

    void setHooks(Hooks hooks);
    bool supported() const;
    QString serverName() const;

    // Connects and lists the server's tools on a thread of its own. UI thread (reads the address override from
    // RikuganConfig); `done` runs on the UI thread.
    void probe(std::function<void(const Probe&)> done);
    // Hooks::start. UI thread.
    bool startServer();
    // The server's address from Rikugan's settings (RikuganConfig::hostMcpUrl): when set it replaces the address the
    // host's settings give; the host's token stays. Any thread.
    void setUrlOverride(const QString& url);

    // Adds the tools the last probe found to a database's registry, their descriptions clipped. UI thread.
    void install(ToolRegistry& registry);
    // One of the server's tools (installed in some registry); one the model may see now.
    bool owns(const QString& name) const;
    bool offers(const QString& name) const;
    int toolCount() const;

    // tools/call on `host`'s database. Tool workers.
    ToolResult call(Host& host, const QString& name, const QJsonObject& args, const CancelToken* cancel);

    // `text` cut to about `max` characters at a word boundary, with "…" when cut (whitespace collapsed then).
    static QString clipDescription(const QString& text, qsizetype max);

private:
    HostMcp() = default;
    Location locate() const;                  // the hook's answer with the override applied
    McpClient& client(const Location& loc);  // callMu_ held; throws ToolError when there is no usable address

    mutable std::mutex mu_;  // hooks_, urlOverride_, tools_, owned_
    std::mutex callMu_;      // one exchange with the server at a time; client_
    Hooks hooks_;
    QString urlOverride_;
    std::unique_ptr<McpClient> client_;
    QHash<QString, QJsonObject> tools_;  // offered: name -> tools/list entry
    QSet<QString> owned_;
};

}  // namespace rk
