#pragma once
// Runs host tool calls from omc (host_tool_call) and answers them (host_tool_result).
//
// Pipeline: unknown/internal -> profile denial -> mode
// phase gate -> decompiler requirement -> script approval -> approve_mutations -> undo pre-state -> run (main
// thread for MainThread tools) -> mutation record -> result cache -> injection-marker strip -> IOC filter ->
// artifact cap. Process-wide pool of 8 workers; Blocking tools get their own thread.
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <QJsonObject>
#include <QString>

#include "common/tools/Tool.h"

namespace rk {

struct ChatContext;
class Host;
class OmcSession;
class ToolRegistry;

class ToolDispatcher {
public:
    static ToolDispatcher& instance();

    // A host_tool_call frame ({id, toolCallId, toolName, arguments}) for `chat`. Any thread; never blocks.
    void dispatch(const std::shared_ptr<ChatContext>& chat, const std::shared_ptr<OmcSession>& session,
                  const QJsonObject& call);
    // host_tool_cancel from `session`: that call's token is set; a queued call is dropped.
    void cancel(const OmcSession* session, const QString& hostCallId);

    // The whole pipeline on the calling thread (also used by tests and Rikugan-internal calls).
    static ToolResult execute(ChatContext& chat, const QString& toolName, const QJsonObject& args,
                              const QString& toolCallId, const std::shared_ptr<CancelToken>& token,
                              const std::function<void(const QString&)>& update = {});
    // A tool by name with no gates and no logging (undo, pre-state readers, the bulk renamer). Main-thread
    // tools are marshalled. Returns the text; throws ToolError (also for an error result).
    static QString runDirect(Host& host, ToolRegistry& registry, ChatContext* chat, const QString& tool,
                             const QJsonObject& args, const std::shared_ptr<CancelToken>& token = {});

    void shutdown();

private:
    ToolDispatcher();
    ~ToolDispatcher();
    void workerLoop();
    void runJob(const std::shared_ptr<ChatContext>& chat, const std::weak_ptr<OmcSession>& session, const QJsonObject& call,
                const std::shared_ptr<CancelToken>& token, const QString& key);

    struct Job {
        std::shared_ptr<ChatContext> chat;
        std::weak_ptr<OmcSession> session;
        QJsonObject call;
        std::shared_ptr<CancelToken> token;
        QString key;
    };

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    std::map<QString, std::shared_ptr<CancelToken>> tokens_;  // session:host call id -> token (queued and running)
    std::vector<std::thread> workers_;
    bool stopping_ = false;
    int blockingRunning_ = 0;
};

}  // namespace rk
