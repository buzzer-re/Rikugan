#pragma once
// Disk and shell tool approvals. omc asks before every read/write/edit/bash call (the managed config sets those tools
// to "prompt"); Rikugan answers from the chat's grants: the first call of a kind asks the user, "Allow for this chat"
// remembers that kind for the chat and its subagents, and the global settings (auto_allow_files,
// auto_allow_commands) skip asking altogether.
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>

#include <QJsonObject>
#include <QString>

namespace rk {

class OmcSession;

enum class PermissionKind { Read, Write, Edit, Bash };
enum class PermissionChoice { AllowOnce, AllowChat, Deny };

// read/grep/glob -> Read, write -> Write, edit/ast_edit -> Edit, bash -> Bash; other tools need no permission.
std::optional<PermissionKind> permissionKindForTool(const QString& tool);
QString permissionKindName(PermissionKind k);  // "read" | "write" | "edit" | "bash"

// One per chat, shared with the chat's subagents. Thread-safe.
class PermissionState {
public:
    // Where requests and resolutions go (the chat's EventQueue): {"type":"rikugan_permission", ...} and
    // {"type":"rikugan_permission_resolved", ...}. Set once by the chat.
    void setSink(std::function<void(const QJsonObject&)> sink);

    // The chat grant, or the global setting for that kind.
    bool granted(PermissionKind k) const;
    void grant(PermissionKind k);
    std::set<PermissionKind> chatGrants() const;

    // An omc extension_ui_request. Called on an omc event thread; never blocks and never sends to omc from that
    // thread. Returns true when the frame was a tool approval and has been handled (answered from a grant, or
    // pending and announced to the UI), false for any other dialog.
    bool intercept(const QJsonObject& frame, const std::shared_ptr<OmcSession>& session, const QString& subagentId);
    // An omc `cancel {targetId}`: returns true when it closed a pending approval.
    bool cancelRequest(const QString& omcId);

    // UI thread: the user's answer. AllowChat grants the kind and answers every pending request of that kind.
    void answer(const QString& key, PermissionChoice c);
    // Stop / session closing: every pending request is denied and resolved as cancelled.
    void cancelAll();

    int pendingCount() const;

private:
    struct Pending {
        PermissionKind kind;
        QString tool;
        std::weak_ptr<OmcSession> session;
    };
    void respond(const std::weak_ptr<OmcSession>& session, const QString& omcId, bool approve);
    void resolved(const QString& key, const char* outcome);

    mutable std::mutex mu_;
    std::set<PermissionKind> grants_;
    std::map<QString, Pending> pending_;  // key = omc request id
    std::function<void(const QJsonObject&)> sink_;
};

}  // namespace rk
