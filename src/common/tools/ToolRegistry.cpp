#include "common/tools/ToolRegistry.h"

#include <QJsonObject>

#include "common/config/Profile.h"

namespace rk {

void ToolRegistry::add(ToolSpec spec) {
    std::lock_guard<std::mutex> g(mu_);
    const QString name = spec.name;
    auto it = specs_.find(name);
    if (it != specs_.end()) *it->second = std::move(spec);  // replaced in place: pointers stay valid
    else specs_.emplace(name, std::make_unique<ToolSpec>(std::move(spec)));
}

const ToolSpec* ToolRegistry::find(const QString& name) const {
    std::lock_guard<std::mutex> g(mu_);
    auto it = specs_.find(name);
    return it == specs_.end() ? nullptr : it->second.get();
}

QStringList ToolRegistry::names() const {
    std::lock_guard<std::mutex> g(mu_);
    QStringList out;
    for (const auto& [n, s] : specs_) out << n;
    return out;
}

std::vector<const ToolSpec*> ToolRegistry::all() const {
    std::lock_guard<std::mutex> g(mu_);
    std::vector<const ToolSpec*> out;
    for (const auto& [n, s] : specs_) out.push_back(s.get());
    return out;
}

std::vector<const ToolSpec*> ToolRegistry::visible(const AnalysisProfile& profile, bool hasDecompiler,
                                                   const std::function<bool(const QString&)>& include) const {
    std::vector<const ToolSpec*> out;
    for (const ToolSpec* s : all()) {
        if (s->has(Internal)) continue;
        if (profile.deniedTools.contains(s->name)) continue;
        if (s->has(RequiresDecompiler) && !hasDecompiler) continue;
        if (include && !include(s->name)) continue;
        out.push_back(s);
    }
    return out;
}

QJsonArray ToolRegistry::hostToolsJson(const std::vector<const ToolSpec*>& specs) {
    QJsonArray tools;
    for (const ToolSpec* s : specs)
        tools.append(QJsonObject{{"name", s->name},
                                 {"description", s->description},
                                 {"parameters", s->schema()},
                                 {"label", s->name},
                                 {"exclusive", s->exclusive()}});
    return tools;
}

}  // namespace rk
