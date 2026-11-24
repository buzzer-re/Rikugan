#pragma once
// One open database in IDA or Binary Ninja: everything shared code needs from the host.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>

#include <QByteArray>
#include <QString>

#include "common/tools/Tool.h"

namespace rk {

class ToolRegistry;

enum class MainAccess { Read, Write };

struct PythonResult {
    bool ok = false;
    QString output;  // stdout + stderr of the script
    QString error;   // traceback or why it could not run
};

// The shared state of one job posted to the main thread. The host posts [job]{ job->runOnMain(); } with its
// own mechanism (IDA execute_sync MFF_NOWAIT, BN ExecuteOnMainThread) and calls wait() on the worker.
// wait() returns false only for a job that never started (Host::closing was set while it was queued): the
// main thread then skips it, so nothing ever runs against the abandoned caller's stack.
class MainThreadCall {
public:
    explicit MainThreadCall(std::function<void()> fn) : fn_(std::move(fn)) {}
    void runOnMain();
    bool wait();

private:
    enum class State { Queued, Running, Done, Abandoned };
    std::mutex mu_;
    std::condition_variable cv_;
    State state_ = State::Queued;
    std::function<void()> fn_;
};

class Host {
public:
    virtual ~Host() = default;

    virtual QString hostName() const = 0;  // "ida" | "binja"
    virtual QString databasePath() const = 0;
    virtual QString inputPath() const = 0;
    // Stable id stored in the database (IDA netnode "$ rikugan" supstr 0; BN metadata "rikugan_db_id").
    virtual QString databaseInstanceId() = 0;
    virtual std::optional<uint64_t> currentAddress() const = 0;
    virtual QString currentFunctionLabel() const = 0;
    virtual void navigate(uint64_t ea) = 0;
    // Runs `fn` on the host's main thread and waits; returns false (without running it, or abandoning the wait)
    // when Rikugan is shutting down. Never call while holding a lock the main thread may need.
    virtual bool runOnMainThread(const std::function<void()>& fn, MainAccess access) = 0;
    virtual bool isMainThread() const = 0;
    virtual bool hasDecompiler() const = 0;
    // The get_binary_info tool's text (prompt context, welcome card).
    virtual QString binaryInfo() = 0;
    // Adds every analysis tool of this host.
    virtual void registerTools(ToolRegistry& registry) = 0;
    // The host's own Python (IDAPython / Binary Ninja's interpreter): runs the guard prelude once, then
    // __rk_exec(code, <persistent namespace>). ok = no guard rejection; output = the script's captured output;
    // error = the guard's violation text. Main thread where the host requires it.
    virtual PythonResult runPython(const QString& code, const std::shared_ptr<CancelToken>& token) = 0;
    virtual bool writeBytes(uint64_t ea, const QByteArray& bytes) = 0;
    virtual QByteArray readBytes(uint64_t ea, size_t n) = 0;

    // Set when the plugin unloads: waits on the main thread give up.
    static std::atomic<bool> closing;
};

// Runs `f` on the main thread (inline when already there) and returns its value; exceptions thrown by `f`
// reach the caller. Throws ToolError when Rikugan is shutting down.
template <class F>
auto onMain(Host& host, MainAccess access, F&& f) -> std::invoke_result_t<F&> {
    using R = std::invoke_result_t<F&>;
    if (host.isMainThread()) return f();
    std::exception_ptr err;
    if constexpr (std::is_void_v<R>) {
        const bool ran = host.runOnMainThread(
            [&] {
                try {
                    f();
                } catch (...) {
                    err = std::current_exception();
                }
            },
            access);
        if (!ran) throw ToolError(QStringLiteral("Rikugan is shutting down"));
        if (err) std::rethrow_exception(err);
    } else {
        std::optional<R> out;
        const bool ran = host.runOnMainThread(
            [&] {
                try {
                    out.emplace(f());
                } catch (...) {
                    err = std::current_exception();
                }
            },
            access);
        if (!ran) throw ToolError(QStringLiteral("Rikugan is shutting down"));
        if (err) std::rethrow_exception(err);
        return std::move(*out);
    }
}

}  // namespace rk
