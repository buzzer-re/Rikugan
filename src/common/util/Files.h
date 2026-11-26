#pragma once
// Small file helpers shared by every module.
#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace rk {

// Writes `data` to a temporary file next to `path` and renames it over `path`. `mode` is a POSIX
// permission (0 keeps the default umask result). Creates missing parent directories.
bool writeFileAtomic(const QString& path, const QByteArray& data, int mode = 0, QString* error = nullptr);

// Whole file, or an empty array (and *ok = false) when it can't be read.
QByteArray readFile(const QString& path, bool* ok = nullptr);

// A JSON object from a file; an empty object when the file is missing or isn't an object.
QJsonObject readJsonObject(const QString& path, bool* ok = nullptr);

bool ensureDir(const QString& path);

// "~/x" -> "$HOME/x"
QString expandHome(const QString& path);

// One-line compact JSON.
inline QByteArray compactJson(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }

}  // namespace rk
