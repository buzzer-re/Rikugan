#include "common/util/Files.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include "common/util/Env.h"

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace rk {

bool ensureDir(const QString& path) { return QDir().mkpath(path); }

QString expandHome(const QString& path) {
    if (path == QLatin1String("~")) return homeDir();
    if (path.startsWith(QLatin1String("~/"))) return homeDir() + path.mid(1);
    return path;
}

bool writeFileAtomic(const QString& path, const QByteArray& data, int mode, QString* error) {
    ensureDir(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
#ifndef _WIN32
    if (mode) ::fchmod(f.handle(), mode_t(mode));
#endif  // Windows: files under the user profile inherit its ACL (the user, SYSTEM and administrators only)
    if (f.write(data) != data.size() || !f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

QByteArray readFile(const QString& path, bool* ok) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (ok) *ok = false;
        return {};
    }
    if (ok) *ok = true;
    return f.readAll();
}

QJsonObject readJsonObject(const QString& path, bool* ok) {
    bool r = false;
    QByteArray data = readFile(path, &r);
    QJsonParseError err{};
    QJsonDocument doc = r ? QJsonDocument::fromJson(data, &err) : QJsonDocument();
    bool good = r && err.error == QJsonParseError::NoError && doc.isObject();
    if (ok) *ok = good;
    return good ? doc.object() : QJsonObject();
}

}  // namespace rk
