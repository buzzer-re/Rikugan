#pragma once
// One libomc session (omc_session_t) driven over omc's RPC protocol, plus the worker thread that owns it.
//
// Threads:
//   - every omc_session_new / omc_session_free / non-control omc_send runs on the session's worker
//     (post()); compact, session changes and abort_and_prompt block for up to a minute inside omc_send.
//   - control frames (host_tool_result, host_tool_update, extension_ui_response) and omc_abort go out from
//     any thread: omc handles them inline without waiting behind a running turn.
//   - omc calls the event callback on its own threads under its event lock: the callback only peeks at
//     "type", answers waiters and hands the raw frame to the listener, which must not block.
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

struct omc_session;

namespace rk {

struct OmcOptions {
    QString cwd, sessionDir, resume, model, thinking, systemPrompt;
    QStringList builtinTools;  // omc built-in allow-list; empty with noTools = none
    bool noSession = false, noTools = false, noSkills = false, noMcp = false;
    QJsonArray hostTools;  // set_host_tools "tools" sent right after the session opens
    // the session's name, set when it opens; empty: omc names it after its first exchange (a chat). Every session
    // that is not a chat sets one, so omc spends no model call naming it.
    QString title;
};

// Receives what omc sends. Called on omc threads; must return quickly and never call back into the
// session except for sendControl().
class OmcListener {
public:
    virtual ~OmcListener() = default;
    // Every frame except responses, ready and the host tool frames (raw JSON, one object).
    virtual void onFrame(std::string_view type, const std::string& json) = 0;
    virtual void onHostToolCall(const QJsonObject& call) = 0;     // {id, toolCallId, toolName, arguments}
    virtual void onHostToolCancel(const QString& targetId) = 0;
    // The session could not be created (worker thread).
    virtual void onOpenFailed(const QString& error) { (void)error; }
    // A text block of any tool's result before the model sees it (tool worker thread): the replacement, or nullopt
    // to keep it.
    virtual std::optional<QString> filterToolResult(const QString& tool, const QString& text) {
        (void)tool;
        (void)text;
        return std::nullopt;
    }
};

class OmcSession : public std::enable_shared_from_this<OmcSession> {
public:
    struct PromptOutcome {
        QString status;    // completed | aborted | error
        QString error;
        QString lastText;  // final assistant text
    };

    // Returns at once; the session is created on the worker. The listener is held until close().
    static std::shared_ptr<OmcSession> open(OmcOptions opts, std::shared_ptr<OmcListener> listener);
    ~OmcSession();

    OmcSession(const OmcSession&) = delete;
    OmcSession& operator=(const OmcSession&) = delete;

    // Runs `job` on the worker after the open attempt (also when it failed: isOpen() is false and requests fail
    // with openError()). Jobs queued after close() are dropped.
    void post(std::function<void(OmcSession&)> job);

    // ---- worker only ----
    // Sends `cmd` with a fresh "id" and waits for its response; {"success":false,"error":...} on timeout.
    QJsonObject request(QJsonObject cmd, int timeoutMs = 60000);
    // A prompt to completion: prompt -> prompt_result -> get_last_assistant_text. In goal mode, until the goal's
    // continuation turns settle (session_settled).
    PromptOutcome prompt(const QString& text, const QJsonArray& images = {});
    bool isOpen() const { return s_ != nullptr; }
    QString openError() const { return openError_; }
    // Refreshes the cached state (get_state); returns the state object.
    QJsonObject refreshState();

    // ---- any thread ----
    void sendControl(const QJsonObject& frame);
    void abort();
    // Aborts, frees the omc session on the worker and ends the worker. Never blocks the caller.
    void close();
    // Like close(), then waits up to `ms` for the worker to finish (shutdown only).
    bool closeAndWait(int ms);
    bool busy() const { return busy_.load(); }
    bool closed() const { return closing_.load(); }
    // omc refused to start it (no model set up yet, ...): it never will; the owner opens a new one to retry.
    bool openFailed() const { return openFailed_.load(); }
    QString sessionFile() const;
    QJsonObject cachedState() const;
    // get_state from a helper thread while the worker may be busy with a prompt; `done` runs on that thread.
    void refreshStateAsync(std::function<void(const QJsonObject&)> done);

private:
    OmcSession(OmcOptions opts, std::shared_ptr<OmcListener> listener);
    static void onEvent(void* ud, const char* json);
    static char* onFilterResult(void* ud, const char* tool, const char* text);
    void handleEvent(const char* json);
    void run();
    void openOnWorker();
    std::shared_ptr<OmcListener> listener() const;

    struct Waiter {
        bool done = false;
        QJsonObject frame;
    };

    OmcOptions opts_;
    omc_session* s_ = nullptr;
    mutable std::shared_mutex sMu_;  // shared: control senders; exclusive: the worker taking s_ away
    QString openError_;

    mutable std::mutex mu_;  // waiters_, listener_, state_
    std::condition_variable cv_;
    std::map<std::string, std::shared_ptr<Waiter>> waiters_;
    std::shared_ptr<OmcListener> listener_;
    QJsonObject state_;
    std::atomic<uint64_t> nextId_{1};
    uint64_t settled_ = 0;  // session_settled frames seen (mu_)

    std::mutex jobsMu_;
    std::condition_variable jobsCv_;
    std::deque<std::function<void(OmcSession&)>> jobs_;
    std::atomic<bool> closing_{false};
    std::atomic<bool> finished_{false};
    std::atomic<bool> openFailed_{false};
    std::atomic<bool> busy_{false};
    std::mutex doneMu_;
    std::condition_variable doneCv_;
};

}  // namespace rk
