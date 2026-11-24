#pragma once
// One Binary Ninja Python ScriptingInstance per database, driven like the Python console.
//
// BN's Python provider runs input asynchronously on its interpreter thread and feeds it to an InteractiveConsole
// line by line, so every command sent here is a single line: the real source travels base64-encoded and is exec'd
// by helpers installed once (the guard prelude plus Rikugan's own). Results come back on stdout between markers;
// completion is the input-ready state returning to ReadyForScriptExecution.
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <binaryninjaapi.h>

#include "common/host/Host.h"

namespace rk::binja {

class BnPython : public BinaryNinja::ScriptingOutputListener {
public:
    explicit BnPython(BinaryNinja::Ref<BinaryNinja::BinaryView> view);
    ~BnPython();

    BnPython(const BnPython&) = delete;
    BnPython& operator=(const BnPython&) = delete;

    // True when the caller gave up (tool call cancelled).
    using CancelFn = std::function<bool()>;

    // `code` through the guard (__rk_exec) in the persistent namespace {bv, binaryninja, binaryninjaui,
    // current_address}.
    PythonResult runGuarded(const QString& code, uint64_t currentAddress, const CancelFn& cancelled);
    // Trusted Rikugan source, see BnHost::runTrustedPython.
    PythonResult runTrusted(const QString& source, const CancelFn& cancelled);

    void NotifyOutput(const std::string& text) override;
    void NotifyWarning(const std::string& text) override;
    void NotifyError(const std::string& text) override;
    void NotifyInputReadyStateChanged(BNScriptingProviderInputReadyState state) override;

private:
    struct LineResult {
        bool finished = false;  // the interpreter ran the line and is ready again
        bool cancelled = false;
        QString error;          // why the line could not run
        std::string out;
        std::string err;
    };

    // Requires runMu_. Creates the instance and runs the bootstrap once.
    QString ensureReady(const CancelFn& cancelled);
    LineResult runLine(const std::string& line, const CancelFn& cancelled);
    // Requires runMu_. Prints `call` between the result markers and returns the text between them.
    PythonResult runHelper(const std::string& call, const CancelFn& cancelled);

    BinaryNinja::Ref<BinaryNinja::BinaryView> view_;
    BinaryNinja::Ref<BinaryNinja::ScriptingInstance> instance_;
    bool bootstrapped_ = false;
    bool hasPrelude_ = false;

    std::mutex runMu_;  // one line at a time

    std::mutex mu_;  // guards everything below (written by the interpreter thread)
    std::condition_variable cv_;
    std::string out_;
    std::string err_;
    BNScriptingProviderInputReadyState state_ = ReadyForScriptExecution;
    uint64_t readyCount_ = 0;
};

}  // namespace rk::binja
