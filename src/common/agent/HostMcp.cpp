#include "common/agent/HostMcp.h"

#include <QJsonArray>
#include <QMetaObject>

#include "common/tools/ToolRegistry.h"
#include "common/util/Log.h"
#include "common/util/ModuleLifetime.h"

namespace rk {

namespace {

constexpr int kProbeTimeoutMs = 4000;
constexpr int kListTimeoutMs = 15000;
constexpr int kCallTimeoutMs = 10 * 60 * 1000;  // analysis updates can take long; Stop ends a call sooner
const char* const kCategory = "host_mcp";

}  // namespace

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

McpClient& HostMcp::client() {
    std::function<Location()> locate;
    {
        std::lock_guard<std::mutex> g(mu_);
        locate = hooks_.locate;
    }
    if (!locate) throw ToolError(QStringLiteral("this host has no MCP server"));
    const Location loc = locate();
    if (!loc.problem.isEmpty()) throw ToolError(loc.problem);
    if (!client_ || !(client_->endpoint() == loc.endpoint)) client_ = std::make_unique<McpClient>(loc.endpoint);
    return *client_;
}

void HostMcp::probe(std::function<void(const Probe&)> done) {
    spawnThread([this, done = std::move(done)] {
        Probe p;
        try {
            std::lock_guard<std::mutex> calls(callMu_);
            Location loc;
            {
                std::lock_guard<std::mutex> g(mu_);
                if (hooks_.locate) loc = hooks_.locate();
                else loc.problem = QStringLiteral("this host has no MCP server");
            }
            p.enabled = loc.enabled;
            p.url = loc.endpoint.url();
            if (!loc.problem.isEmpty()) throw ToolError(loc.problem);
            McpClient& c = client();
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
        spec.description = t.value(QStringLiteral("description")).toString();
        spec.inputSchema = t.value(QStringLiteral("inputSchema")).toObject();
        if (spec.inputSchema.isEmpty()) spec.inputSchema = QJsonObject{{"type", "object"}, {"properties", QJsonObject()}};
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
    McpClient& c = client();
    if (select) select(host, c, cancel);
    const QJsonObject result = c.callTool(name, args, cancel, kCallTimeoutMs);
    const QString text = McpClient::resultText(result);
    if (result.value(QStringLiteral("isError")).toBool()) return ToolResult::error(QStringLiteral("Error: ") + text);
    return ToolResult::ok(text);
}

}  // namespace rk
