#pragma once
// Results of read-only listing tools, per database, until the next mutation.
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>

#include <QJsonObject>
#include <QString>

namespace rk {

class ResultCache {
public:
    static bool cacheable(const QString& tool);  // list_functions, list_strings, get_binary_info, decompile_function, function_xrefs
    std::optional<QString> get(const QString& tool, const QJsonObject& args);
    void put(const QString& tool, const QJsonObject& args, const QString& result);
    void invalidate();

private:
    static constexpr size_t kMax = 64;
    std::string key(const QString& tool, const QJsonObject& args) const;
    std::mutex mu_;
    std::list<std::pair<std::string, QString>> lru_;  // front = newest
    std::unordered_map<std::string, std::list<std::pair<std::string, QString>>::iterator> index_;
};

}  // namespace rk
