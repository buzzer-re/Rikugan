// Rikugan's updates against a local stand-in for GitHub's releases: the latest release is found through the
// /releases/latest redirect, a newer one is downloaded, checked and put in place of the installed plugin, and a
// package that does not match its SHA-256, holds another version, lacks the plugin or cannot be written leaves the
// installed plugin exactly as it was. The Updater turns the same steps into what the panel shows.
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "TestEnv.h"
#include "common/update/Update.h"
#include "common/update/Updater.h"

using namespace rk::update;

namespace {

const char kServer[] = R"PY(
import http.server, sys
root, tag = sys.argv[1], sys.argv[2]
class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=root, **k)
    def log_message(self, *a):
        pass
    def route(self):
        if self.path == '/releases/latest':
            self.send_response(302)
            self.send_header('Location', '/releases/tag/' + tag)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return True
        if self.path == '/releases/tag/' + tag:
            body = b'the release page'
            self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            if self.command == 'GET':
                self.wfile.write(body)
            return True
        if self.path.startswith('/releases/'):
            self.path = self.path[len('/releases'):]
        return False
    def do_GET(self):
        if not self.route():
            super().do_GET()
    def do_HEAD(self):
        if not self.route():
            super().do_HEAD()
srv = http.server.ThreadingHTTPServer(('127.0.0.1', 0), H)
print(srv.server_address[1], flush=True)
srv.serve_forever()
)PY";

const QString kPackage = QStringLiteral("binja-linux-test");
const QString kTag = QStringLiteral("v9.9.9");

QByteArray plugin(const char* body, const char* version) {
    return QByteArray(body) + '\0' + "RIKUGAN-VERSION:" + version + '\0' + "rest of the module";
}

void writeFile(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) qFatal("cannot write %s", qPrintable(path));
    f.write(data);
}

QByteArray readFile(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

}  // namespace

class UpdateTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir;
    QProcess server;
    QString module;   // the installed plugin: <dir>/plugins/librikugan.so
    QString release;  // where the server serves the release's files

    // The release's package holding `files` (relative path -> contents) and its .sha256 (`checksum` when given).
    void publish(const QMap<QString, QByteArray>& files, const QString& checksum = {}) {
        const QString stage = dir.filePath(QStringLiteral("stage"));
        QDir(stage).removeRecursively();
        QStringList names;
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            writeFile(stage + QLatin1Char('/') + it.key(), it.value());
            names << it.key();
        }
        const QString archive = release + QStringLiteral("/rikugan-9.9.9-%1.tar.gz").arg(kPackage);
        QDir().mkpath(release);
        QFile::remove(archive);
        QProcess tar;
        tar.start(QStringLiteral("tar"), QStringList{QStringLiteral("-czf"), archive, QStringLiteral("-C"), stage} + names);
        QVERIFY(tar.waitForFinished(30000) && tar.exitCode() == 0);
        const QString sum = checksum.isEmpty() ? sha256OfFile(archive) : checksum;
        writeFile(archive + QStringLiteral(".sha256"), (sum + QStringLiteral("  rikugan-9.9.9-%1.tar.gz\n").arg(kPackage)).toUtf8());
    }

    std::optional<Release> latest() {
        QString error;
        const QUrl landing = latestLanding(releasesUrl(), &error);
        if (landing.isEmpty()) qWarning("%s", qPrintable(error));
        return releaseFromLanding(landing, releasesUrl(), kPackage);
    }

    QStringList leftovers() {
        return QDir(QFileInfo(module).absolutePath()).entryList({QStringLiteral("*.rikugan-*")}, QDir::Files);
    }

private Q_SLOTS:
    void initTestCase() {
        const QString script = dir.filePath(QStringLiteral("server.py"));
        writeFile(script, kServer);
        release = dir.filePath(QStringLiteral("www/download/") + kTag);
        QDir().mkpath(release);
        server.start(rktest::python(), {QStringLiteral("-I"), script, dir.filePath(QStringLiteral("www")), kTag});
        QVERIFY(server.waitForStarted(10000));
        QVERIFY(server.waitForReadyRead(60000));  // a Python start can take seconds on a busy CI runner
        const int port = server.readLine().trimmed().toInt();
        QVERIFY(port > 0);
        qputenv("RIKUGAN_UPDATE_URL", QStringLiteral("http://127.0.0.1:%1/releases").arg(port).toUtf8());
        module = dir.filePath(QStringLiteral("plugins/librikugan.so"));
    }

    void cleanupTestCase() {
        server.kill();
        server.waitForFinished(5000);
        qunsetenv("RIKUGAN_UPDATE_URL");
    }

    void init() { writeFile(module, plugin("installed build", "1.0.0")); }

    void versionsCompareAsNumbers() {
        QVERIFY(compareVersions(QStringLiteral("2.0.10"), QStringLiteral("2.0.9")) > 0);
        QVERIFY(compareVersions(QStringLiteral("v2.1"), QStringLiteral("2.0.99")) > 0);
        QCOMPARE(compareVersions(QStringLiteral("2.0"), QStringLiteral("2.0.0")), 0);
        QVERIFY(compareVersions(QStringLiteral("2.0.0-rc1"), QStringLiteral("1.0.0")) < 0);  // unparsable: older
    }

    void onlyAReleasePageOfTheSameSiteIsARelease() {
        const QUrl releases(QStringLiteral("https://github.com/buzzer-re/Rikugan/releases"));
        const auto r = releaseFromLanding(QUrl(QStringLiteral("https://github.com/buzzer-re/Rikugan/releases/tag/v2.1.0")), releases,
                                          QStringLiteral("ida-windows-x86_64"));
        QVERIFY(r);
        QCOMPARE(r->version, QStringLiteral("2.1.0"));
        QCOMPARE(r->package.toString(),
                 QStringLiteral("https://github.com/buzzer-re/Rikugan/releases/download/v2.1.0/rikugan-2.1.0-ida-windows-x86_64.zip"));
        QCOMPARE(r->checksum.toString(), r->package.toString() + QStringLiteral(".sha256"));
        // no release published yet lands on the list; another site, http or a query are not GitHub's answer
        for (const char* landed : {"https://github.com/buzzer-re/Rikugan/releases", "https://evil.example/buzzer-re/Rikugan/releases/tag/v9.0.0",
                                   "http://github.com/buzzer-re/Rikugan/releases/tag/v9.0.0",
                                   "https://github.com/buzzer-re/Rikugan/releases/tag/v9.0.0?x=1",
                                   "https://github.com/buzzer-re/Rikugan/releases/tag/nightly"})
            QVERIFY2(!releaseFromLanding(QUrl(QString::fromLatin1(landed)), releases, QStringLiteral("binja-linux-x86_64")), landed);
    }

    void aDayBetweenChecks() {
        const qint64 day = 24LL * 3600 * 1000, now = 100 * day;
        QVERIFY(isDue(0, now));
        QVERIFY(!isDue(now - day + 1000, now));
        QVERIFY(isDue(now - day, now));
        QVERIFY(isDue(now + 1000, now));  // the clock went back
    }

    void newerReleaseReplacesTheInstalledPlugin() {
        publish({{QStringLiteral("librikugan.so"), plugin("new build", "9.9.9")}});
        const auto r = latest();
        QVERIFY(r);
        QCOMPARE(r->version, QStringLiteral("9.9.9"));
        QString error;
        QStringList steps;
        const QString work = dir.filePath(QStringLiteral("work"));
        QVERIFY2(install(*r, module, work, &error, [&](const QString& s) { steps << s; }), qPrintable(error));
        QVERIFY(readFile(module).startsWith("new build"));
        QCOMPARE(versionOfFile(module), QStringLiteral("9.9.9"));
        QVERIFY(leftovers().isEmpty());
        QVERIFY(!QFileInfo::exists(work));
        QCOMPARE(steps.size(), 3);  // downloading, checking, installing
    }

    void corruptDownloadIsNotInstalled() {
        publish({{QStringLiteral("librikugan.so"), plugin("new build", "9.9.9")}}, QString(64, QLatin1Char('a')));
        QString error;
        QVERIFY(!install(*latest(), module, dir.filePath(QStringLiteral("work")), &error));
        QVERIFY2(error.contains(QStringLiteral("SHA-256")), qPrintable(error));
        QCOMPARE(versionOfFile(module), QStringLiteral("1.0.0"));
        QVERIFY(leftovers().isEmpty());
    }

    void packageOfAnotherVersionIsNotInstalled() {
        publish({{QStringLiteral("librikugan.so"), plugin("new build", "9.9.8")}});
        QString error;
        QVERIFY(!install(*latest(), module, dir.filePath(QStringLiteral("work")), &error));
        QVERIFY2(error.contains(QStringLiteral("9.9.8")), qPrintable(error));
        QCOMPARE(versionOfFile(module), QStringLiteral("1.0.0"));
    }

    void packageWithoutThePluginIsNotInstalled() {
        publish({{QStringLiteral("rikugan/libomc.dll"), QByteArray("library")}});
        QString error;
        QVERIFY(!install(*latest(), module, dir.filePath(QStringLiteral("work")), &error));
        QVERIFY2(error.contains(QStringLiteral("librikugan.so")), qPrintable(error));
        QCOMPARE(versionOfFile(module), QStringLiteral("1.0.0"));
        QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("plugins/rikugan/libomc.dll"))));
    }

    void unwritableFolderLeavesEverythingAsItWas() {
#ifdef _WIN32
        QSKIP("folder permissions are not POSIX modes on Windows");
#else
        if (geteuid() == 0) QSKIP("root writes anywhere");
        // two files: the plugin and a library next to it, as the Windows packages hold
        publish({{QStringLiteral("librikugan.so"), plugin("new build", "9.9.9")}, {QStringLiteral("rikugan/libomc.so"), QByteArray("lib")}});
        const QString plugins = QFileInfo(module).absolutePath();
        QDir().mkpath(plugins + QStringLiteral("/rikugan"));
        QFile::setPermissions(plugins + QStringLiteral("/rikugan"), QFile::ReadOwner | QFile::ExeOwner);
        QString error;
        const bool ok = install(*latest(), module, dir.filePath(QStringLiteral("work")), &error);
        QFile::setPermissions(plugins + QStringLiteral("/rikugan"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QVERIFY(!ok);
        QVERIFY2(error.contains(QStringLiteral("cannot write")), qPrintable(error));
        QCOMPARE(versionOfFile(module), QStringLiteral("1.0.0"));
        QVERIFY(leftovers().isEmpty());
#endif
    }

    void updaterOffersAndInstalls() {
        publish({{QStringLiteral("librikugan.so"), plugin("new build", "9.9.9")}});
        Updater& u = Updater::instance();
        u.setTarget(kPackage, module);
        u.check(true);
        QVERIFY(rktest::waitFor([&] { return u.state() == Updater::State::Available; }, 30000));
        QCOMPARE(u.release().version, QStringLiteral("9.9.9"));
        u.later();
        QVERIFY(u.dismissed());
        u.check(true);  // a new check offers it again
        QVERIFY(rktest::waitFor([&] { return u.state() == Updater::State::Available && !u.dismissed(); }, 30000));
        u.installAvailable();
        QVERIFY(rktest::waitFor([&] { return u.state() == Updater::State::Installed || u.state() == Updater::State::Failed; }, 60000));
        QVERIFY2(u.state() == Updater::State::Installed, qPrintable(u.message()));
        QCOMPARE(versionOfFile(module), QStringLiteral("9.9.9"));
        // the next check sees 9.9.9 on disk: a restart is all it takes
        u.check(true);
        QVERIFY(rktest::waitFor([&] { return u.state() == Updater::State::Installed; }, 30000));
    }

    void aDeveloperBuildDoesNotUpdateItself() {
        Updater& u = Updater::instance();
        u.setTarget(QString(), module);
        u.check(true);
        QCOMPARE(u.state(), Updater::State::Failed);
        QVERIFY(u.message().contains(QStringLiteral("release package")));
    }
};

RK_TEST(UpdateTest)
#include "test_update.moc"
