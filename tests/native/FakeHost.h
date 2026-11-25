#pragma once
// A Host with a few tools for tests: main-thread marshalling through the Qt event loop.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMap>
#include <QSet>
#include <QMetaObject>
#include <QThread>
#include <QUuid>
#include <mutex>
#include <thread>
#include <vector>

#include "common/host/Host.h"
#include "common/tools/ToolRegistry.h"

namespace rktest {

class FakeHost : public rk::Host {
public:
    struct Event {
        QString what;
        qint64 at;
    };

    QString dbPath;
    std::mutex mu;
    QMap<uint64_t, QString> names{{0x401000, "sub_401000"}, {0x402000, "sub_402000"}};
    // rename_function answers "No function at ..." as ok text for these, like the hosts do
    QSet<uint64_t> renameMissing;
    std::vector<Event> events;
    std::atomic<bool> slowCancelled{false};
    QElapsedTimer clock;

    FakeHost() { clock.start(); }

    void note(const QString& what) {
        std::lock_guard<std::mutex> g(mu);
        events.push_back({what, clock.elapsed()});
    }

    QString hostName() const override { return QStringLiteral("binja"); }
    QString databasePath() const override { return dbPath; }
    QString inputPath() const override { return dbPath; }
    QString databaseInstanceId() override { return QStringLiteral("test-db"); }
    std::optional<uint64_t> currentAddress() const override { return 0x401000; }
    QString currentFunctionLabel() const override { return QStringLiteral("sub_401000"); }
    void navigate(uint64_t) override {}
    bool isMainThread() const override { return QThread::currentThread() == QCoreApplication::instance()->thread(); }
    bool runOnMainThread(const std::function<void()>& fn, rk::MainAccess) override {
        if (isMainThread()) {
            fn();
            return true;
        }
        auto job = std::make_shared<rk::MainThreadCall>(fn);
        QMetaObject::invokeMethod(QCoreApplication::instance(), [job] { job->runOnMain(); }, Qt::QueuedConnection);
        return job->wait();
    }
    bool hasDecompiler() const override { return true; }
    QString binaryInfo() override { return QStringLiteral("File: test.bin\nArch: x86_64"); }
    rk::PythonResult runPython(const QString&, const std::shared_ptr<rk::CancelToken>&) override { return {true, "(no output)", {}}; }
    bool writeBytes(uint64_t, const QByteArray&) override { return true; }
    QByteArray readBytes(uint64_t, size_t n) override { return QByteArray(qsizetype(n), '\0'); }

    void registerTools(rk::ToolRegistry& r) override {
        using rk::ToolParam;
        using rk::ToolResult;
        r.add(rk::ToolSpec{"get_cursor_position", "navigation", "Get the current cursor address.", {}, 0, 20000,
                           [](rk::ToolCall&) { return ToolResult::ok(QStringLiteral("0x401000")); }});
        r.add(rk::ToolSpec{"read_function_name", "annotations", "Read a function name.",
                           {ToolParam{"address", ToolParam::String, "Address", {}, {}, {}}}, rk::Internal | rk::MainThread, 20000,
                           [this](rk::ToolCall& c) {
                               std::lock_guard<std::mutex> g(mu);
                               return ToolResult::ok(names.value(c.address("address")));
                           }});
        r.add(rk::ToolSpec{"rename_function", "annotations", "Rename the function at an address.",
                           {ToolParam{"address", ToolParam::String, "Address", {}, {}, {}},
                            ToolParam{"new_name", ToolParam::String, "Name", {}, {}, {}}},
                           rk::Mutating | rk::MainThread, 20000, [this](rk::ToolCall& c) {
                               const uint64_t ea = c.address("address");
                               note(QStringLiteral("start %1").arg(rk::hex(ea)));
                               QThread::msleep(150);
                               QString old;
                               {
                                   std::lock_guard<std::mutex> g(mu);
                                   if (renameMissing.contains(ea)) return ToolResult::ok(QStringLiteral("No function at %1").arg(rk::hex(ea)));
                                   old = names.value(ea);
                                   names[ea] = c.str("new_name");
                               }
                               note(QStringLiteral("end %1").arg(rk::hex(ea)));
                               return ToolResult::ok(QStringLiteral("Renamed %1 → %2").arg(old, c.str("new_name")));
                           }});
        // 5 s of wall time, not a count of sleeps: macOS stretches short sleeps on a busy runner
        r.add(rk::ToolSpec{"slow_tool", "test", "Waits until cancelled (5 s at most).", {}, 0, 20000, [this](rk::ToolCall& c) {
                               QElapsedTimer clock;
                               clock.start();
                               while (clock.elapsed() < 5000) {
                                   if (c.cancelled()) {
                                       slowCancelled = true;
                                       return ToolResult::error(QStringLiteral("cancelled"));
                                   }
                                   QThread::msleep(10);
                               }
                               return ToolResult::ok(QStringLiteral("slow done"));
                           }});
        r.add(rk::ToolSpec{"big_tool", "test", "Returns a large text.", {}, 0, 1000, [](rk::ToolCall&) {
                               QStringList lines;
                               for (int i = 0; i < 300; ++i) lines << QStringLiteral("line %1").arg(i);
                               return ToolResult::ok(lines.join(QLatin1Char('\n')));
                           }});
    }
};

}  // namespace rktest
