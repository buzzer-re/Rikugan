#include "common/agent/HostMcp.h"

#include <QJsonArray>
#include <QMetaObject>

#include "common/config/RikuganConfig.h"
#include "common/tools/ToolRegistry.h"
#include "common/util/Log.h"
#include "common/util/ModuleLifetime.h"

namespace rk {

namespace {

constexpr int kProbeTimeoutMs = 4000;
constexpr int kListTimeoutMs = 15000;
constexpr int kCallTimeoutMs = 10 * 60 * 1000;  // analysis updates can take long; Stop ends a call sooner
const char* const kCategory = "host_mcp";
// Binary Ninja's tools carry pages of manual (~54 KB in all) that would go to the model with every turn; the name,
// a line of description and the schema are enough to call them.
constexpr qsizetype kToolDescriptionMax = 140;
constexpr qsizetype kParamDescriptionMax = 60;

}  // namespace

QString HostMcp::clipDescription(const QString& text, qsizetype max) {
    if (text.size() <= max) return text;
    const QString t = text.simplified();
    if (t.size() <= max) return t;
    qsizetype cut = t.lastIndexOf(QLatin1Char(' '), max);
    if (cut < max / 2) cut = max;  // no word boundary in a useful place: cut inside the word
    QString out = t.left(cut);
    while (!out.isEmpty() && (out.back().isSpace() || QStringLiteral(",;:.-(").contains(out.back()))) out.chop(1);
    return out + QChar(0x2026);
}

HostMcp& HostMcp::instance() {
    static HostMcp h;
    return h;
}

void HostMcp::setHooks(Hooks hooks) {
    std::lock_guard<std::mutex> g(mu_);
    hooks_ = std::move(hooks);
}

bool HostMcp::supported() const {
    std::lock_guard<std::mutex> g(mu_);
    return bool(hooks_.locate);
}

QString HostMcp::serverName() const {
    std::lock_guard<std::mutex> g(mu_);
    return hooks_.serverName;
}

void HostMcp::setUrlOverride(const QString& url) {
    std::lock_guard<std::mutex> g(mu_);
    urlOverride_ = url.trimmed();
}

HostMcp::Location HostMcp::locate() const {
    std::function<Location()> fn;
    QString url;
    {
        std::lock_guard<std::mutex> g(mu_);
        fn = hooks_.locate;
        url = urlOverride_;
    }
    Location loc;
    if (!fn) {
        loc.problem = QStringLiteral("this host has no MCP server");
        return loc;
    }
    loc = fn();
    if (url.isEmpty()) return loc;
    // the address the user set wins over the host's settings (a port the system picks, a proxy); the token stays
    QString error;
    const std::optional<McpEndpoint> e = McpEndpoint::parse(url, loc.endpoint.token, &error);
    if (!e) {
        loc.problem = QStringLiteral("the MCP server address in Rikugan's settings is not usable: %1").arg(error);
        return loc;
    }
    loc.endpoint = *e;
    loc.problem.clear();
    return loc;
}

McpClient& HostMcp::client(const Location& loc) {
    if (!loc.problem.isEmpty()) throw ToolError(loc.problem);
    if (!client_ || !(client_->endpoint() == loc.endpoint)) client_ = std::make_unique<McpClient>(loc.endpoint);
    return *client_;
}

void HostMcp::probe(std::function<void(const Probe&)> done) {
    setUrlOverride(RikuganConfig::instance().hostMcpUrl);
    spawnThread([this, done = std::move(done)] {
        Probe p;
        try {
            std::lock_guard<std::mutex> calls(callMu_);
            const Location loc = locate();
            p.enabled = loc.enabled;
            p.url = loc.endpoint.url();
            McpClient& c = client(loc);
            c.initialize(nullptr, kProbeTimeoutMs);
            const QJsonArray tools = c.listTools(nullptr, kListTimeoutMs);
            std::lock_guard<std::mutex> g(mu_);
            tools_.clear();
            for (const QJsonValue& v : tools) {
                const QJsonObject t = v.toObject();
                const QString name = t.value(QStringLiteral("name")).toString();
                if (name.isEmpty() || (hooks_.hidden && hooks_.hidden(name))) continue;
                tools_.insert(name, t);
            }
            p.tools = int(tools_.size());
            p.ok = true;
        } catch (const ToolError& e) {
            p.error = e.message();
        }
        QMetaObject::invokeMethod(uiAnchor(), [done, p] { done(p); }, Qt::QueuedConnection);
    });
}

bool HostMcp::startServer() {
    std::function<bool()> start;
    {
        std::lock_guard<std::mutex> g(mu_);
        start = hooks_.start;
    }
    return start && start();
}

void HostMcp::install(ToolRegistry& registry) {
    std::lock_guard<std::mutex> g(mu_);
    for (auto it = tools_.cbegin(); it != tools_.cend(); ++it) {
        const QString& name = it.key();
        const QJsonObject& t = it.value();
        if (const ToolSpec* mine = registry.find(name); mine && mine->category != QLatin1String(kCategory)) {
            log(QStringLiteral("Rikugan: %1 MCP tool %2 has the name of a Rikugan tool, left out")
                    .arg(hooks_.serverName, name));
            continue;
        }
        const QJsonValue hint = t.value(QStringLiteral("annotations")).toObject().value(QStringLiteral("readOnlyHint"));
        const bool readOnly = hint.isBool() ? hint.toBool() : (hooks_.readOnly && hooks_.readOnly(t));
        ToolSpec spec;
        spec.name = name;
        spec.category = QString::fromLatin1(kCategory);
        spec.description = clipDescription(t.value(QStringLiteral("description")).toString(), kToolDescriptionMax);
        spec.inputSchema = t.value(QStringLiteral("inputSchema")).toObject();
        if (spec.inputSchema.isEmpty()) spec.inputSchema = QJsonObject{{"type", "object"}, {"properties", QJsonObject()}};
        QJsonObject props = spec.inputSchema.value(QStringLiteral("properties")).toObject();
        for (auto p = props.begin(); p != props.end(); ++p) {
            QJsonObject prop = p.value().toObject();
            const QJsonValue d = prop.value(QStringLiteral("description"));
            if (!d.isString()) continue;
            prop.insert(QStringLiteral("description"), clipDescription(d.toString(), kParamDescriptionMax));
            p.value() = prop;
        }
        if (spec.inputSchema.value(QStringLiteral("properties")).isObject())
            spec.inputSchema.insert(QStringLiteral("properties"), props);
        // a tool that may change the database is a mutation: approval, phase gate, cache flush, an undo record
        // that says it cannot be undone
        spec.flags = External | (readOnly ? 0u : uint32_t(Mutating));
        spec.fn = [name](ToolCall& call) { return HostMcp::instance().call(call.host(), name, call.args(), call.token().get()); };
        registry.add(std::move(spec));
        owned_.insert(name);
    }
}

bool HostMcp::owns(const QString& name) const {
    std::lock_guard<std::mutex> g(mu_);
    return owned_.contains(name);
}

bool HostMcp::offers(const QString& name) const {
    std::lock_guard<std::mutex> g(mu_);
    return tools_.contains(name);
}

int HostMcp::toolCount() const {
    std::lock_guard<std::mutex> g(mu_);
    return int(tools_.size());
}

ToolResult HostMcp::call(Host& host, const QString& name, const QJsonObject& args, const CancelToken* cancel) {
    std::function<void(Host&, McpClient&, const CancelToken*)> select;
    {
        std::lock_guard<std::mutex> g(mu_);
        select = hooks_.select;
    }
    std::lock_guard<std::mutex> calls(callMu_);
    McpClient& c = client(locate());
    if (select) select(host, c, cancel);
    const QJsonObject result = c.callTool(name, args, cancel, kCallTimeoutMs);
    const QString text = McpClient::resultText(result);
    if (result.value(QStringLiteral("isError")).toBool()) return ToolResult::error(QStringLiteral("Error: ") + text);
    return ToolResult::ok(text);
}

}  // namespace rk
