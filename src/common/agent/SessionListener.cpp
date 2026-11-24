#include "common/agent/SessionListener.h"

#include <QJsonArray>
#include <QJsonDocument>

#include "common/tools/ToolDispatcher.h"

namespace rk {

SessionListener::SessionListener(std::shared_ptr<ChatContext> ctx, FrameFn onFrame)
    : ctx_(std::move(ctx)), frame_(std::move(onFrame)) {}

void SessionListener::setSession(const std::shared_ptr<OmcSession>& s) {
    std::lock_guard<std::mutex> g(mu_);
    session_ = s;
}

void SessionListener::setFrameFn(FrameFn fn) {
    std::lock_guard<std::mutex> g(mu_);
    frame_ = std::move(fn);
}

void SessionListener::onFrame(std::string_view type, const std::string& json) {
    // disk / shell tool approvals are answered from the chat's grants or become a permission card; they never
    // reach the frame callback (the generic dialog card is for other omc dialogs)
    if (type == "extension_ui_request" && ctx_ && ctx_->permissions) {
        const QJsonObject f = QJsonDocument::fromJson(QByteArray::fromRawData(json.data(), qsizetype(json.size()))).object();
        const QString method = f.value(QStringLiteral("method")).toString();
        if (method == QLatin1String("cancel") && ctx_->permissions->cancelRequest(f.value(QStringLiteral("targetId")).toString()))
            return;
        if (method == QLatin1String("select")) {
            std::shared_ptr<OmcSession> s;
            {
                std::lock_guard<std::mutex> g(mu_);
                s = session_.lock();
            }
            if (s && ctx_->permissions->intercept(f, s, ctx_->subagentId)) return;
        }
    }
    FrameFn fn;
    {
        std::lock_guard<std::mutex> g(mu_);
        fn = frame_;
    }
    if (fn) fn(type, json);
}

void SessionListener::onHostToolCall(const QJsonObject& call) {
    std::shared_ptr<OmcSession> s;
    {
        std::lock_guard<std::mutex> g(mu_);
        s = session_.lock();
    }
    if (!s) return;  // closing: omc fails the call on its own when the session goes
    ToolDispatcher::instance().dispatch(ctx_, s, call);
}

void SessionListener::onHostToolCancel(const QString& targetId) {
    std::shared_ptr<OmcSession> s;
    {
        std::lock_guard<std::mutex> g(mu_);
        s = session_.lock();
    }
    if (s) ToolDispatcher::instance().cancel(s.get(), targetId);  // gone: its calls end with it
}

void SessionListener::onOpenFailed(const QString& error) {
    const QJsonObject f{{"type", "rikugan_open_failed"}, {"error", error}};
    onFrame("rikugan_open_failed", QJsonDocument(f).toJson(QJsonDocument::Compact).toStdString());
}

}  // namespace rk
