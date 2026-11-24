#pragma once
// Frames from omc threads to one UI-thread QObject. push() from any thread; the first push into an empty queue
// posts one DrainEvent, and the receiver takes everything queued so far with drain(). No timers, no idle work.
#include <deque>
#include <mutex>
#include <string>

#include <QEvent>
#include <QObject>
#include <QPointer>

namespace rk {

class EventQueue {
public:
    static const QEvent::Type DrainEvent;

    explicit EventQueue(QObject* receiver) : receiver_(receiver) {}
    void push(std::string frame);
    std::deque<std::string> drain();
    // Stops posting (the receiver is going away). Called on the UI thread.
    void detach();

private:
    std::mutex mu_;
    std::deque<std::string> q_;
    bool posted_ = false;
    QPointer<QObject> receiver_;
};

}  // namespace rk
