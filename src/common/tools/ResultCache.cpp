#include "common/tools/ResultCache.h"

#include <QSet>

#include "common/util/Files.h"

namespace rk {

bool ResultCache::cacheable(const QString& tool) {
    static const QSet<QString> s{"list_functions", "list_strings", "get_binary_info", "decompile_function", "function_xrefs"};
    return s.contains(tool);
}

std::string ResultCache::key(const QString& tool, const QJsonObject& args) const {
    // QJsonObject keeps keys sorted: the compact form is canonical
    return tool.toStdString() + '\0' + compactJson(args).toStdString();
}

std::optional<QString> ResultCache::get(const QString& tool, const QJsonObject& args) {
    if (!cacheable(tool)) return std::nullopt;
    std::lock_guard<std::mutex> g(mu_);
    auto it = index_.find(key(tool, args));
    if (it == index_.end()) return std::nullopt;
    lru_.splice(lru_.begin(), lru_, it->second);
    return it->second->second;
}

void ResultCache::put(const QString& tool, const QJsonObject& args, const QString& result) {
    if (!cacheable(tool)) return;
    std::lock_guard<std::mutex> g(mu_);
    const std::string k = key(tool, args);
    auto it = index_.find(k);
    if (it != index_.end()) {
        it->second->second = result;
        lru_.splice(lru_.begin(), lru_, it->second);
        return;
    }
    lru_.emplace_front(k, result);
    index_[k] = lru_.begin();
    if (lru_.size() > kMax) {
        index_.erase(lru_.back().first);
        lru_.pop_back();
    }
}

void ResultCache::invalidate() {
    std::lock_guard<std::mutex> g(mu_);
    lru_.clear();
    index_.clear();
}

}  // namespace rk
