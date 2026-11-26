#include "common/omc/EventQueue.h"

#include <QCoreApplication>

namespace rk {

const QEvent::Type EventQueue::DrainEvent = static_cast<QEvent::Type>(QEvent::registerEventType());

void EventQueue::push(std::string frame) {
    QObject* target = nullptr;
    {
        std::lock_guard<std::mutex> g(mu_);
        q_.push_back(std::move(frame));
        if (posted_ || !receiver_) return;
        posted_ = true;
        target = receiver_.data();
    }
    // thread-safe; the event is dropped if the receiver is deleted first
    QCoreApplication::postEvent(target, new QEvent(DrainEvent));
}

std::deque<std::string> EventQueue::drain() {
    std::lock_guard<std::mutex> g(mu_);
    posted_ = false;
    std::deque<std::string> out;
    out.swap(q_);
    return out;
}

void EventQueue::detach() {
    std::lock_guard<std::mutex> g(mu_);
    receiver_ = nullptr;
    q_.clear();
}

}  // namespace rk
