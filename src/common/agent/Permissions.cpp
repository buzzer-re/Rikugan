#include "common/agent/Permissions.h"

#include <vector>

#include "common/config/RikuganConfig.h"
#include "common/omc/OmcSession.h"
#include "common/util/ModuleLifetime.h"

namespace rk {

namespace {

constexpr char kTitlePrefix[] = "Allow tool: ";

}  // namespace

std::optional<PermissionKind> permissionKindForTool(const QString& tool) {
    if (tool == QLatin1String("read") || tool == QLatin1String("grep") || tool == QLatin1String("glob"))
        return PermissionKind::Read;
    if (tool == QLatin1String("write")) return PermissionKind::Write;
    if (tool == QLatin1String("edit") || tool == QLatin1String("ast_edit")) return PermissionKind::Edit;
    if (tool == QLatin1String("bash")) return PermissionKind::Bash;
    return std::nullopt;
}

QString permissionKindName(PermissionKind k) {
    switch (k) {
    case PermissionKind::Read: return QStringLiteral("read");
    case PermissionKind::Write: return QStringLiteral("write");
    case PermissionKind::Edit: return QStringLiteral("edit");
    case PermissionKind::Bash: return QStringLiteral("bash");
    }
    return {};
}

void PermissionState::setSink(std::function<void(const QJsonObject&)> sink) {
    std::lock_guard<std::mutex> g(mu_);
    sink_ = std::move(sink);
}

bool PermissionState::granted(PermissionKind k) const {
    // the global switches are read live, so ticking them in Settings applies to every chat at once
    const RikuganConfig& cfg = RikuganConfig::instance();
    if (k == PermissionKind::Bash ? cfg.autoAllowCommands : cfg.autoAllowFiles) return true;
    std::lock_guard<std::mutex> g(mu_);
    return grants_.count(k) != 0;
}

void PermissionState::grant(PermissionKind k) {
    std::lock_guard<std::mutex> g(mu_);
    grants_.insert(k);
}

std::set<PermissionKind> PermissionState::chatGrants() const {
    std::lock_guard<std::mutex> g(mu_);
    return grants_;
}

bool PermissionState::intercept(const QJsonObject& frame, const std::shared_ptr<OmcSession>& session,
                                const QString& subagentId) {
    if (frame.value(QStringLiteral("method")).toString() != QLatin1String("select")) return false;
    const QString title = frame.value(QStringLiteral("title")).toString();
    if (!title.startsWith(QLatin1String(kTitlePrefix))) return false;
    const qsizetype nl = title.indexOf(QLatin1Char('\n'));
    const QString tool = title.mid(int(sizeof kTitlePrefix) - 1, nl < 0 ? -1 : nl - (qsizetype(sizeof kTitlePrefix) - 1)).trimmed();
    const std::optional<PermissionKind> kind = permissionKindForTool(tool);
    if (!kind) return false;  // a tool Rikugan does not gate: the generic dialog card handles it
    const QString id = frame.value(QStringLiteral("id")).toString();
    if (granted(*kind)) {
        respond(session, id, true);
        return true;
    }
    std::function<void(const QJsonObject&)> sink;
    {
        std::lock_guard<std::mutex> g(mu_);
        pending_[id] = Pending{*kind, tool, session};
        sink = sink_;
    }
    if (sink)
        sink(QJsonObject{{"type", "rikugan_permission"},
                         {"key", id},
                         {"tool", tool},
                         {"kind", permissionKindName(*kind)},
                         {"title", title.section(QLatin1Char('\n'), 0, 0)},
                         {"details", nl < 0 ? QString() : title.mid(nl + 1)},
                         {"subagentId", subagentId}});
    else
        answer(id, PermissionChoice::Deny);  // nobody can ask the user (headless chat)
    return true;
}

bool PermissionState::cancelRequest(const QString& omcId) {
    {
        std::lock_guard<std::mutex> g(mu_);
        if (!pending_.erase(omcId)) return false;
    }
    resolved(omcId, "cancelled");
    return true;
}

void PermissionState::answer(const QString& key, PermissionChoice c) {
    std::vector<std::pair<QString, Pending>> answered;  // (key, request) to respond to, with this one first
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = pending_.find(key);
        if (it == pending_.end()) return;
        const Pending p = it->second;
        pending_.erase(it);
        answered.emplace_back(key, p);
        if (c == PermissionChoice::AllowChat) {
            grants_.insert(p.kind);
            // parallel calls of the same kind wait on their own prompts: the grant answers them too
            for (auto o = pending_.begin(); o != pending_.end();) {
                if (o->second.kind == p.kind) {
                    answered.emplace_back(o->first, o->second);
                    o = pending_.erase(o);
                } else {
                    ++o;
                }
            }
        }
    }
    const bool approve = c != PermissionChoice::Deny;
    const char* outcome = c == PermissionChoice::Deny        ? "denied"
                          : c == PermissionChoice::AllowChat ? "allowed_chat"
                                                             : "allowed_once";
    for (const auto& [k, p] : answered) {
        respond(p.session, k, approve);
        resolved(k, outcome);
    }
}

void PermissionState::cancelAll() {
    std::map<QString, Pending> all;
    {
        std::lock_guard<std::mutex> g(mu_);
        all.swap(pending_);
    }
    for (const auto& [k, p] : all) {
        respond(p.session, k, false);
        resolved(k, "cancelled");
    }
}

int PermissionState::pendingCount() const {
    std::lock_guard<std::mutex> g(mu_);
    return int(pending_.size());
}

void PermissionState::respond(const std::weak_ptr<OmcSession>& session, const QString& omcId, bool approve) {
    // never from inside omc's event callback: the answer is sent on a thread of its own
    spawnThread([session, omcId, approve] {
        if (auto s = session.lock())
            s->sendControl(QJsonObject{{"type", "extension_ui_response"},
                                       {"id", omcId},
                                       {"value", approve ? QStringLiteral("Approve") : QStringLiteral("Deny")}});
    });
}

void PermissionState::resolved(const QString& key, const char* outcome) {
    std::function<void(const QJsonObject&)> sink;
    {
        std::lock_guard<std::mutex> g(mu_);
        sink = sink_;
    }
    if (sink) sink(QJsonObject{{"type", "rikugan_permission_resolved"}, {"key", key}, {"outcome", QLatin1String(outcome)}});
}

}  // namespace rk
