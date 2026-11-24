#include "binja/host/BnHostMcp.h"

#include <map>

#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <action.h>
#include <binaryninjaapi.h>
#include <uicontext.h>

#include "binja/host/BnHost.h"
#include "common/agent/HostMcp.h"

using namespace BinaryNinja;

namespace rk::binja {

namespace {

const QString kStartAction = QStringLiteral("MCP\\Start Server");
constexpr int kSelectTimeoutMs = 30000;
// where Binary Ninja's MCP plugin listens by default ("MCP server listening at http://127.0.0.1:24642/mcp")
constexpr uint64_t kDefaultPort = 24642;

// The rows of the Markdown table Binary Ninja's list tools answer with, by column name.
QList<QHash<QString, QString>> tableRows(const QString& text) {
    QList<QHash<QString, QString>> rows;
    QStringList header;
    auto cells = [](QString line) {
        line = line.trimmed();
        line.replace(QStringLiteral("\\|"), QStringLiteral("\x01"));
        if (line.startsWith(QLatin1Char('|'))) line.remove(0, 1);
        if (line.endsWith(QLatin1Char('|'))) line.chop(1);
        QStringList out;
        for (QString c : line.split(QLatin1Char('|'))) out << c.replace(QLatin1Char('\x01'), QLatin1Char('|')).trimmed();
        return out;
    };
    for (const QString& line : text.split(QLatin1Char('\n'))) {
        if (!line.trimmed().startsWith(QLatin1Char('|'))) continue;
        const QStringList c = cells(line);
        if (header.isEmpty()) {
            header = c;
            continue;
        }
        if (!c.isEmpty() && c.first().startsWith(QLatin1String("---"))) continue;
        QHash<QString, QString> row;
        for (qsizetype i = 0; i < header.size() && i < c.size(); ++i) row.insert(header[i], c[i]);
        rows << row;
    }
    return rows;
}

QString listTool(McpClient& client, const char* tool, const CancelToken* cancel) {
    const QJsonObject r = client.callTool(QString::fromLatin1(tool), QJsonObject{{"limit", 1000}}, cancel, kSelectTimeoutMs);
    const QString text = McpClient::resultText(r);
    if (r.value(QStringLiteral("isError")).toBool())
        throw ToolError(QStringLiteral("Binary Ninja's MCP server could not list its files: %1").arg(text));
    return text;
}

// The server's handle for the binary view of `bv`, found among its open items by file session.
QString findView(McpClient& client, BinaryView& bv, const QString& session, const CancelToken* cancel) {
    QString item;
    for (const auto& row : tableRows(listTool(client, "bn_open_item_list", cancel)))
        if (row.value(QStringLiteral("sessionId")) == session) item = row.value(QStringLiteral("openItem"));
    if (item.isEmpty())
        throw ToolError(QStringLiteral(
            "Binary Ninja's MCP server does not list this database. It serves the files open in Binary Ninja's window, "
            "not databases opened in the background."));
    const QString type = QString::fromStdString(bv.GetTypeName());
    QString recommended;
    for (const auto& row : tableRows(listTool(client, "bn_binary_view_list", cancel))) {
        if (row.value(QStringLiteral("openItem")) != item) continue;
        if (row.value(QStringLiteral("viewType")) == type) return row.value(QStringLiteral("binaryView"));
        if (row.value(QStringLiteral("recommended")) == QLatin1String("true")) recommended = row.value(QStringLiteral("binaryView"));
    }
    if (recommended.isEmpty())
        throw ToolError(QStringLiteral("Binary Ninja's MCP server lists no %1 view of this database").arg(type));
    return recommended;
}

// Makes `handle` the server's active view. False when the handle no longer names this database (the server
// restarted and numbers its handles anew).
bool activate(McpClient& client, const QString& handle, const QString& session, const CancelToken* cancel) {
    const QJsonObject r = client.callTool(QStringLiteral("bn_binary_view_set_active"), QJsonObject{{"binaryView", handle}},
                                          cancel, kSelectTimeoutMs);
    if (r.value(QStringLiteral("isError")).toBool()) return false;
    // the session id is past 2^53: compare its digits, not a parsed double
    static const QRegularExpression re(QStringLiteral("\"sessionId\"\\s*:\\s*\"?(\\d+)"));
    const QRegularExpressionMatch m = re.match(McpClient::resultText(r));
    return m.hasMatch() && m.captured(1) == session;
}

bool readOnlyByName(const QString& name) {
    // Binary Ninja's tools carry no readOnlyHint. They are named bn_<subject>_<verb>: these verbs only read.
    static const char* const kReading[] = {"_list",    "_get",      "_get_active", "_info",          "_status",
                                           "_triage",  "_decompile", "_disassembly", "_il",          "_search",
                                           "_read",    "_at",        "_from",        "_to",          "_callers",
                                           "_callees", "_basic_blocks", "_complexity", "_stack_layout"};
    for (const char* v : kReading)
        if (name.endsWith(QLatin1String(v))) return true;
    return false;
}

}  // namespace

void installHostMcp() {
    HostMcp::Hooks h;
    h.serverName = QStringLiteral("Binary Ninja");
    h.locate = [] {
        // The server is probed whatever "ui.mcp.enabled" says: it can be started from Plugins → MCP without the
        // setting, and a setting turned on takes effect only after a restart. The settings give its address.
        HostMcp::Location loc;
        Ref<Settings> s = Settings::Instance();
        uint64_t port = kDefaultPort;
        QString path = QStringLiteral("/mcp");
        if (s->Contains("ui.mcp.port")) {
            loc.enabled = s->Get<bool>("ui.mcp.enabled");
            port = s->Get<uint64_t>("ui.mcp.port");
            if (s->Contains("ui.mcp.endpoint")) path = QString::fromStdString(s->Get<std::string>("ui.mcp.endpoint"));
            if (s->Contains("ui.mcp.token")) loc.endpoint.token = QString::fromStdString(s->Get<std::string>("ui.mcp.token"));
        }
        if (!path.startsWith(QLatin1Char('/'))) path.prepend(QLatin1Char('/'));
        loc.endpoint.host = QStringLiteral("127.0.0.1");
        loc.endpoint.port = quint16(port);
        loc.endpoint.path = path;
        if (port == 0 || port > 65535)
            loc.problem = QStringLiteral("Binary Ninja's MCP server listens on a port the system picks (ui.mcp.port is 0). "
                                         "Set a fixed port so Rikugan can find it.");
        return loc;
    };
    h.start = [] {
        if (!UIAction::isActionRegistered(kStartAction)) return false;
        UIContext* ctx = UIContext::activeContext();
        UIActionHandler* handler = ctx ? ctx->contentActionHandler() : nullptr;
        if (!handler) handler = UIActionHandler::globalActions();
        if (!handler) return false;
        handler->executeAction(kStartAction);
        return true;
    };
    h.select = [](Host& host, McpClient& client, const CancelToken* cancel) {
        auto* bn = dynamic_cast<BnHost*>(&host);
        if (!bn || !bn->view()) throw ToolError(QStringLiteral("no Binary Ninja database for this chat"));
        BinaryView& bv = *bn->view();
        const QString session = QString::number(bv.GetFile()->GetSessionId());
        // HostMcp serializes calls, so the cache needs no lock of its own
        static std::map<QString, QString> handles;  // file session -> view handle
        auto it = handles.find(session);
        if (it != handles.end() && activate(client, it->second, session, cancel)) return;
        const QString handle = findView(client, bv, session, cancel);
        if (!activate(client, handle, session, cancel)) {
            handles.erase(session);
            throw ToolError(QStringLiteral("Binary Ninja's MCP server could not select this database"));
        }
        handles[session] = handle;
    };
    h.readOnly = [](const QJsonObject& tool) { return readOnlyByName(tool.value(QStringLiteral("name")).toString()); };
    h.hidden = [](const QString& name) {
        // Rikugan selects the view for every call, and opens, saves and closes databases itself
        static const QSet<QString> kHidden{QStringLiteral("bn_binary_view_set_active"), QStringLiteral("bn_open_item_open"),
                                           QStringLiteral("bn_open_item_close"), QStringLiteral("bn_open_item_save"),
                                           QStringLiteral("bn_project_file_open")};
        return kHidden.contains(name);
    };
    HostMcp::instance().setHooks(std::move(h));
}

}  // namespace rk::binja
