#pragma once
// The OmcListener of a chat-like session: host tool calls go to the ToolDispatcher with the chat's context, disk /
// shell tool approvals to the chat's PermissionState, other frames to a callback (which must not block: it runs on
// omc threads).
#include <functional>
#include <memory>
#include <mutex>

#include "common/agent/ChatContext.h"
#include "common/omc/OmcSession.h"

namespace rk {

class SessionListener : public OmcListener {
public:
    using FrameFn = std::function<void(std::string_view type, const std::string& json)>;

    SessionListener(std::shared_ptr<ChatContext> ctx, FrameFn onFrame);
    void setSession(const std::shared_ptr<OmcSession>& s);
    void setFrameFn(FrameFn fn);

    void onFrame(std::string_view type, const std::string& json) override;
    void onHostToolCall(const QJsonObject& call) override;
    void onHostToolCancel(const QString& targetId) override;
    void onOpenFailed(const QString& error) override;

private:
    std::shared_ptr<ChatContext> ctx_;
    std::mutex mu_;
    FrameFn frame_;
    std::weak_ptr<OmcSession> session_;
};

}  // namespace rk
