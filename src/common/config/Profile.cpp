#include "common/config/Profile.h"

#include "common/config/RikuganConfig.h"

namespace rk {

namespace {

QStringList strings(const QJsonValue& v) {
    QStringList out;
    for (const QJsonValue& x : v.toArray())
        if (x.isString()) out << x.toString();
    return out;
}

}  // namespace

const QList<QPair<QString, QString>>& iocFilterCategories() {
    static const QList<QPair<QString, QString>> cats{
        {"hashes", "File hashes (MD5, SHA1, SHA256)"},
        {"ipv4", "IPv4 addresses"},
        {"ipv6", "IPv6 addresses"},
        {"domains", "Domain names"},
        {"urls", "URLs (http/https/ftp)"},
        {"registry_keys", "Windows registry keys"},
        {"file_paths", "File paths (Windows & Unix)"},
        {"emails", "Email addresses"},
        {"crypto_wallets", "Cryptocurrency wallet addresses"},
        {"mutexes", "Mutex / named object names"},
    };
    return cats;
}

bool AnalysisProfile::hasAnyIocFilter() const {
    for (bool on : iocFilters)
        if (on) return true;
    return !customFilterRules.isEmpty();
}

QJsonObject AnalysisProfile::toJson() const {
    QJsonObject ioc;
    for (auto it = iocFilters.begin(); it != iocFilters.end(); ++it) ioc.insert(it.key(), it.value());
    return QJsonObject{{"name", name},
                       {"description", description},
                       {"denied_tools", QJsonArray::fromStringList(deniedTools)},
                       {"denied_functions", QJsonArray::fromStringList(deniedFunctions)},
                       {"custom_filters", QJsonArray::fromStringList(customFilters)},
                       {"hide_binary_metadata", hideBinaryMetadata},
                       {"ioc_filters", ioc},
                       {"custom_filter_rules", customFilterRules},
                       {"singular_analysis", singularAnalysis}};
}

AnalysisProfile AnalysisProfile::fromJson(const QString& name, const QJsonObject& o) {
    AnalysisProfile p;
    p.name = name;
    p.description = o.value(QStringLiteral("description")).toString();
    p.deniedTools = strings(o.value(QStringLiteral("denied_tools")));
    p.deniedFunctions = strings(o.value(QStringLiteral("denied_functions")));
    p.customFilters = strings(o.value(QStringLiteral("custom_filters")));
    p.hideBinaryMetadata = o.value(QStringLiteral("hide_binary_metadata")).toBool();
    const QJsonObject ioc = o.value(QStringLiteral("ioc_filters")).toObject();
    for (auto it = ioc.begin(); it != ioc.end(); ++it) p.iocFilters.insert(it.key(), it.value().toBool());
    // old configs: filter_iocs_in_data turned every category on
    if (o.value(QStringLiteral("filter_iocs_in_data")).toBool() && ioc.isEmpty())
        for (const auto& c : iocFilterCategories()) p.iocFilters.insert(c.first, true);
    p.customFilterRules = o.value(QStringLiteral("custom_filter_rules")).toArray();
    p.singularAnalysis = o.value(QStringLiteral("singular_analysis")).toBool();
    return p;
}

AnalysisProfile defaultProfile() {
    AnalysisProfile p;
    p.name = QStringLiteral("default");
    p.description = QStringLiteral("Standard analysis mode");
    p.builtin = true;
    return p;
}

AnalysisProfile privateProfile() {
    AnalysisProfile p;
    p.name = QStringLiteral("private");
    p.description = QStringLiteral("Private malware analysis — no metadata or IOCs leak");
    p.hideBinaryMetadata = true;
    for (const auto& c : iocFilterCategories()) p.iocFilters.insert(c.first, true);
    p.singularAnalysis = true;
    p.builtin = true;
    return p;
}

AnalysisProfile getProfile(const QString& name, const QJsonObject& customProfiles) {
    if (name == QLatin1String("default")) return defaultProfile();
    if (name == QLatin1String("private")) return privateProfile();
    if (customProfiles.value(name).isObject()) return AnalysisProfile::fromJson(name, customProfiles.value(name).toObject());
    return defaultProfile();
}

QList<AnalysisProfile> listProfiles(const QJsonObject& customProfiles) {
    QList<AnalysisProfile> out{defaultProfile(), privateProfile()};
    for (auto it = customProfiles.begin(); it != customProfiles.end(); ++it) {
        if (it.key() == QLatin1String("default") || it.key() == QLatin1String("private")) continue;
        if (it.value().isObject()) out << AnalysisProfile::fromJson(it.key(), it.value().toObject());
    }
    return out;
}

AnalysisProfile activeProfile() {
    const RikuganConfig& c = RikuganConfig::instance();
    return getProfile(c.activeProfile, c.customProfiles);
}

}  // namespace rk
