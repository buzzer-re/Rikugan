#include "common/config/Paths.h"

#include <QDir>

#include "common/config/RikuganConfig.h"
#include "common/util/Files.h"

namespace rk::paths {

QString home() {
    QString env = qEnvironmentVariable("RIKUGAN_HOME");
    return env.isEmpty() ? QDir::homePath() + QStringLiteral("/.rikugan") : expandHome(env);
}

QString configFile() { return home() + QStringLiteral("/config.json"); }
QString omcConfigDir() { return home() + QStringLiteral("/omc"); }
QString omcAgentDir() { return omcConfigDir() + QStringLiteral("/agent"); }

QString sessionsRoot() {
    const QString custom = RikuganConfig::instance().sessionStorageDir;
    return custom.isEmpty() ? home() + QStringLiteral("/sessions") : expandHome(custom);
}

QString sessionsDir(const QString& dbId) { return sessionsRoot() + QLatin1Char('/') + dbId; }
QString scratchDir(const QString& dbId) { return home() + QStringLiteral("/scratch/") + dbId; }
QString artifactsDir(const QString& dbId) { return scratchDir(dbId) + QStringLiteral("/artifacts"); }

}  // namespace rk::paths
