#include "common/update/Update.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdio>
#endif

namespace rk::update {

namespace {

// Every build carries it, so an update can tell which version a downloaded plugin is before installing it.
const char kMarker[] = "RIKUGAN-VERSION:" RIKUGAN_VERSION;
constexpr int kPrefixLen = int(sizeof("RIKUGAN-VERSION:")) - 1;
const char kOld[] = ".rikugan-old";
const char kNew[] = ".rikugan-new";

#ifdef _WIN32
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif

// A tool the system ships: the System32 copy on Windows (curl.exe and tar.exe since Windows 10), /usr/bin's elsewhere.
QString systemTool(const char* name) {
#ifdef _WIN32
    const QString sys = QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows"))).filePath(QStringLiteral("System32/%1.exe").arg(QLatin1String(name)));
#else
    const QString sys = QStringLiteral("/usr/bin/%1").arg(QLatin1String(name));
#endif
    if (QFileInfo(sys).isExecutable()) return sys;
    return QStandardPaths::findExecutable(QString::fromLatin1(name));
}

bool run(const char* tool, const QStringList& args, int timeoutMs, QString* error, QByteArray* out = nullptr) {
    const QString exe = systemTool(tool);
    if (exe.isEmpty()) {
        if (error) *error = QStringLiteral("%1 is not installed: Rikugan downloads and unpacks updates with it").arg(QLatin1String(tool));
        return false;
    }
    QProcess p;
    p.setProgram(exe);
    p.setArguments(args);
    p.start();
    if (!p.waitForStarted(10000)) {
        if (error) *error = QStringLiteral("cannot run %1: %2").arg(exe, p.errorString());
        return false;
    }
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(3000);
        if (error) *error = QStringLiteral("%1 took too long").arg(QLatin1String(tool));
        return false;
    }
    if (out) *out = p.readAllStandardOutput();
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        if (error) {
            const QString err = QString::fromUtf8(p.readAllStandardError()).trimmed().left(300);
            *error = QStringLiteral("%1 failed (exit %2)%3").arg(QLatin1String(tool)).arg(p.exitCode())
                         .arg(err.isEmpty() ? QString() : QStringLiteral(": ") + err);
        }
        return false;
    }
    return true;
}

// https only, unless the releases themselves are elsewhere (a test's local server)
QStringList protocols(const QUrl& url) {
    if (url.scheme() == QLatin1String("https")) return {QStringLiteral("--proto"), QStringLiteral("=https"), QStringLiteral("--proto-redir"), QStringLiteral("=https")};
    return {};
}

QString nullDevice() {
#ifdef _WIN32
    return QStringLiteral("NUL");
#else
    return QStringLiteral("/dev/null");
#endif
}

// `from` replaces `to` (which may not exist); the running host may have `to` open (Windows: loaded).
bool replaceFile(const QString& from, const QString& to) {
#ifdef _WIN32
    return MoveFileExW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(from).utf16()),
                       reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(to).utf16()), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return std::rename(QFile::encodeName(from).constData(), QFile::encodeName(to).constData()) == 0;
#endif
}

QString systemError() {
#ifdef _WIN32
    return QStringLiteral("Windows error %1").arg(GetLastError());
#else
    return QString::fromLocal8Bit(std::strerror(errno));
#endif
}

}  // namespace

std::optional<QList<int>> parseVersion(const QString& text) {
    QString t = text.trimmed();
    if (t.startsWith(QLatin1Char('v'))) t.remove(0, 1);
    const QStringList parts = t.split(QLatin1Char('.'));
    if (parts.isEmpty() || parts.size() > 4) return std::nullopt;
    QList<int> nums;
    for (const QString& p : parts) {
        bool ok = false;
        const int n = p.toInt(&ok);
        if (!ok || n < 0 || p.isEmpty() || !p.front().isDigit()) return std::nullopt;
        nums << n;
    }
    return nums;
}

int compareVersions(const QString& a, const QString& b) {
    const auto x = parseVersion(a), y = parseVersion(b);
    if (!x || !y) return x ? 1 : (y ? -1 : 0);
    for (qsizetype i = 0; i < std::max(x->size(), y->size()); ++i) {
        const int p = i < x->size() ? x->at(i) : 0, q = i < y->size() ? y->at(i) : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

const char* versionMarker() { return kMarker; }

QString currentVersion() { return QString::fromLatin1(kMarker + kPrefixLen); }

QString packageName() {
#ifdef RIKUGAN_PACKAGE
    return QStringLiteral(RIKUGAN_PACKAGE);
#else
    return {};
#endif
}

QString hostName(const QString& package) {
    return package.startsWith(QLatin1String("ida-")) ? QStringLiteral("IDA Pro") : QStringLiteral("Binary Ninja");
}

QUrl releasesUrl() {
    const QString o = qEnvironmentVariable("RIKUGAN_UPDATE_URL");
    return QUrl(o.isEmpty() ? QStringLiteral("https://github.com/buzzer-re/Rikugan/releases") : o);
}

std::optional<Release> releaseFromLanding(const QUrl& landed, const QUrl& releases, const QString& package) {
    if (package.isEmpty() || landed.scheme() != releases.scheme() || landed.host() != releases.host() ||
        landed.port() != releases.port() || landed.hasQuery() || landed.hasFragment() || !landed.userName().isEmpty())
        return std::nullopt;
    const QString prefix = releases.path() + QStringLiteral("/tag/");
    if (!landed.path().startsWith(prefix)) return std::nullopt;
    const QString tag = landed.path().mid(prefix.size());
    if (!tag.startsWith(QLatin1Char('v')) || tag.contains(QLatin1Char('/')) || !parseVersion(tag)) return std::nullopt;
    Release r;
    r.tag = tag;
    r.version = tag.mid(1);
    r.page = landed;
    const QString ext = package.contains(QLatin1String("windows")) ? QStringLiteral("zip") : QStringLiteral("tar.gz");
    QUrl dl = releases;
    dl.setPath(releases.path() + QStringLiteral("/download/%1/rikugan-%2-%3.%4").arg(tag, r.version, package, ext));
    r.package = dl;
    dl.setPath(dl.path() + QStringLiteral(".sha256"));
    r.checksum = dl;
    return r;
}

bool isDue(qint64 lastCheckMs, qint64 nowMs) {
    return lastCheckMs <= 0 || nowMs - lastCheckMs >= 24LL * 3600 * 1000 || lastCheckMs > nowMs;
}

QUrl latestLanding(const QUrl& releases, QString* error) {
    QUrl latest = releases;
    latest.setPath(releases.path() + QStringLiteral("/latest"));
    QByteArray out;
    // HEAD, following the redirect to the latest release's page; curl prints where it landed
    QStringList args{QStringLiteral("-fsSIL"), QStringLiteral("--connect-timeout"), QStringLiteral("15"),
                     QStringLiteral("--max-time"), QStringLiteral("60"), QStringLiteral("-o"), nullDevice(),
                     QStringLiteral("-w"), QStringLiteral("%{url_effective}")};
    args << protocols(releases) << latest.toString(QUrl::FullyEncoded);
    if (!run("curl", args, 90000, error, &out)) return {};
    return QUrl(QString::fromUtf8(out).trimmed());
}

bool download(const QUrl& url, const QString& file, QString* error) {
    QStringList args{QStringLiteral("-fsSL"), QStringLiteral("--retry"), QStringLiteral("2"), QStringLiteral("--connect-timeout"),
                     QStringLiteral("20"), QStringLiteral("--max-time"), QStringLiteral("900"), QStringLiteral("-o"), file};
    args << protocols(url) << url.toString(QUrl::FullyEncoded);
    QString err;
    if (run("curl", args, 960000, &err)) return true;
    if (error) *error = QStringLiteral("cannot download %1: %2").arg(url.fileName(), err);
    return false;
}

QString hashInChecksumFile(const QString& file) {
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QString word = QString::fromLatin1(f.read(4096)).simplified().section(QLatin1Char(' '), 0, 0).toLower();
    if (word.size() != 64) return {};
    for (const QChar c : word)
        if (!c.isDigit() && (c < QLatin1Char('a') || c > QLatin1Char('f'))) return {};
    return word;
}

QString sha256OfFile(const QString& file) {
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f)) return {};
    return QString::fromLatin1(h.result().toHex());
}

QString versionOfFile(const QString& file) {
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray data = f.readAll();
    const QByteArray prefix = QByteArray::fromRawData(kMarker, kPrefixLen);
    for (qsizetype at = data.indexOf(prefix); at >= 0; at = data.indexOf(prefix, at + 1)) {
        qsizetype end = at + kPrefixLen;
        while (end < data.size() && (std::isdigit(static_cast<unsigned char>(data[end])) || data[end] == '.')) ++end;
        if (end > at + kPrefixLen && end < data.size() && data[end] == '\0')
            return QString::fromLatin1(data.mid(at + kPrefixLen, end - at - kPrefixLen));
    }
    return {};
}

std::optional<Plan> plan(const QString& unpacked, const QString& module, QString* error) {
    auto fail = [&](const QString& why) -> std::optional<Plan> {
        if (error) *error = why;
        return std::nullopt;
    };
    QStringList files;
    QDirIterator it(unpacked, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo fi(it.next());
        if (fi.isSymLink()) return fail(QStringLiteral("the package holds a link (%1): not installed").arg(fi.fileName()));
        if (fi.isDir()) continue;
        if (!fi.isFile()) return fail(QStringLiteral("the package holds something that is not a file: not installed"));
        files << QDir(unpacked).relativeFilePath(fi.absoluteFilePath());
    }
    const QString name = QFileInfo(module).fileName();
    QStringList hits;
    for (const QString& f : files)
        if (QFileInfo(f).fileName().compare(name, kPathCase) == 0) hits << f;
    if (hits.size() != 1) return fail(QStringLiteral("the package does not hold %1: not installed").arg(name));
    const QString rel = hits.front();
    const QString mod = QDir::fromNativeSeparators(module);
    if (!mod.endsWith(QLatin1Char('/') + rel, kPathCase))
        return fail(QStringLiteral("this Rikugan (%1) is not where the package puts it (%2): install the update with the "
                                   "installer instead").arg(mod, rel));
    Plan p;
    p.root = mod.left(mod.size() - rel.size());
    p.files = files;
    p.files.sort();
    return p;
}

bool install(const Release& release, const QString& module, const QString& work, QString* error,
             const std::function<void(const QString&)>& progress) {
    auto say = [&](const QString& s) {
        if (progress) progress(s);
    };
    auto fail = [&](const QString& why) {
        if (error) *error = why;
        return false;
    };
    QDir(work).removeRecursively();
    if (!QDir().mkpath(work)) return fail(QStringLiteral("cannot create %1").arg(work));
    const QString archive = QDir(work).filePath(release.package.fileName());
    const QString sums = QDir(work).filePath(release.checksum.fileName());
    say(QStringLiteral("Downloading Rikugan %1\u2026").arg(release.version));
    QString err;
    if (!download(release.package, archive, &err) || !download(release.checksum, sums, &err)) return fail(err);

    say(QStringLiteral("Checking Rikugan %1\u2026").arg(release.version));
    const QString want = hashInChecksumFile(sums);
    if (want.isEmpty()) return fail(QStringLiteral("%1 holds no SHA-256: not installed").arg(release.checksum.fileName()));
    const QString got = sha256OfFile(archive);
    if (got != want)
        return fail(QStringLiteral("%1 does not match its SHA-256 (got %2, the release says %3): not installed")
                        .arg(release.package.fileName(), got, want));
    const QString unpacked = QDir(work).filePath(QStringLiteral("unpacked"));
    QDir().mkpath(unpacked);
    if (!run("tar", {archive.endsWith(QLatin1String(".zip")) ? QStringLiteral("-xf") : QStringLiteral("-xzf"), archive,
                     QStringLiteral("-C"), unpacked}, 300000, &err))
        return fail(QStringLiteral("cannot unpack %1: %2").arg(release.package.fileName(), err));
    const std::optional<Plan> p = plan(unpacked, module, &err);
    if (!p) return fail(err);
    const QString rel = QDir::fromNativeSeparators(module).mid(p->root.size());
    const QString version = versionOfFile(QDir(unpacked).filePath(rel));
    if (version != release.version)
        return fail(QStringLiteral("the package of %1 holds Rikugan %2: not installed")
                        .arg(release.tag, version.isEmpty() ? QStringLiteral("(no version)") : version));

    say(QStringLiteral("Installing Rikugan %1\u2026").arg(release.version));
    // 1. every new file next to its place, so nothing is touched unless all of them could be written
    QStringList staged;
    auto unstage = [&] {
        for (const QString& s : staged) QFile::remove(s);
    };
    for (const QString& f : p->files) {
        const QString dst = p->root + f, tmp = dst + QLatin1String(kNew);
        QDir().mkpath(QFileInfo(dst).absolutePath());
        QFile::remove(tmp);
        if (!QFile::copy(QDir(unpacked).filePath(f), tmp)) {
            unstage();
            return fail(QStringLiteral("Rikugan cannot write to %1 (an administrator can, or run the installer)")
                            .arg(QDir::toNativeSeparators(QFileInfo(dst).absolutePath())));
        }
        QFile(tmp).setPermissions(QFile(QDir(unpacked).filePath(f)).permissions() | QFile::ReadOwner | QFile::WriteOwner);
        staged << tmp;
    }
    // 2. each installed file renamed aside (Windows lets a loaded DLL be renamed, not overwritten) and the new one put
    //    in its place; any failure puts every file back as it was
    const QString stamp = QString::number(QDateTime::currentMSecsSinceEpoch());
    QList<QPair<QString, QString>> moved;  // (place, the old file's new name)
    auto rollback = [&] {
        for (auto it = moved.crbegin(); it != moved.crend(); ++it) {
            QFile::remove(it->first);
            if (!it->second.isEmpty()) replaceFile(it->second, it->first);
        }
        unstage();
    };
    for (const QString& f : p->files) {
        const QString dst = p->root + f;
        QString old;
        if (QFileInfo::exists(dst)) {
            old = dst + QLatin1String(kOld) + QLatin1Char('-') + stamp;
            if (!replaceFile(dst, old)) {
                const QString why = systemError();
                rollback();
                return fail(QStringLiteral("cannot replace %1 (%2): close %3 and run the installer")
                                .arg(QDir::toNativeSeparators(dst), why, hostName(packageName())));
            }
        }
        moved << qMakePair(dst, old);
        if (!replaceFile(dst + QLatin1String(kNew), dst)) {
            const QString why = systemError();
            rollback();
            return fail(QStringLiteral("cannot put the new %1 in place (%2)").arg(QDir::toNativeSeparators(dst), why));
        }
    }
    // 3. the old files: gone now where the system allows it (Linux, macOS), at the next start otherwise (Windows)
    for (const auto& m : moved)
        if (!m.second.isEmpty()) QFile::remove(m.second);
    QDir(work).removeRecursively();
    return true;
}

void removeLeftovers(const QString& root) {
    for (const QString& dir : {root, root + QStringLiteral("/rikugan")}) {
        QDir d(dir);
        for (const QString& f : d.entryList({QStringLiteral("*") + QLatin1String(kOld) + QStringLiteral("*")}, QDir::Files | QDir::Hidden))
            QFile::remove(d.filePath(f));
    }
}

}  // namespace rk::update
