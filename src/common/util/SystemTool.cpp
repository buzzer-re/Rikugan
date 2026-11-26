#include "common/util/SystemTool.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace rk {

QString systemTool(const char* name) {
#ifdef _WIN32
    const QString sys = QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows")))
                            .filePath(QStringLiteral("System32/%1.exe").arg(QLatin1String(name)));
#else
    const QString sys = QStringLiteral("/usr/bin/%1").arg(QLatin1String(name));
#endif
    if (QFileInfo(sys).isExecutable()) return sys;
    return QStandardPaths::findExecutable(QString::fromLatin1(name));
}

}  // namespace rk
