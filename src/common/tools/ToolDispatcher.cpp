#include "common/tools/ToolDispatcher.h"

#include "common/util/CrashGuard.h"

#include <QJsonArray>

#include "common/agent/ChatContext.h"
#include "common/config/RikuganConfig.h"
#include "common/host/Host.h"
#include "common/omc/OmcSession.h"
#include "common/security/Sanitize.h"
#include "common/tools/Artifacts.h"
#include "common/tools/ResultCache.h"
#include "common/tools/ToolRegistry.h"
#include "common/util/Log.h"
#include "common/util/ModuleLifetime.h"

namespace rk {

namespace {

constexpr int kWorkers = 8;
constexpr size_t kMaxQueued = 64;
constexpr int kMaxBlocking = 16;

ToolResult runSpec(const ToolSpec& spec, ToolCall& call) {
    if (spec.has(MainThread)) return onMain(call.host(), spec.has(Mutating) ? MainAccess::Write : MainAccess::Read,
                                            [&] { return spec.fn(call); });
    return spec.fn(call);
}

QJsonObject resultFrame(const QString& id, const QString& text, bool isError, const QJsonObject& details) {
    QJsonObject result{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}}};
    if (!details.isEmpty()) result.insert(QStringLiteral("details"), details);
    return QJsonObject{{"type", "host_tool_result"}, {"id", id}, {"result", result}, {"isError", isError}};
}

// omc numbers host calls per session (host-1, host-2 ...): a call is known by its session and its id
QString tokenKey(const OmcSession* session, const QString& id) {
    return QString::number(quintptr(session), 16) + QLatin1Char(':') + id;
}

}  // namespace

ToolDispatcher& ToolDispatcher::instance() {
    static ToolDispatcher* d = new ToolDispatcher;  // never destroyed: workers may outlive static destruction order
    return *d;
}

ToolDispatcher::ToolDispatcher() {
    for (int i = 0; i < kWorkers; ++i) workers_.emplace_back([this] {
        crash::guardThread();
        workerLoop();
    });
}

ToolDispatcher::~ToolDispatcher() { shutdown(); }

void ToolDispatcher::shutdown() {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (stopping_) return;
        stopping_ = true;
        for (auto& [id, t] : tokens_) t->cancel();
    }
    cv_.notify_all();
    for (std::thread& t : workers_)
        if (t.joinable()) t.join();
}

void ToolDispatcher::dispatch(const std::shared_ptr<ChatContext>& chat, const std::shared_ptr<OmcSession>& session,
                              const QJsonObject& call) {
    const QString id = call.value(QStringLiteral("id")).toString();
    const QString key = tokenKey(session.get(), id);
    const QString toolName = call.value(QStringLiteral("toolName")).toString();
    auto token = std::make_shared<CancelToken>();
    const ToolSpec* spec = chat->registry ? chat->registry->find(toolName) : nullptr;
    std::unique_lock<std::mutex> lk(mu_);
    if (stopping_) {
        lk.unlock();
        session->sendControl(resultFrame(id, QStringLiteral("Error: Rikugan is shutting down"), true, {}));
        return;
    }
    if (spec && spec->has(Blocking)) {
        if (blockingRunning_ >= kMaxBlocking) {
            lk.unlock();
            session->sendControl(resultFrame(id, QStringLiteral("Error: too many tool calls at once"), true, {}));
            return;
        }
        ++blockingRunning_;
        tokens_[key] = token;
        lk.unlock();
        std::weak_ptr<OmcSession> weak = session;
        spawnThread([this, chat, weak, call, token, key] {
            runJob(chat, weak, call, token, key);
            std::lock_guard<std::mutex> g(mu_);
            --blockingRunning_;
        });
        return;
    }
    if (queue_.size() >= kMaxQueued) {
        lk.unlock();
        session->sendControl(resultFrame(id, QStringLiteral("Error: too many tool calls at once"), true, {}));
        return;
    }
    tokens_[key] = token;
    queue_.push_back(Job{chat, session, call, token, key});
    lk.unlock();
    cv_.notify_one();
}

void ToolDispatcher::cancel(const OmcSession* session, const QString& hostCallId) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = tokens_.find(tokenKey(session, hostCallId));
    if (it != tokens_.end()) it->second->cancel();
}

void ToolDispatcher::workerLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        runJob(job.chat, job.session, job.call, job.token, job.key);
    }
}

void ToolDispatcher::runJob(const std::shared_ptr<ChatContext>& chat, const std::weak_ptr<OmcSession>& weak,
                            const QJsonObject& call, const std::shared_ptr<CancelToken>& token, const QString& key) {
    const QString id = call.value(QStringLiteral("id")).toString();
    auto finish = [&] {
        std::lock_guard<std::mutex> g(mu_);
        tokens_.erase(key);
    };
    // omc already answered a cancelled call ("Tool call cancelled"): a queued one never runs
    if (token->cancelled()) {
        finish();
        return;
    }
    std::shared_ptr<OmcSession> session = weak.lock();
    if (!session) {
        finish();
        return;
    }
    auto update = [weak, id](const QString& partial) {
        if (auto s = weak.lock())
            s->sendControl(QJsonObject{{"type", "host_tool_update"},
                                       {"id", id},
                                       {"partialResult", QJsonObject{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", partial}}}}}}});
    };
    const ToolResult r = execute(*chat, call.value(QStringLiteral("toolName")).toString(),
                                 call.value(QStringLiteral("arguments")).toObject(),
                                 call.value(QStringLiteral("toolCallId")).toString(), token, update);
    finish();
    if (!token->cancelled()) session->sendControl(resultFrame(id, r.text, r.isError, r.details));
}

ToolResult ToolDispatcher::execute(ChatContext& chat, const QString& toolName, const QJsonObject& args,
                                   const QString& toolCallId, const std::shared_ptr<CancelToken>& token,
                                   const std::function<void(const QString&)>& update) {
    const ToolSpec* spec = chat.registry ? chat.registry->find(toolName) : nullptr;
    if (!spec || spec->has(Internal) || !chat.host)
        return ToolResult::error(QStringLiteral("Error: Unknown tool: %1").arg(toolName));
    if (chat.profile.deniedTools.contains(toolName))
        return ToolResult::error(QStringLiteral("Error: Tool '%1' is denied by the active profile.").arg(toolName));
    if (chat.phaseReadOnly() && (spec->has(Mutating) || spec->has(Script)))
        return ToolResult::error(
            QStringLiteral("Error: Mutating tools are disabled during the %1 phase.").arg(chat.phase()));
    if (spec->has(RequiresDecompiler) && !chat.host->hasDecompiler())
        return ToolResult::error(QStringLiteral("Error: Tool %1 unavailable — requires: hexrays").arg(toolName));

    if (spec->has(Script) && !chat.alwaysAllowScripts.load()) {
        if (!chat.ui) return ToolResult::error(QStringLiteral("Tool execution denied by user."));
        const ApprovalChoice c = chat.ui->askApproval(ApprovalRequest{ApprovalRequest::Script, toolName, args, {}}, token);
        if (c == ApprovalChoice::Deny) return ToolResult::error(QStringLiteral("Tool execution denied by user."));
        if (c == ApprovalChoice::AlwaysAllow) chat.alwaysAllowScripts = true;
    }
    const bool mutating = spec->has(Mutating);
    if (mutating && RikuganConfig::instance().approveMutations && chat.ui && !chat.alwaysAllowMutations.load()) {
        const ApprovalChoice c = chat.ui->askApproval(ApprovalRequest{ApprovalRequest::Mutation, toolName, args, {}}, token);
        if (c == ApprovalChoice::Deny) return ToolResult::error(QStringLiteral("Mutation denied by user."));
        if (c == ApprovalChoice::AlwaysAllow) chat.alwaysAllowMutations = true;
    }

    Host& host = *chat.host;
    ToolRegistry& registry = *chat.registry;
    MutationLog::Executor direct = [&](const QString& t, const QJsonObject& a) {
        return runDirect(host, registry, &chat, t, a, token);
    };
    // another program's tool (the host's MCP server) changes what Rikugan cannot read back
    QJsonObject pre;
    if (mutating && spec->has(External))
        pre.insert(QStringLiteral("_capture_failed"), QStringLiteral("the host's MCP server made this change"));
    else if (mutating)
        pre = MutationLog::capturePreState(toolName, args, direct);

    ToolResult result;
    std::optional<QString> cached = chat.cache ? chat.cache->get(toolName, args) : std::nullopt;
    if (cached) {
        result = ToolResult::ok(*cached);
    } else {
        ToolCall call(*spec, args, toolCallId, host, &chat, token, update);
        try {
            result = runSpec(*spec, call);
        } catch (const ToolError& e) {
            result = ToolResult::error(QStringLiteral("Error: %1").arg(e.message()));
        } catch (const std::exception& e) {
            result = ToolResult::error(QStringLiteral("Unexpected error: %1").arg(QString::fromUtf8(e.what())));
            log(QStringLiteral("Rikugan: tool %1 failed: %2").arg(toolName, QString::fromUtf8(e.what())));
        }
        if (!result.isError && chat.cache) {
            if (mutating) chat.cache->invalidate();
            else chat.cache->put(toolName, args, result.text);
        }
    }

    QJsonObject rk{{"tool", toolName}};
    if (mutating && !result.isError && MutationLog::mutationSucceeded(toolName, result.text)) {
        MutationRecord rec = MutationLog::buildReverseRecord(toolName, args, pre);
        rk.insert(QStringLiteral("mutation"), rec.toJson());
        chat.mutations->push(std::move(rec));
    }
    for (auto it = result.details.begin(); it != result.details.end(); ++it) rk.insert(it.key(), it.value());
    result.details = QJsonObject{{"rikugan", rk}};

    result.text = sanitize::stripInjectionMarkers(result.text);
    if (chat.profile.hasAnyIocFilter())
        result.text = sanitize::stripIocs(result.text, chat.profile.iocFilters, chat.profile.customFilterRules);
    result.text = capResult(result.text, spec->resultLimit, chat.artifactsDir, toolCallId);
    return result;
}

QString ToolDispatcher::runDirect(Host& host, ToolRegistry& registry, ChatContext* chat, const QString& tool,
                                  const QJsonObject& args, const std::shared_ptr<CancelToken>& token) {
    const ToolSpec* spec = registry.find(tool);
    if (!spec) throw ToolError(QStringLiteral("Unknown tool: %1").arg(tool));
    ToolCall call(*spec, args, QString(), host, chat, token ? token : std::make_shared<CancelToken>());
    const ToolResult r = runSpec(*spec, call);
    if (r.isError) throw ToolError(r.text);
    return r.text;
}

}  // namespace rk
