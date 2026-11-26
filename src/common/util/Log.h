#pragma once
// Messages for the host's output window (IDA "Output", Binary Ninja "Log"). The host installs a sink at init;
// without one, messages go to stderr. Any thread.
#include <functional>

#include <QString>

namespace rk {

void setLogSink(std::function<void(const QString&)> sink);
void log(const QString& message);
// Logs `message` the first time `key` is seen in this process.
void logOnce(const QString& key, const QString& message);

}  // namespace rk
