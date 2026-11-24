#include "common/util/Files.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <sys/stat.h>

namespace rk {

bool ensureDir(const QString& path) { return QDir().mkpath(path); }

QString expandHome(const QString& path) {
    if (path == QLatin1String("~")) return QDir::homePath();
    if (path.startsWith(QLatin1String("~/"))) return QDir::homePath() + path.mid(1);
    return path;
}

bool writeFileAtomic(const QString& path, const QByteArray& data, int mode, QString* error) {
    ensureDir(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    if (mode) ::fchmod(f.handle(), mode_t(mode));
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
