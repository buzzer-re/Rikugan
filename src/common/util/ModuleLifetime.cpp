#include "common/util/ModuleLifetime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QThread>

#include "common/util/Log.h"

namespace rk {

namespace {

struct Spawned {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
};

// Never destroyed: a host that exits without unloading the plugin (Binary Ninja) must not run ~std::thread on a
// joinable thread, which terminates the process.
struct Registry {
    std::mutex anchorMu;
    QObject* anchor = nullptr;
    std::mutex threadsMu;
    std::condition_variable threadsCv;
    std::vector<Spawned> threads;
};

Registry& reg() {
    static Registry* r = new Registry;
    return *r;
}

// Joins threads that have finished their work (they are only unwinding); with threadsMu held.
void reapLocked(Registry& r) {
    for (auto it = r.threads.begin(); it != r.threads.end();) {
        if (it->done->load()) {
            it->thread.join();
            it = r.threads.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace

QObject* uiAnchor() {
    Registry& r = reg();
    std::lock_guard<std::mutex> g(r.anchorMu);
    if (!r.anchor) {
        r.anchor = new QObject;
        // created on a worker: hand it to the UI thread (pushing from the owning thread is allowed)
        if (QCoreApplication* app = QCoreApplication::instance(); app && QThread::currentThread() != app->thread())
            r.anchor->moveToThread(app->thread());
    }
    return r.anchor;
}

void spawnThread(std::function<void()> fn) {
    Registry& r = reg();
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread t([&r, fn = std::move(fn), done]() mutable {
        {
            // the job and what it captured are destroyed before the thread counts as done
            std::function<void()> job = std::move(fn);
            job();
        }
        {
            std::lock_guard<std::mutex> g(r.threadsMu);
            done->store(true);
        }
        r.threadsCv.notify_all();
    });
    std::lock_guard<std::mutex> g(r.threadsMu);
    reapLocked(r);
    r.threads.push_back(Spawned{std::move(t), std::move(done)});
}

void shutdownModule(int waitMs) {
    Registry& r = reg();
    {
        std::unique_lock<std::mutex> lk(r.threadsMu);
        r.threadsCv.wait_for(lk, std::chrono::milliseconds(waitMs), [&r] {
            for (const Spawned& s : r.threads)
                if (!s.done->load()) return false;
            return true;
        });
        reapLocked(r);
        if (!r.threads.empty()) {
            log(QStringLiteral("Rikugan: %1 background thread(s) still running at unload").arg(r.threads.size()));
            for (Spawned& s : r.threads) s.thread.detach();
            r.threads.clear();
        }
    }
    std::lock_guard<std::mutex> g(r.anchorMu);
    delete r.anchor;
    r.anchor = nullptr;
}

}  // namespace rk
