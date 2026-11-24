#pragma once
// rk::Host for one Binary Ninja BinaryView.
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

#include <binaryninjaapi.h>

#include "common/host/Host.h"

namespace rk::binja {

class BnPython;

class BnHost : public rk::Host {
public:
    explicit BnHost(BinaryNinja::Ref<BinaryNinja::BinaryView> view);
    ~BnHost() override;

    BnHost(const BnHost&) = delete;
    BnHost& operator=(const BnHost&) = delete;

    const BinaryNinja::Ref<BinaryNinja::BinaryView>& view() const { return view_; }

    // The cursor of this view, reported by the plugin's UI notifications (any thread).
    void setCurrentLocation(uint64_t addr);

    // True when the UI moved to `ea` (runs on the main thread).
    bool navigateTo(uint64_t ea);
    // runPython for a tool call: cancellation follows `cancelled`.
    PythonResult runGuardedPython(const QString& code, const std::function<bool()>& cancelled);
    // Runs trusted Rikugan Python (not the guard) in BN's interpreter: `source` is exec'd in a fresh dict holding
    // `bv` and the persistent `__rk_state` dict; the value it assigns to `__rk_result` is returned as output.
    PythonResult runTrustedPython(const QString& source, const std::function<bool()>& cancelled);

    QString hostName() const override;
    QString databasePath() const override;
    QString inputPath() const override;
    QString databaseInstanceId() override;
    static QString storedInstanceId(BinaryNinja::BinaryView& view);  // empty: Rikugan never saw this file
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

private:
    std::optional<uint64_t> uiAddress() const;
    BnPython& python();

    BinaryNinja::Ref<BinaryNinja::BinaryView> view_;
    std::atomic<bool> hasLocation_{false};
    std::atomic<uint64_t> location_{0};
    std::mutex idMu_;
    QString instanceId_;
    std::mutex pythonMu_;
    std::unique_ptr<BnPython> python_;
};

}  // namespace rk::binja
