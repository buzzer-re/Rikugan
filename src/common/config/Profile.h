#pragma once
// Analysis profiles: what data reaches the model and which tools it may use.
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

namespace rk {

// IOC filter category key -> label (IOC_FILTER_CATEGORIES, same order).
const QList<QPair<QString, QString>>& iocFilterCategories();

struct AnalysisProfile {
    QString name;
    QString description;
    QStringList deniedTools;
    QStringList deniedFunctions;
    QStringList customFilters;
    bool hideBinaryMetadata = false;
    QMap<QString, bool> iocFilters;
    QJsonArray customFilterRules;  // [{name, pattern, replacement}]
    bool singularAnalysis = false;
    bool builtin = false;

    bool hasAnyIocFilter() const;
    QJsonObject toJson() const;  // without "builtin"
    static AnalysisProfile fromJson(const QString& name, const QJsonObject& o);
};

AnalysisProfile defaultProfile();
AnalysisProfile privateProfile();
// Built-in first, then config.custom_profiles; falls back to "default".
AnalysisProfile getProfile(const QString& name, const QJsonObject& customProfiles);
QList<AnalysisProfile> listProfiles(const QJsonObject& customProfiles);
// The active profile of the process-wide config.
AnalysisProfile activeProfile();

}  // namespace rk
