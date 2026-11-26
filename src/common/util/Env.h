#pragma once
// Process environment shared with libomc. On Windows libomc.dll runs on its own C runtime (msvcrt.dll), whose getenv
// reads a copy of the environment taken when that runtime loaded: a variable set only through the plugin's runtime
// or the Win32 environment never reaches omc. These helpers update every copy.
#include <QString>

namespace rk {

// Sets `name` (UTF-8 value) for this process, its children and libomc.
void setEnv(const char* name, const QString& value);

// Removes `name` for this process, its children and libomc.
void unsetEnv(const char* name);

// The user's home as omc resolves it: $HOME when set (Linux, macOS, and Windows shells that export it), else the
// platform's home (Windows: the user profile).
QString homeDir();

}  // namespace rk
