#pragma once
// Where Rikugan keeps its files: ~/.rikugan/ (RIKUGAN_HOME overrides, for tests).
#include <QString>

namespace rk::paths {

QString home();                         // ~/.rikugan
QString configFile();                   // ~/.rikugan/config.json
QString omcConfigDir();                 // ~/.rikugan/omc         (OMC_CONFIG_DIR)
QString omcAgentDir();                  // ~/.rikugan/omc/agent
QString sessionsRoot();                 // config.sessionStorageDir or ~/.rikugan/sessions
QString sessionsDir(const QString& dbId);   // <sessionsRoot>/<dbId>
QString scratchDir(const QString& dbId);    // ~/.rikugan/scratch/<dbId>
QString artifactsDir(const QString& dbId);  // <scratch>/artifacts
QString logsDir();                      // ~/.rikugan/logs        (crash logs)

}  // namespace rk::paths
