#pragma once
// Shared test environment: a temporary HOME / RIKUGAN_HOME, omc's mock OpenAI server, a "mock/mock-1" model.
#include <functional>
#include <vector>

#include <QObject>
#include <QString>

namespace rktest {

struct Env {
    QString home;
    int port = 0;
    // a port reserved for a test's own mock server, registered as provider "modemock" (model "m1"): omc reads
    // models.yml once per process, so every endpoint a test uses must exist before the first session opens
    int modePort = 0;
    QString model = QStringLiteral("mock/mock-1");
};

const Env& env();

// Test classes register themselves; main() runs them all.
using Factory = std::function<QObject*()>;
std::vector<Factory>& registry();
struct Registrar {
    explicit Registrar(Factory f) { registry().push_back(std::move(f)); }
};

// Spins the Qt event loop until `pred` is true or `ms` pass.
bool waitFor(const std::function<bool()>& pred, int ms = 10000);

// The Python interpreter for omc's mock server and script checks: RK_PYTHON (set by CTest), else python3.
QString python();

}  // namespace rktest

#define RK_TEST(Class) static rktest::Registrar rk_registrar_##Class([] { return static_cast<QObject*>(new Class); });
