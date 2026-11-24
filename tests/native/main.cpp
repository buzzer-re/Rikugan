// Native test runner: a temporary home, omc's mock model server, then every registered QtTest class.
#include <cstdio>

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QLabel>
#include <QProcess>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest>

#include "TestEnv.h"
#include "common/config/RikuganConfig.h"
#include "common/omc/AgentHome.h"
#include "common/util/Env.h"
#include "common/ui/settings/SettingsDialog.h"

namespace rktest {

namespace {
Env g_env;
}

const Env& env() { return g_env; }

std::vector<Factory>& registry() {
    static std::vector<Factory> r;
    return r;
}

bool waitFor(const std::function<bool()>& pred, int ms) {
    QElapsedTimer t;
    t.start();
    while (!pred()) {
        if (t.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return true;
}

QString python() {
    const QString p = qEnvironmentVariable("RK_PYTHON");
    return p.isEmpty() ? QStringLiteral("python3") : p;
}

}  // namespace rktest

namespace {

// HOME, RIKUGAN_HOME and TMPDIR inside `dir`, and no provider keys from the environment: nothing may touch the real
// ~/.rikugan, ~/.omc or credentials, and a developer's ANTHROPIC_API_KEY must not change what the tests see. Through
// rk::setEnv so libomc's own C runtime (Windows) sees the same environment.
void isolate(const QTemporaryDir& dir) {
    rk::setEnv("HOME", dir.path());
    rk::setEnv("RIKUGAN_HOME", dir.path() + "/.rikugan");
    rk::setEnv("TMPDIR", dir.path());
#ifdef _WIN32
    rk::setEnv("USERPROFILE", QDir::toNativeSeparators(dir.path()));  // Qt's home on Windows
#endif
    QDir().mkpath(dir.path() + "/.git");  // bounds omc's project walk (see ../ohmyc/tests/e2e/run.sh)
    static const QRegularExpression credential(QStringLiteral("(_API_KEY|_TOKEN|_SECRET)$|^AWS_|^GOOGLE_APPLICATION"));
    for (const QString& k : QProcessEnvironment::systemEnvironment().keys())
        if (credential.match(k).hasMatch()) rk::unsetEnv(k.toUtf8().constData());
}

// A fresh install, run as a child of SettingsTest: no endpoints, no keys, no logins. omc's catalog and credentials
// are process-wide and the test process always holds the mock endpoint's key, so this needs its own process.
// Exit 0 once the Settings dialog lists providers.
int freshSettingsProbe(int argc, char** argv) {
    QTemporaryDir tmp;
    isolate(tmp);
    QApplication app(argc, argv);
    rk::AgentHome::prepare();
    rk::SettingsDialog dlg;
    auto* providers = dlg.findChild<QComboBox*>(QStringLiteral("providerCombo"));
    auto* status = dlg.findChild<QLabel*>(QStringLiteral("providerStatus"));
    if (!providers || !status) return 2;
    const bool listed = rktest::waitFor([&] { return providers->findData(QStringLiteral("anthropic")) >= 0; }, 30000);
    std::printf("providers=%d status=%s\n", providers->count(), qPrintable(status->text()));
    return listed && !status->text().contains(QLatin1String("Could not")) ? 0 : 1;
}

}  // namespace

int crashProbe(int argc, char** argv);  // test_crash.cpp

int main(int argc, char** argv) {
    if (argc > 1 && qstrcmp(argv[1], "--fresh-settings-probe") == 0) return freshSettingsProbe(1, argv);
    if (argc > 3 && qstrcmp(argv[1], "--crash-probe") == 0) return crashProbe(argc, argv);

    QTemporaryDir tmp;
    tmp.setAutoRemove(qEnvironmentVariableIsEmpty("RK_KEEP_TMP"));
    isolate(tmp);
    rktest::g_env.home = tmp.path();

    QApplication app(argc, argv);

    QTcpServer probe;
    probe.listen(QHostAddress::LocalHost, 0);
    rktest::g_env.port = probe.serverPort();
    probe.close();
    probe.listen(QHostAddress::LocalHost, 0);
    rktest::g_env.modePort = probe.serverPort();
    probe.close();
    QProcess mock;
    mock.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    QProcessEnvironment pe = QProcessEnvironment::systemEnvironment();
    pe.insert("MOCK_LOG", tmp.path() + "/mock.jsonl");
    mock.setProcessEnvironment(pe);
    mock.start(rktest::python(), {"-I", qEnvironmentVariable("RK_OMC_DIR") + "/tests/e2e/mock_openai.py",
                           QString::number(rktest::g_env.port)});
    if (!mock.waitForStarted()) qFatal("cannot start the mock model server");
    rktest::waitFor([&] {
        QTcpSocket s;
        s.connectToHost(QHostAddress::LocalHost, quint16(rktest::g_env.port));
        return s.waitForConnected(100);
    });

    rk::RikuganConfig& cfg = rk::RikuganConfig::instance();
    cfg.customEndpoints = {{"mock", QStringLiteral("http://127.0.0.1:%1/v1").arg(rktest::g_env.port),
                            "openai-completions", "MOCK_KEY", {"mock-1"}},
                           {"modemock", QStringLiteral("http://127.0.0.1:%1/v1").arg(rktest::g_env.modePort),
                            "openai-completions", "MOCK_KEY", {"m1"}}};
    cfg.model = rktest::g_env.model;
    cfg.autoContext = false;
    rk::AgentHome::prepare();

    int failures = 0;
    for (const rktest::Factory& f : rktest::registry()) {
        QObject* t = f();
        failures += QTest::qExec(t, argc, argv);
        delete t;
    }
    mock.kill();
    mock.waitForFinished();
    return failures ? 1 : 0;
}
