// The crash guard, in a child process that crashes on purpose: a fault inside Rikugan's code (on the main thread and
// on a worker) and an exception nobody catches each leave a crash log in the logs folder and a recovery copy from
// the host's save hook, and the next start reports the log once.
#include <thread>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include "TestEnv.h"
#include "common/util/CrashGuard.h"
#include "common/util/Files.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

void installForProbe(const QString& dir) {
    rk::crash::Hooks h;
    h.crashDir = (dir + "/logs").toStdString();
    h.version = "Rikugan test";
    h.saveOnMain = [dir]() -> std::string {
        const std::string out = rk::crash::recoveryPath((dir + "/sample.i64").toStdString(), "i64");
        QFile f(QString::fromStdString(out));
        if (f.open(QIODevice::WriteOnly)) f.write("saved");
        return out;
    };
    h.postToMain = [](std::function<void()> job) { QMetaObject::invokeMethod(qApp, std::move(job), Qt::QueuedConnection); };
    h.isMainThread = [] { return QThread::currentThread() == qApp->thread(); };
    rk::crash::install(std::move(h));
}

[[noreturn]] void fault() {
    volatile int* p = nullptr;
    *p = 1;
    std::abort();
}

}  // namespace

// rikugan_tests --crash-probe <main|worker|terminate> <dir>
int crashProbe(int argc, char** argv) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);  // no Windows Error Reporting dialog
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    QCoreApplication app(argc, argv);
    const QString mode = QString::fromLocal8Bit(argv[2]);
    installForProbe(QString::fromLocal8Bit(argv[3]));
    QMetaObject::invokeMethod(&app, [mode] {
        if (mode == QLatin1String("main")) fault();
        std::thread([mode] {
            rk::crash::guardThread();
            if (mode == QLatin1String("worker")) fault();
            throw std::runtime_error("probe exception");
        }).detach();
    }, Qt::QueuedConnection);
    return app.exec();
}

class CrashTest : public QObject {
    Q_OBJECT

    static QString onlyLog(const QString& dir) {
        const QStringList logs = QDir(dir + "/logs").entryList({"crash-log-*.log"}, QDir::Files);
        return logs.size() == 1 ? dir + "/logs/" + logs.first() : QString();
    }

    void crashIn(const QString& mode, const QString& reason) {
        QTemporaryDir dir;
        QProcess child;
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(), {"--crash-probe", mode, dir.path()});
        QVERIFY(child.waitForStarted());
        const qint64 pid = child.processId();
        QVERIFY(child.waitForFinished(60000));
        QVERIFY2(child.exitStatus() == QProcess::CrashExit || child.exitCode() != 0, child.readAll().constData());
        const QString log = onlyLog(dir.path());
        QVERIFY2(!log.isEmpty(), qPrintable(QDir(dir.path() + "/logs").entryList().join(", ")));
        QVERIFY(QFileInfo(log).fileName().endsWith(QStringLiteral("-%1.log").arg(pid)));
        const QByteArray text = rk::readFile(log);
        QVERIFY2(text.contains(reason.toUtf8()), text.constData());
        QVERIFY2(text.contains("Stack:"), text.constData());
        const QString copy = dir.path() + "/sample.rikugan-recovered.i64";
        QVERIFY2(text.contains(copy.toUtf8()), text.constData());
        QCOMPARE(rk::readFile(copy), QByteArray("saved"));
        if (qEnvironmentVariableIsSet("RK_SHOW_CRASH_LOG")) qInfo().noquote() << text;
    }

private Q_SLOTS:
    void faultOnMainThread() {
#ifdef _WIN32
        crashIn("main", "access violation");
#else
        crashIn("main", "SIGSEGV");
#endif
    }
    void faultOnWorker() {
#ifdef _WIN32
        crashIn("worker", "access violation");
#else
        crashIn("worker", "SIGSEGV");
#endif
    }
    void uncaughtException() { crashIn("terminate", "uncaught exception: probe exception"); }

    void nextStartReportsTheLogOnce() {
        QTemporaryDir dir;
        QDir().mkpath(dir.path() + "/logs");
        QFile f(dir.path() + "/logs/crash-log-20260101-000000-42.log");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        QStringList reported;
        rk::crash::Hooks h;
        h.crashDir = (dir.path() + "/logs").toStdString();
        h.reportPrevious = [&](const std::string& log) { reported << QString::fromStdString(log); };
        h.isMainThread = [] { return true; };
        rk::crash::install(h);
        rk::crash::uninstall();
        QCOMPARE(reported.size(), 1);
        rk::crash::install(h);
        rk::crash::uninstall();
        QCOMPARE(reported.size(), 1);
    }

    void recoveryPaths() {
        QCOMPARE(rk::crash::recoveryPath("C:/x/a.i64", "i64"), std::string("C:/x/a.rikugan-recovered.i64"));
        QCOMPARE(rk::crash::recoveryPath("/x/sample.exe", "bndb"), std::string("/x/sample.exe.rikugan-recovered.bndb"));
        QCOMPARE(rk::crash::recoveryPath("/x/a.bndb", "bndb"), std::string("/x/a.rikugan-recovered.bndb"));
    }
};

RK_TEST(CrashTest)
#include "test_crash.moc"
