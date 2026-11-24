#include "common/host/Host.h"

#include <chrono>

namespace rk {

std::atomic<bool> Host::closing{false};

void MainThreadCall::runOnMain() {
    {
        std::lock_guard<std::mutex> g(mu_);
        // the database is going away: a job that has not started never will
        if (state_ == State::Abandoned || (Host::closing.load() && state_ == State::Queued)) {
            state_ = State::Abandoned;
            cv_.notify_all();
            return;
        }
    }
    fn_();
    {
        std::lock_guard<std::mutex> g(mu_);
        state_ = State::Done;
        fn_ = nullptr;
    }
    cv_.notify_all();
}

bool MainThreadCall::wait() {
    std::unique_lock<std::mutex> lk(mu_);
    for (;;) {
        if (cv_.wait_for(lk, std::chrono::milliseconds(100), [&] { return state_ == State::Done || state_ == State::Abandoned; }))
            return state_ == State::Done;
        if (Host::closing.load() && state_ == State::Queued) {
            state_ = State::Abandoned;
            return false;
        }
    }
}

}  // namespace rk
