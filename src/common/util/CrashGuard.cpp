#include "common/util/CrashGuard.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <windows.h>
// windows.h first
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#else
#include <csignal>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#ifdef __APPLE__
#include <sys/ucontext.h>  // the context types only: <ucontext.h> wants _XOPEN_SOURCE for its deprecated routines
#else
#include <ucontext.h>
#endif
#include <unistd.h>
#endif

namespace rk::crash {

namespace {

namespace fs = std::filesystem;

// Never freed: a crash may come while static destructors run.
Hooks* g_hooks = nullptr;
std::atomic<bool> g_installed{false};
std::atomic<bool> g_handling{false};
std::terminate_handler g_prevTerminate = nullptr;

constexpr int kSaveTimeoutMs = 30000;
constexpr int kMaxFrames = 64;

fs::path u8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

FILE* openWrite(const fs::path& p) {
#ifdef _WIN32
    FILE* f = nullptr;
    return _wfopen_s(&f, p.c_str(), L"w") == 0 ? f : nullptr;
#else
    return std::fopen(p.c_str(), "w");
#endif
}

void note(const std::string& what) {
    std::fprintf(stderr, "Rikugan: %s\n", what.c_str());
    std::fflush(stderr);
}

// What happened, captured on the crashing thread.
struct Crash {
    std::string reason;
    const void* address = nullptr;  // the faulting instruction (null for std::terminate)
    std::string detail;             // e.g. "writing 0x10"
    unsigned long long threadId = 0;
    bool mainThread = false;
#ifdef _WIN32
    CONTEXT context{};
    bool hasContext = false;
#else
    void* frames[kMaxFrames]{};
    int frameCount = 0;
#endif
};

FILE* openLog(std::string* path) {
    if (!g_hooks || g_hooks->crashDir.empty()) return nullptr;
    std::error_code ec;
    fs::create_directories(u8(g_hooks->crashDir), ec);
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
    const unsigned long pid = GetCurrentProcessId();
#else
    localtime_r(&now, &tm);
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    char name[80];
    std::snprintf(name, sizeof name, "crash-log-%04d%02d%02d-%02d%02d%02d-%lu.log", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, pid);
    const fs::path p = u8(g_hooks->crashDir) / name;
    *path = utf8(p);
    return openWrite(p);
}

#ifdef _WIN32
HMODULE moduleOf(const void* address) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(address), &m);
    return m;
}

std::string moduleName(HMODULE m) {
    wchar_t w[MAX_PATH];
    const DWORD n = m ? GetModuleFileNameW(m, w, MAX_PATH) : 0;
    if (!n) return "?";
    const fs::path p(std::wstring(w, n));
    return utf8(p.filename());
}

bool ours(const void* address) {
    const HMODULE m = moduleOf(address);
    return m && (m == moduleOf(reinterpret_cast<const void*>(&openLog)) || m == GetModuleHandleW(L"libomc.dll"));
}

void writeFrame(FILE* f, int n, DWORD64 pc) {
    const HMODULE m = moduleOf(reinterpret_cast<const void*>(pc));
    std::fprintf(f, "  #%-2d 0x%016llx %s+0x%llx", n, static_cast<unsigned long long>(pc), moduleName(m).c_str(),
                 static_cast<unsigned long long>(m ? pc - reinterpret_cast<DWORD64>(m) : pc));
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 511;
    DWORD64 disp = 0;
    if (SymFromAddr(GetCurrentProcess(), pc, &disp, sym)) std::fprintf(f, "  %s+0x%llx", sym->Name, static_cast<unsigned long long>(disp));
    std::fputc('\n', f);
}

// The crashing thread's stack from its saved context (that thread waits in the handler meanwhile). dbghelp looks
// for .pdb files next to each module.
void writeStack(FILE* f, const Crash& c) {
    if (!c.hasContext) return;
    const HANDLE proc = GetCurrentProcess();
    SymSetOptions(SymGetOptions() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
    const bool mine = SymInitializeW(proc, nullptr, TRUE) != FALSE;  // false: the host already initialized it
    const HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, DWORD(c.threadId));
    CONTEXT ctx = c.context;
    STACKFRAME64 sf{};
    sf.AddrPC.Offset = ctx.Rip;
    sf.AddrStack.Offset = ctx.Rsp;
    sf.AddrFrame.Offset = ctx.Rbp;
    sf.AddrPC.Mode = sf.AddrStack.Mode = sf.AddrFrame.Mode = AddrModeFlat;
    std::fprintf(f, "\nStack:\n");
    for (int n = 0; n < kMaxFrames; ++n) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread ? thread : GetCurrentThread(), &sf, &ctx, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
            sf.AddrPC.Offset == 0)
            break;
        writeFrame(f, n, sf.AddrPC.Offset);
    }
    if (thread) CloseHandle(thread);
    if (mine) SymCleanup(proc);
}

void writeWhere(FILE* f, const Crash& c) {
    if (!c.address) return;
    const HMODULE m = moduleOf(c.address);
    std::fprintf(f, "Address: %p (%s+0x%llx)\n", c.address, moduleName(m).c_str(),
                 static_cast<unsigned long long>(m ? reinterpret_cast<uintptr_t>(c.address) - reinterpret_cast<uintptr_t>(m) : 0));
}
#else
void* pcOf(void* context) {
    auto* uc = static_cast<ucontext_t*>(context);
#if defined(__APPLE__) && defined(__aarch64__)
    return reinterpret_cast<void*>(uc->uc_mcontext->__ss.__pc);
#elif defined(__APPLE__) && defined(__x86_64__)
    return reinterpret_cast<void*>(uc->uc_mcontext->__ss.__rip);
#elif defined(__linux__) && defined(__aarch64__)
    return reinterpret_cast<void*>(uc->uc_mcontext.pc);
#elif defined(__linux__) && defined(__x86_64__)
    return reinterpret_cast<void*>(uc->uc_mcontext.gregs[REG_RIP]);
#else
    (void)uc;
    return nullptr;
#endif
}

bool ours(const void* pc) {
    Dl_info at{}, self{};
    return pc && dladdr(pc, &at) && dladdr(reinterpret_cast<void*>(&openLog), &self) && at.dli_fbase == self.dli_fbase;
}

void writeWhere(FILE* f, const Crash& c) {
    if (!c.address) return;
    Dl_info at{};
    if (dladdr(c.address, &at) && at.dli_fname) {
        std::fprintf(f, "Address: %p (%s+0x%lx", c.address, fs::path(at.dli_fname).filename().c_str(),
                     static_cast<unsigned long>(reinterpret_cast<uintptr_t>(c.address) - reinterpret_cast<uintptr_t>(at.dli_fbase)));
        if (at.dli_sname)
            std::fprintf(f, " %s+0x%lx", at.dli_sname,
                         static_cast<unsigned long>(reinterpret_cast<uintptr_t>(c.address) - reinterpret_cast<uintptr_t>(at.dli_saddr)));
        std::fprintf(f, ")\n");
    } else {
        std::fprintf(f, "Address: %p\n", c.address);
    }
}

void writeStack(FILE* f, const Crash& c) {
    if (c.frameCount <= 0) return;
    std::fprintf(f, "\nStack:\n");
    std::fflush(f);
    backtrace_symbols_fd(const_cast<void* const*>(c.frames), c.frameCount, fileno(f));
}
#endif

// The log, then the recovery copies (on the main thread: directly when it is the crashing one, else queued there,
// since it never waits on Rikugan's threads), then their paths in the log. Once per process.
void handle(const Crash& c, bool save) {
    if (!g_installed.load() || !g_hooks || g_handling.exchange(true)) return;
    const Hooks& h = *g_hooks;
    std::string logPath;
    FILE* f = openLog(&logPath);
    if (f) {
        std::fprintf(f, "Rikugan crash report\n%s\n\n", h.version.c_str());
        std::fprintf(f, "Reason: %s\n", c.reason.c_str());
        if (!c.detail.empty()) std::fprintf(f, "Detail: %s\n", c.detail.c_str());
        writeWhere(f, c);
        std::fprintf(f, "Thread: %llu%s\n", c.threadId, c.mainThread ? " (host main thread)" : "");
        writeStack(f, c);
        std::fflush(f);
    }
    note("crashed" + (logPath.empty() ? std::string() : "; crash log: " + logPath));
    std::string saved;
    if (!save) {
        saved = "not attempted (the main thread's stack is exhausted)";
    } else if (c.mainThread) {
        try {
            saved = h.saveOnMain();
        } catch (...) {
            saved = "failed";
        }
    } else {
        struct Done {
            std::mutex mu;
            std::condition_variable cv;
            bool done = false;
            std::string result;
        };
        auto done = std::make_shared<Done>();
        try {
            h.postToMain([done, fn = h.saveOnMain] {
                std::string r;
                try {
                    r = fn();
                } catch (...) {
                    r = "failed";
                }
                std::lock_guard<std::mutex> g(done->mu);
                done->result = r;
                done->done = true;
                done->cv.notify_all();
            });
            std::unique_lock<std::mutex> lk(done->mu);
            saved = done->cv.wait_for(lk, std::chrono::milliseconds(kSaveTimeoutMs), [&] { return done->done; })
                        ? done->result
                        : "the host main thread did not run the save in time";
        } catch (...) {
            saved = "failed";
        }
    }
    if (f) {
        std::fprintf(f, "\nRecovery copies:\n%s\n", saved.empty() ? "(no database open)" : saved.c_str());
        std::fclose(f);
    }
    note("recovery copies: " + (saved.empty() ? std::string("(no database open)") : saved));
}

unsigned long long currentThreadId() {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(reinterpret_cast<void*>(pthread_self())));
#endif
}

[[noreturn]] void onTerminate() {
    Crash c;
    c.reason = "std::terminate (an exception nobody caught, or a call to std::terminate)";
    if (std::exception_ptr e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& ex) {
            c.detail = std::string("uncaught exception: ") + ex.what();
        } catch (...) {
            c.detail = "uncaught exception of an unknown type";
        }
    }
    c.threadId = currentThreadId();
    c.mainThread = g_hooks && g_hooks->isMainThread && g_hooks->isMainThread();
#ifdef _WIN32
    RtlCaptureContext(&c.context);
    c.hasContext = true;
#else
    c.frameCount = backtrace(c.frames, kMaxFrames);
#endif
    handle(c, true);
    if (g_prevTerminate && g_prevTerminate != &onTerminate) g_prevTerminate();
    std::abort();
}

// Runs handle() on a fresh thread and waits: the crashing thread's stack may be exhausted or corrupted.
void handleElsewhere(const Crash& c) {
    std::thread t([&c] { handle(c, true); });
    t.join();
}

#ifdef _WIN32
PVOID g_vectored = nullptr;

const char* codeName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer division by zero";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    default: return nullptr;
    }
}

// First-chance: Rikugan has no __try handlers, so one of these inside it is a crash.
LONG CALLBACK onException(EXCEPTION_POINTERS* e) {
    const EXCEPTION_RECORD* r = e->ExceptionRecord;
    const char* name = codeName(r->ExceptionCode);
    if (!name || !ours(r->ExceptionAddress) || g_handling.load()) return EXCEPTION_CONTINUE_SEARCH;
    auto c = std::make_unique<Crash>();
    char reason[96];
    std::snprintf(reason, sizeof reason, "%s (exception 0x%08lx)", name, static_cast<unsigned long>(r->ExceptionCode));
    c->reason = reason;
    c->address = r->ExceptionAddress;
    if ((r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || r->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) && r->NumberParameters >= 2) {
        const ULONG_PTR op = r->ExceptionInformation[0];
        char d[64];
        std::snprintf(d, sizeof d, "%s 0x%llx", op == 0 ? "reading" : op == 1 ? "writing" : "executing",
                      static_cast<unsigned long long>(r->ExceptionInformation[1]));
        c->detail = d;
    }
    c->threadId = GetCurrentThreadId();
    c->mainThread = g_hooks && g_hooks->isMainThread();
    c->context = *e->ContextRecord;
    c->hasContext = true;
    const bool overflow = r->ExceptionCode == EXCEPTION_STACK_OVERFLOW;
    if (c->mainThread && !overflow) handle(*c, true);
    else if (c->mainThread) {  // no stack left to save with, and no other thread can: the log only
        std::thread t([&c] { handle(*c, false); });
        t.join();
    } else {
        handleElsewhere(*c);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
struct sigaction g_old[sizeof kSignals / sizeof kSignals[0]];

void restore(int sig) {
    for (size_t i = 0; i < sizeof kSignals / sizeof kSignals[0]; ++i)
        if (kSignals[i] == sig) sigaction(sig, &g_old[i], nullptr);
}

// Puts the previous handler back afterwards: returning re-runs the instruction, which faults again into it (or into
// the default action). libomc is linked into the same module, so "ours" covers it.
void onSignal(int sig, siginfo_t* info, void* context) {
    void* pc = pcOf(context);
    if (ours(pc) && !g_handling.load()) {
        Crash c;
        char reason[64];
        std::snprintf(reason, sizeof reason, "%s (signal %d, code %d)",
                      sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGILL ? "SIGILL" : "SIGFPE", sig,
                      info ? info->si_code : 0);
        c.reason = reason;
        c.address = pc;
        if (info && (sig == SIGSEGV || sig == SIGBUS)) {
            char d[48];
            std::snprintf(d, sizeof d, "at %p", info->si_addr);
            c.detail = d;
        }
        c.threadId = currentThreadId();
        c.mainThread = g_hooks && g_hooks->isMainThread();
        c.frameCount = backtrace(c.frames, kMaxFrames);
        if (c.mainThread) handle(c, true);
        else handleElsewhere(c);
    }
    restore(sig);
}
#endif

void reportPrevious(const Hooks& h) {
    if (!h.reportPrevious || h.crashDir.empty()) return;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(u8(h.crashDir), ec)) {
        const fs::path p = e.path();
        const std::string name = utf8(p.filename());
        if (name.rfind("crash-log-", 0) != 0 || p.extension() != ".log") continue;
        fs::path seen = p;
        seen += ".seen";
        if (fs::exists(seen, ec)) continue;
        h.reportPrevious(utf8(p));
        if (FILE* f = openWrite(seen)) std::fclose(f);
    }
}

}  // namespace

void install(Hooks hooks) {
    if (g_installed.load()) return;
    if (!g_hooks) g_hooks = new Hooks;
    *g_hooks = std::move(hooks);
    reportPrevious(*g_hooks);
#ifdef _WIN32
    g_vectored = AddVectoredExceptionHandler(1, onException);
#else
    for (size_t i = 0; i < sizeof kSignals / sizeof kSignals[0]; ++i) {
        struct sigaction sa {};
        sa.sa_sigaction = onSignal;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(kSignals[i], &sa, &g_old[i]);
    }
#endif
    g_prevTerminate = std::set_terminate(onTerminate);
    g_installed = true;
}

void uninstall() {
    if (!g_installed.exchange(false)) return;
#ifdef _WIN32
    if (g_vectored) RemoveVectoredExceptionHandler(g_vectored);
    g_vectored = nullptr;
#else
    for (size_t i = 0; i < sizeof kSignals / sizeof kSignals[0]; ++i) sigaction(kSignals[i], &g_old[i], nullptr);
#endif
    std::set_terminate(g_prevTerminate);
}

void guardThread() {
    if (g_installed.load()) std::set_terminate(onTerminate);
}

std::string recoveryPath(const std::string& path, const std::string& ext) {
    std::string base = path;
    const std::string dotExt = "." + ext;
    if (base.size() > dotExt.size() && base.compare(base.size() - dotExt.size(), dotExt.size(), dotExt) == 0)
        base.resize(base.size() - dotExt.size());
    return base + ".rikugan-recovered" + dotExt;
}

}  // namespace rk::crash
