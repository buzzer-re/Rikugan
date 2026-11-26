#include "common/util/Env.h"

#include <cstdlib>

#include <QDir>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rk {

#ifdef _WIN32
namespace {

// libomc's runtime is msvcrt.dll (omc's MinGW-w64 build); its _putenv_s updates the table omc's getenv reads. The
// value is UTF-8, which is what omc expects in its environment.
void msvcrtPut(const char* name, const char* value) {
    using PutEnv = int(__cdecl*)(const char*, const char*);
    static const PutEnv put = [] {
        HMODULE crt = LoadLibraryW(L"msvcrt.dll");
        return crt ? reinterpret_cast<PutEnv>(reinterpret_cast<void*>(GetProcAddress(crt, "_putenv_s"))) : nullptr;
    }();
    if (put) put(name, value);
}

}  // namespace

void setEnv(const char* name, const QString& value) {
    msvcrtPut(name, value.toUtf8().constData());
    // last, so the Win32 environment (children, the plugin's own runtime) holds the exact UTF-16 value
    _wputenv_s(reinterpret_cast<const wchar_t*>(QString::fromLatin1(name).utf16()),
               reinterpret_cast<const wchar_t*>(value.utf16()));
}

void unsetEnv(const char* name) {
    msvcrtPut(name, "");
    _wputenv_s(reinterpret_cast<const wchar_t*>(QString::fromLatin1(name).utf16()), L"");
}
#else
void setEnv(const char* name, const QString& value) { ::setenv(name, value.toUtf8().constData(), 1); }

void unsetEnv(const char* name) { ::unsetenv(name); }
#endif

QString homeDir() {
    const QString home = qEnvironmentVariable("HOME");
    return home.isEmpty() ? QDir::homePath() : QDir::fromNativeSeparators(home);
}

}  // namespace rk
