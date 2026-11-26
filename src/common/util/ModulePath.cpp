#include "common/util/ModulePath.h"

#include <iterator>

#include <QDir>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace rk {

QString modulePath() {
#ifdef _WIN32
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&modulePath), &self))
        return {};
    wchar_t path[32768];
    const DWORD n = GetModuleFileNameW(self, path, DWORD(std::size(path)));
    if (n == 0 || n >= std::size(path)) return {};
    return QDir::fromNativeSeparators(QString::fromWCharArray(path, int(n)));
#else
    Dl_info at{};
    if (!dladdr(reinterpret_cast<void*>(&modulePath), &at) || !at.dli_fname) return {};
    return QDir(QString::fromLocal8Bit(at.dli_fname)).absolutePath();
#endif
}

}  // namespace rk
