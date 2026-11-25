#pragma once
// What a tool call needs to know about the chat it runs for. Shared (shared_ptr) between the UI-thread
// ChatSession, the omc listener and tool workers; fields are set before the chat's session opens unless marked.
#include <atomic>
#include <functional>
#include <memory>
#include <string_view>

#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include "common/agent/Permissions.h"
#include "common/config/Profile.h"
#include "common/tools/MutationLog.h"
#include "common/tools/Tool.h"

namespace rk {

class Host;
class OmcSession;
class ResultCache;
class ToolRegistry;
struct DbContext;

enum class ApprovalChoice { Allow, AlwaysAllow, Deny };

struct ApprovalRequest {
    enum Kind { Script, Mutation };
    Kind kind = Script;
    QString toolName;
    QJsonObject arguments;
    QString reason;
};

struct UserAnswer {
    bool cancelled = false;
    QString text;
};

// Implemented by the chat's UI side. Every method may be called from any non-UI thread and blocks it until the
// user answers or `token` is cancelled (then the card closes and the answer is Deny / cancelled).
class ChatUi {
public:
    virtual ~ChatUi() = default;
    virtual ApprovalChoice askApproval(const ApprovalRequest& req, const std::shared_ptr<CancelToken>& token) = 0;
    virtual UserAnswer askUser(const QString& question, const QStringList& options,
                               const std::shared_ptr<CancelToken>& token) = 0;
    // A mode decision card: kind "plan" (payload {text, steps:[...]}; answers "approve", "regenerate",
    // "cancel" or "reject:<feedback>") or "save" (payload {patches:[{address, original_hex, new_hex, description}]};
    // answers "save" or "discard"). Cancelled -> UserAnswer.cancelled.
    virtual UserAnswer askDecision(const QString& kind, const QJsonObject& payload,
                                   const std::shared_ptr<CancelToken>& token) = 0;
    // A frame for the chat view that did not come from omc (subagent progress, mode events): {"type": ...}.
    virtual void postUiEvent(const QJsonObject& event) = 0;
};

struct ChatContext {
    QString chatId;
    QString dbId;
    std::shared_ptr<DbContext> db;  // host, registry, cache
    Host* host = nullptr;
    ToolRegistry* registry = nullptr;
    ResultCache* cache = nullptr;
    QString artifactsDir;
    AnalysisProfile profile;
    // shared with the chat's subagents
    std::shared_ptr<MutationLog> mutations = std::make_shared<MutationLog>();
    std::shared_ptr<ChatUi> ui;  // null for headless chats (bulk renamer workers)
    // "Allow for this chat" on a script card, on a database-change card (shared with subagents, both ways)
    std::atomic<bool> alwaysAllowScripts{false};
    std::atomic<bool> alwaysAllowMutations{false};
    // disk / shell tool approvals (shared with subagents)
    std::shared_ptr<PermissionState> permissions = std::make_shared<PermissionState>();
    // the chat's omc session (set when it opens; read by tool workers for sendControl)
    std::weak_ptr<OmcSession> session;

    // Mode phase: a read-only phase refuses Mutating and Script tools.
    void setPhase(const QString& name, bool readOnly);
    QString phase() const;
    bool phaseReadOnly() const;

    // Chat tools shown only in some states (exploration_report, research_note).
    void setToolShown(const QString& name, bool shown);
    bool toolShown(const QString& name) const;

    // Subagent depth: spawn_subagent is not offered to depth >= 1.
    int depth = 0;
    // Messages the user typed while the agent works, not taken by it yet (ChatSession keeps it current). A long
    // Rikugan tool (a subagent run) ends early when one comes, so the agent reads it at its next step.
    std::atomic<int> steerPending{0};
    // set by the mode driver while /explore, /modify or /research run (shared with their subagents)
    std::shared_ptr<class ModeState> mode;
    // the tool call that spawned this subagent context (Agents tab), empty for chats
    QString subagentId;

    // Observer of the chat's omc frame types (called on omc threads, must not block): the mode driver counts
    // turn_start with it to cap phases.
    void setFrameHook(std::function<void(std::string_view type)> fn);
    void notifyFrame(std::string_view type);

private:
    mutable std::mutex mu_;
    QString phase_;
    bool readOnly_ = false;
    QSet<QString> shown_;
    std::function<void(std::string_view)> frameHook_;
};

}  // namespace rk
