#include "common/agent/ChatContext.h"

namespace rk {

void ChatContext::setPhase(const QString& name, bool readOnly) {
    std::lock_guard<std::mutex> g(mu_);
    phase_ = name;
    readOnly_ = readOnly;
}

QString ChatContext::phase() const {
    std::lock_guard<std::mutex> g(mu_);
    return phase_;
}

bool ChatContext::phaseReadOnly() const {
    std::lock_guard<std::mutex> g(mu_);
    return readOnly_;
}

void ChatContext::setToolShown(const QString& name, bool shown) {
    std::lock_guard<std::mutex> g(mu_);
    if (shown) shown_.insert(name);
    else shown_.remove(name);
}

bool ChatContext::toolShown(const QString& name) const {
    std::lock_guard<std::mutex> g(mu_);
    return shown_.contains(name);
}


void ChatContext::setFrameHook(std::function<void(std::string_view)> fn) {
    std::lock_guard<std::mutex> g(mu_);
    frameHook_ = std::move(fn);
}

void ChatContext::notifyFrame(std::string_view type) {
    std::function<void(std::string_view)> fn;
    {
        std::lock_guard<std::mutex> g(mu_);
        fn = frameHook_;
    }
    if (fn) fn(type);
}
}  // namespace rk
