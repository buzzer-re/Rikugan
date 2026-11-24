#include "common/omc/OmcLibrary.h"

#include <iterator>

#include <omc.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace rk {

#ifdef _WIN32
namespace {

// The folder of the module this code is linked into (the plugin DLL, or the test executable).
QString moduleDir() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&loadOmcLibrary), &self))
        return {};
    wchar_t path[32768];
    const DWORD n = GetModuleFileNameW(self, path, DWORD(std::size(path)));
    if (n == 0 || n >= std::size(path)) return {};
    const QString file = QString::fromWCharArray(path, int(n));
    return file.left(file.lastIndexOf(QLatin1Char('\\')));
}

}  // namespace

bool loadOmcLibrary(QString* error) {
    static bool loaded = false;
    if (!loaded) {
        // IDA: plugins\rikugan\{rikugan.dll, libomc.dll} (a plugin folder). Binary Ninja loads every DLL at the top of
        // its plugins folder as a plugin, so libomc.dll goes one level down: plugins\rikugan.dll + plugins\rikugan\.
        const QString dir = moduleDir();
        QString tried;
        for (const QString& dll : {dir + QStringLiteral("\\libomc.dll"), dir + QStringLiteral("\\rikugan\\libomc.dll")}) {
            if (GetFileAttributesW(reinterpret_cast<LPCWSTR>(dll.utf16())) == INVALID_FILE_ATTRIBUTES) {
                tried += (tried.isEmpty() ? QString() : QStringLiteral(", ")) + dll;
                continue;
            }
            // LOAD_WITH_ALTERED_SEARCH_PATH: libomc.dll's own imports (system DLLs only) resolve from its folder first
            if (!LoadLibraryExW(reinterpret_cast<LPCWSTR>(dll.utf16()), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
                if (error) *error = QStringLiteral("cannot load %1 (Windows error %2)").arg(dll).arg(GetLastError());
                return false;
            }
            loaded = true;  // kept for the life of the process: omc's threads may outlive the plugin's unload
            break;
        }
        if (!loaded) {
            if (error) *error = QStringLiteral("libomc.dll not found (looked for %1)").arg(tried);
            return false;
        }
    }
    if (omc_api_version() != OMC_API_VERSION) {
        if (error)
            *error = QStringLiteral("libomc.dll speaks API version %1, this plugin was built for %2")
                         .arg(omc_api_version())
                         .arg(OMC_API_VERSION);
        return false;
    }
    return true;
}
#else
bool loadOmcLibrary(QString*) { return true; }
#endif

}  // namespace rk
