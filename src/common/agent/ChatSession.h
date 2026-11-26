#pragma once
// One chat on the UI thread: its omc session (opened lazily, resumed from its session file), the frames it
// shows, prompts queued while it works, and the cards tool workers ask the user through.
//
// Threads: every method and signal is UI-thread. Tool workers reach the user through the ChatUi bridge in the
// chat's context (askApproval / askUser block the worker, never the UI).
#include <functional>
#include <memory>
#include <map>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QStringList>

#include "common/agent/ChatContext.h"
#include "common/agent/ChatIndex.h"

namespace rk {

class EventQueue;
class OmcSession;
class SessionListener;
class ChatUiBridge;
struct DbContext;

class ChatSession : public QObject {
    Q_OBJECT
public:
    ChatSession(std::shared_ptr<DbContext> db, ChatEntry entry, QObject* parent = nullptr);
    ~ChatSession() override;

    const ChatEntry& entry() const { return entry_; }
    QString chatId() const { return entry_.chatId; }
    std::shared_ptr<ChatContext> context() const { return ctx_; }
    std::shared_ptr<DbContext> db() const { return db_; }
    bool busy() const { return busy_; }
    bool isOpen() const;
    // What waits for the agent: messages already handed to the running agent (it reads them at its next turn),
    // then the ones that wait for the run to end (commands).
    QStringList queued() const { return steering_ + queue_; }

    // Opens the omc session if it isn't (resuming the chat's session file) and replays the transcript.
    void ensureOpen();
    // Closes the omc session (idle chats beyond the live limit). Resumes on demand.
    void closeSession();
    // The database's tool set changed (its host MCP tools turned on or off). The system prompt only changes when a
    // session opens, so an idle session closes and resumes on demand. A busy one takes the new tools now and
    // closes when the run ends.
    void hostToolsChanged();
    // Settings were saved: the active profile, MCP servers and the rest apply once the session reopens. An idle chat
    // takes the profile now and its session closes (resumes on demand); a busy one when its run ends (tool workers
    // read the profile while it runs).
    void settingsChanged();

    // A prompt for the model (the per-prompt context block is added here). Queued while busy.
    // `images`: omc image blocks ({type: image, data, mimeType}) sent with the text. A message that waits in the queue
    // keeps its images until it goes out.
    void sendPrompt(const QString& text, const QJsonArray& images = {});
    // Runs `job` on the session worker as one busy unit (modes, /compact ...); `done` runs on the UI thread.
    void runJob(std::function<void(OmcSession&)> job, std::function<void()> done = {});
    // Same, with a token that stop() cancels (modes pass it to cards and subagents).
    void runJob(std::function<void(OmcSession&, const std::shared_ptr<CancelToken>&)> job, std::function<void()> done = {});
    // A chat line that did not come from the model (command output, notices).
    void postLocal(const QJsonObject& event);
    void stop();  // abort the running turn and drop the queue
    void removeQueued(int index);
    // Stops the running turn and sends queued()[index] next, the rest of the queue after it.
    void sendQueuedNow(int index);
    // A command that has to wait for the running turn (CommandRouter); queued like a prompt.
    void queueCommand(const QString& text);
    // How queued text starting with "/" is run when its turn comes (CommandRouter::submit). Unset: sent as a prompt.
    void setCommandRunner(std::function<void(const QString&)> run) { commandRunner_ = std::move(run); }

    // Fork into a new chat (same database): `done(newEntry)` on the UI thread, or `failed(error)`.
    void fork(std::function<void(const ChatEntry&)> done, std::function<void(const QString&)> failed);
    void setModel(const QString& model);
    // off | minimal | low | medium | high | xhigh | max | auto (get_state thinkingLevels lists the model's).
    void setThinkingLevel(const QString& level);
    // Renames the chat (chats.json and entryChanged).
    void setTitle(const QString& title);

    // Answers to cards.
    void answerApproval(int requestId, ApprovalChoice choice);
    void answerQuestion(int requestId, const UserAnswer& answer);  // also answers decisions
    // A disk / shell tool approval (rikugan_permission frame).
    void answerPermission(const QString& key, PermissionChoice choice);
    // extension_ui_request from omc: {"value": ...} / {"confirmed": bool} / {"cancelled": true}
    void answerUiRequest(const QString& id, const QJsonObject& answer);

    // The live-session limit (4 per process): closes least recently used idle chats.
    static void touch(ChatSession* s);

Q_SIGNALS:
    void frame(const QJsonObject& frame);        // omc frames and Rikugan-side events, in order
    void transcriptLoaded(const QJsonArray& messages);
    void busyChanged(bool busy);
    void queueChanged(const QStringList& queued);
    void entryChanged(const rk::ChatEntry& entry);
    void stateChanged(const QJsonObject& state);  // get_state data (model, contextUsage ...)
    void approvalRequested(int requestId, const rk::ApprovalRequest& request);
    void questionAsked(int requestId, const QString& question, const QStringList& options);
    void decisionRequested(int requestId, const QString& kind, const QJsonObject& payload);
    void cardCancelled(int requestId);
    void promptFinished(const QString& status, const QString& error, const QString& lastText);

protected:
    bool event(QEvent* e) override;

private:
    void openSession();
    void startNext();
    void reclaimSteering();
    void queueUpdated();
    void finishPrompt(const QString& status, const QString& error, const QString& text);
    void persistEntry();
    void changeSetting(const QJsonObject& request);
    void onState(const QJsonObject& state);
    void adoptTitle(const QString& title);  // omc's session name, unless the user renamed the chat
    void drain();

    std::shared_ptr<DbContext> db_;
    ChatEntry entry_;
    std::shared_ptr<ChatContext> ctx_;
    std::shared_ptr<ChatUiBridge> bridge_;
    std::shared_ptr<EventQueue> events_;
    std::shared_ptr<SessionListener> listener_;
    std::shared_ptr<OmcSession> session_;
    QStringList queue_;
    QStringList steering_;
    QList<QPair<QString, QJsonArray>> queuedImages_;  // the images of queued / steered messages, by text  // steered into omc, not taken by the agent yet (queue_update tells)
    std::function<void(const QString&)> commandRunner_;
    bool busy_ = false;
    bool transcriptPending_ = false;
    bool reopenWhenIdle_ = false;  // hostToolsChanged / settingsChanged while busy
    std::function<void(const QJsonObject&)> forkDone_;
    std::map<int, std::function<void()>> jobDone_;
    int nextJob_ = 1;
    std::shared_ptr<CancelToken> jobToken_;  // the running job's
    friend class ChatUiBridge;
};

}  // namespace rk

Q_DECLARE_METATYPE(rk::ApprovalRequest)
Q_DECLARE_METATYPE(rk::ChatEntry)
