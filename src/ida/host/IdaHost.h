#pragma once
// rk::Host for IDA Pro: the one open database. Create it on the main thread once the database is open; destroy it
// on the main thread before the database closes.
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

#include "common/host/Host.h"

namespace rk::ida {

class MicrocodeOptimizers;

class IdaHost : public rk::Host {
public:
    IdaHost();
    ~IdaHost() override;

    QString hostName() const override { return QStringLiteral("ida"); }
    QString databasePath() const override;
    QString inputPath() const override;
    QString databaseInstanceId() override;
    std::optional<uint64_t> currentAddress() const override;
    QString currentFunctionLabel() const override;
    void navigate(uint64_t ea) override;
    bool runOnMainThread(const std::function<void()>& fn, MainAccess access) override;
    bool isMainThread() const override;
    bool hasDecompiler() const override;
    QString binaryInfo() override;
    void registerTools(ToolRegistry& registry) override;
    PythonResult runPython(const QString& code, const std::shared_ptr<CancelToken>& token) override;
    bool writeBytes(uint64_t ea, const QByteArray& bytes) override;
    QByteArray readBytes(uint64_t ea, size_t n) override;
    void databaseChanged() override;

    // The plugin's ui_screen_ea_changed hook keeps this current (any thread).
    void setCurrentAddress(uint64_t ea) { currentEa_.store(ea, std::memory_order_relaxed); }

    // Trusted helper code in IDAPython's __main__ (main thread only): runs `statements` after the guard prelude
    // and returns str(<resultExpr>). Throws ToolError when IDAPython is missing or the code fails.
    QString evalPythonHelper(const QString& statements, const QString& resultExpr);
    // A Python string literal holding `s` (decoded from hex, so any text is safe).
    static QString pyLiteral(const QString& s);

    MicrocodeOptimizers& microcode() { return *microcode_; }

private:
    void ensurePython();  // main thread; throws ToolError

    std::atomic<uint64_t> currentEa_;
    mutable std::atomic<int> decompiler_{-1};  // -1 unknown, 0 no, 1 yes
    bool pythonReady_ = false;                 // main thread only
    std::unique_ptr<MicrocodeOptimizers> microcode_;
};

}  // namespace rk::ida
