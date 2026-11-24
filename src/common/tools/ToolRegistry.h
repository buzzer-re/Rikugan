#pragma once
// The host tools of one database: the host's analysis tools plus Rikugan's chat tools (ask_user, ...).
#include <functional>
#include <map>
#include <memory>
#include <mutex>

#include <QJsonArray>
#include <QString>
#include <QStringList>

#include "common/tools/Tool.h"

namespace rk {

struct AnalysisProfile;

class ToolRegistry {
public:
    void add(ToolSpec spec);
    // nullptr when unknown. Specs are never removed, so the pointer stays valid.
    const ToolSpec* find(const QString& name) const;
    QStringList names() const;
    std::vector<const ToolSpec*> all() const;

    // What the model sees: not Internal, not denied by the profile, decompiler tools only with a decompiler,
    // and `include(name)` true (per-chat visibility of chat tools).
    std::vector<const ToolSpec*> visible(const AnalysisProfile& profile, bool hasDecompiler,
                                         const std::function<bool(const QString&)>& include = {}) const;
    // set_host_tools "tools" for those specs.
    static QJsonArray hostToolsJson(const std::vector<const ToolSpec*>& specs);

private:
    mutable std::mutex mu_;
    std::map<QString, std::unique_ptr<ToolSpec>> specs_;
};

}  // namespace rk
