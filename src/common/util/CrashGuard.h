#pragma once
// What Rikugan does when it crashes. A fault whose instruction is inside Rikugan or libomc (access violation,
// illegal instruction, bus error, division by zero) or a std::terminate on a Rikugan thread:
//   1. writes a crash log, <crashDir>/crash-log-<YYYYMMDD-HHMMSS>-<pid>.log (~/.rikugan/logs): what happened, where
//      (module + offset), the thread, the versions and the faulting thread's stack (symbol names when the debug
//      information is next to the module);
//   2. writes a recovery copy of every open database (`<database>.rikugan-recovered.<ext>`, next to it; the database
//      file itself is left as it was last saved) and appends their paths to the log;
//   3. hands the crash on to whatever handled it before (the host's crash reporter, or the default action).
// Faults in the host's own code are not touched. The next start reports crash logs not seen yet.
#include <functional>
#include <string>

namespace rk::crash {

struct Hooks {
    std::string crashDir;  // UTF-8 (~/.rikugan/logs); created when a crash is written
    std::string version;   // "Rikugan 2.0.0, libomc 19.1.0, IDA 9.2 ..."
    // Host main thread: writes the recovery copies and returns a line per database (the path, or why it failed).
    // Called at most once per process.
    std::function<std::string()> saveOnMain;
    // Any thread: queues `job` on the host main thread and returns at once.
    std::function<void(std::function<void()> job)> postToMain;
    std::function<bool()> isMainThread;
    // At install, once per crash log written by an earlier run and not reported yet (its path).
    std::function<void(const std::string& logPath)> reportPrevious;
};

// Host main thread, at plugin init.
void install(Hooks hooks);
// Host main thread, at unload (IDA unloads the module: no handler may point into it afterwards).
void uninstall();
// At the start of every thread Rikugan starts: a std::terminate there is handled too (MSVC keeps the terminate
// handler per thread).
void guardThread();

// The recovery copy's path for a database or input file, saved as `ext`: ("dir/x.i64", "i64") ->
// "dir/x.rikugan-recovered.i64", ("dir/sample.exe", "bndb") -> "dir/sample.exe.rikugan-recovered.bndb".
std::string recoveryPath(const std::string& path, const std::string& ext);

}  // namespace rk::crash
