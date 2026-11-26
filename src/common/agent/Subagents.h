#pragma once
// Subagents: an isolated omc session that shares the parent chat's host tools, approvals and mutation log.
// omc's own `task` subagents cannot see host tools, so Rikugan runs its own.
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include "common/tools/Tool.h"

namespace rk {

struct ChatContext;
struct DbContext;
class OmcSession;

enum class SubagentStatus { Pending, Running, Completed, Failed, Cancelled };
QString subagentStatusName(SubagentStatus s);

// One line of a subagent's live log (the agents panel).
struct SubagentLogEntry {
    qint64 ms = 0;
    QString kind;     // tool | text | error
    QString tool;
    QString summary;  // tool: its main argument, then its result's first line; text: the reply's first lines
    bool isError = false;
    bool running = false;  // a tool call that has not ended yet
};

struct SubagentInfo {
    QString id;
    QString name;
    QString task;
    QString agentType = QStringLiteral("custom");  // custom | network_recon | report_writer
    QString category = QStringLiteral("general");  // general | bulk_rename
    SubagentStatus status = SubagentStatus::Pending;
    qint64 createdAt = 0, completedAt = 0;
    QString parentId;
    QString chatId;  // chat whose summary injection targets
    QString summary;
    int turnCount = 0;
    QStringList perks;
    QString sessionFile;
    // live progress
    QString currentTool, currentArgs;
    qint64 currentToolStartMs = 0;
    int toolCount = 0;
    qint64 tokens = 0;
    QStringList recentOutput;        // the newest assistant replies (first lines), at most 3
    QList<SubagentLogEntry> log;     // at most kMaxLog, newest last
    static constexpr int kMaxLog = 200;
};

// Every subagent of the process (the Agents tab). Thread-safe; observers are told about each change.
class SubagentRegistry {
public:
    static SubagentRegistry& instance();
    QString add(SubagentInfo info);  // returns the id (generated when empty)
    void update(const QString& id, const std::function<void(SubagentInfo&)>& fn);
    std::vector<SubagentInfo> all() const;
    std::optional<SubagentInfo> get(const QString& id) const;
    void removeFinished();
    // Cancels a running subagent (its session aborts).
    void cancel(const QString& id);
    // Cancels every running subagent of a chat (it closed), or of every chat when `chatId` is empty (unload).
    void cancelChat(const QString& chatId);
    void setCanceller(const QString& id, std::function<void()> fn);
    // Sends a steering message to a running subagent; false when it is not running.
    bool steer(const QString& id, const QString& text);
    void setSteerer(const QString& id, std::function<void(const QString&)> fn);
    // Observer: called (any thread) after each change, possibly many times a second while agents run: observers
    // coalesce on their side. Returns a handle for removeObserver.
    int addObserver(std::function<void()> fn);
    void removeObserver(int handle);

    // The agent's omc frames as they came (any thread), for its transcript ("Open chat"). Kept until the agent is
    // removed from the registry.
    void appendFrame(const QString& id, const std::string& json);
    std::vector<std::string> frames(const QString& id) const;
    // Follows one agent's frames: `snapshot` gets those so far and `fn` (any thread, must not block) each later one,
    // with nothing lost or repeated between the two.
    int observeFrames(const QString& id, std::function<void(const std::string& json)> fn, std::vector<std::string>* snapshot);
    void removeFrameObserver(int handle);

private:
    void notify();
    mutable std::mutex mu_;
    std::vector<SubagentInfo> items_;
    std::map<QString, std::function<void()>> cancellers_;
    std::map<QString, std::function<void(const QString&)>> steerers_;
    std::map<int, std::function<void()>> observers_;
    std::map<QString, std::vector<std::string>> frames_;
    std::map<int, std::pair<QString, std::function<void(const std::string&)>>> frameObservers_;  // agent id, callback
    int nextObserver_ = 1;
};

struct SubagentSpec {
    QString name;
    QString task;
    QString agentType = QStringLiteral("custom");
    QStringList perks;
    int maxTurns = 20;
    QString systemAddendum;    // prepended to the task (SubagentRunner.run_task system_addendum)
    QStringList extraTools;    // chat tools shown to the child (exploration_report)
    bool noTools = false;      // report_writer: no tools at all
    QString category = QStringLiteral("general");
    bool silent = false;       // only approvals and questions reach the parent chat
    // ask_database: work on this database (its host tools, read-only) instead of the parent's
    std::shared_ptr<DbContext> database;
    // a tool call of the chat (spawn_subagent, ask_database): a message the user types meanwhile ends it early,
    // returning what the agent has so far, so the chat's agent reads the message next
    bool yieldsToUser = false;
};

struct SubagentOutcome {
    QString status;  // completed | aborted | error | interrupted (yieldsToUser: the user sent a message)
    QString text;    // final assistant text
    QString error;
    QString id;
};

// Runs a subagent to completion on the calling (non-UI) thread. Cancelling `token` aborts it.
SubagentOutcome runSubagent(ChatContext& parent, const SubagentSpec& spec, const std::shared_ptr<CancelToken>& token);

// The text a preset/perk set adds to the task (prompts/agents/*).
QString subagentAddendum(const QString& agentType, const QStringList& perks);

// A chat's omc messages as plain text for the report writer, capped at 60,000 characters (the first 30% and the
// last 70%).
QString subagentTranscript(const QJsonArray& messages);
// get_messages of `session`, on the calling (non-UI) thread; empty when the session closed first.
QJsonArray fetchSessionMessages(const std::shared_ptr<OmcSession>& session);

}  // namespace rk
