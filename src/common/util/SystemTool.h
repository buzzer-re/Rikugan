#pragma once
// Tools the operating system ships, run by the updater and the connection test.
#include <QString>

namespace rk {

// The System32 copy on Windows (curl.exe and tar.exe ship with Windows 10 and later), /usr/bin's elsewhere, else the
// first one on PATH. Empty when there is none.
QString systemTool(const char* name);

}  // namespace rk
