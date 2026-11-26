#pragma once
// What must be gone before the host unloads the plugin. IDA dlcloses rikugan.so at exit while its Qt event loop still
// runs, so nothing may be left that runs plugin code afterwards: no thread still inside it, no posted event or timer
// whose receiver or slot lives in it.
#include <functional>

#include <QObject>

namespace rk {

// A plugin-owned QObject on the UI thread: the context for results posted from worker threads and the parent of
// module-wide timers, instead of qApp. Callable from any thread.
QObject* uiAnchor();

// Starts a thread the module waits for at unload; use instead of std::thread(...).detach().
void spawnThread(std::function<void()> fn);

// Unload: waits up to `waitMs` for spawned threads, joins them, then deletes the anchor (and with it every event posted
// to it). Call on the UI thread once nothing starts new threads.
void shutdownModule(int waitMs);

}  // namespace rk
