#pragma once
// The file of the module this code is linked into: the plugin (librikugan.so, rikugan.dll ...), or the test executable.
#include <QString>

namespace rk {

// Absolute path with '/' separators; empty when the system cannot tell.
QString modulePath();

}  // namespace rk
