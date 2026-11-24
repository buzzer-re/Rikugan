#include "common/util/Log.h"

#include <cstdio>
#include <mutex>

#include <QSet>

namespace rk {

namespace {
std::mutex g_mu;
std::function<void(const QString&)> g_sink;
QSet<QString> g_seen;
}  // namespace

void setLogSink(std::function<void(const QString&)> sink) {
    std::lock_guard<std::mutex> g(g_mu);
    g_sink = std::move(sink);
}

void log(const QString& message) {
    std::function<void(const QString&)> sink;
    {
        std::lock_guard<std::mutex> g(g_mu);
        sink = g_sink;
    }
    if (sink) sink(message);
    else std::fprintf(stderr, "%s\n", message.toUtf8().constData());
}

void logOnce(const QString& key, const QString& message) {
    {
        std::lock_guard<std::mutex> g(g_mu);
        if (g_seen.contains(key)) return;
        g_seen.insert(key);
    }
    log(message);
}

}  // namespace rk
