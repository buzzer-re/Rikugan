#pragma once
// Loading libomc before its first use. Linux and macOS link it into the plugin, so there is nothing to do. On Windows
// libomc.dll ships with the plugin (next to it, or in a "rikugan" folder beside it) and is delay-loaded
// (cmake/omc.cmake): the hosts load plugins from folders the Windows loader does not search, so the plugin loads it
// by full path, after which the delay-load helper resolves every omc_* call to that module.
#include <QString>

namespace rk {

// Host main thread, at plugin init, before any omc_* call. False (with *error) when libomc cannot be loaded or speaks
// another API version than omc.h; the plugin must then not start.
bool loadOmcLibrary(QString* error);

}  // namespace rk
