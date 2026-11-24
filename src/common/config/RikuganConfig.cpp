#include "common/config/RikuganConfig.h"

#include <QJsonObject>

#include "common/config/Paths.h"
#include "common/util/Files.h"

namespace rk {

namespace {

QStringList toStrings(const QJsonValue& v) {
    QStringList out;
    for (const QJsonValue& x : v.toArray())
        if (x.isString()) out << x.toString();
    return out;
}

QJsonArray fromStrings(const QStringList& l) {
    QJsonArray a;
    for (const QString& s : l) a.append(s);
    return a;
}

}  // namespace

RikuganConfig& RikuganConfig::instance() {
    static RikuganConfig cfg = RikuganConfig::load();
    return cfg;
}

void RikuganConfig::clamp() {
    explorationTurnLimit = std::clamp(explorationTurnLimit, 5, 200);
    bulkRenamerBatchSize = std::clamp(bulkRenamerBatchSize, 1, 100);
    bulkRenamerMaxConcurrent = std::clamp(bulkRenamerMaxConcurrent, 1, 16);
}

RikuganConfig RikuganConfig::load() {
    RikuganConfig c;
    const QJsonObject o = readJsonObject(paths::configFile());
    auto str = [&](const char* k, QString& out) {
        if (o.value(QLatin1String(k)).isString()) out = o.value(QLatin1String(k)).toString();
    };
    auto boolean = [&](const char* k, bool& out) {
        if (o.value(QLatin1String(k)).isBool()) out = o.value(QLatin1String(k)).toBool();
    };
    auto integer = [&](const char* k, int& out) {
        if (o.value(QLatin1String(k)).isDouble()) out = o.value(QLatin1String(k)).toInt();
    };
    str("model", c.model);
    str("thinking", c.thinking);
    boolean("use_omc_logins", c.useOmcLogins);
    boolean("use_external_logins", c.useExternalLogins);
    boolean("discover_local_models", c.discoverLocalModels);
    boolean("auto_allow_files", c.autoAllowFiles);
    boolean("auto_allow_commands", c.autoAllowCommands);
    boolean("approve_mutations", c.approveMutations);
    boolean("auto_context", c.autoContext);
    boolean("restore_sessions_on_start", c.restoreSessionsOnStart);
    boolean("dont_auto_load_chats", c.dontAutoLoadChats);
    str("session_storage_dir", c.sessionStorageDir);
    integer("exploration_turn_limit", c.explorationTurnLimit);
    boolean("silent_retry_mode", c.silentRetryMode);
    c.disabledSkills = toStrings(o.value(QStringLiteral("disabled_skills")));
    str("active_profile", c.activeProfile);
    c.customProfiles = o.value(QStringLiteral("custom_profiles")).toObject();
    for (const QJsonValue& v : o.value(QStringLiteral("custom_endpoints")).toArray()) {
        const QJsonObject e = v.toObject();
        CustomEndpoint ep{e.value(QStringLiteral("id")).toString(), e.value(QStringLiteral("base_url")).toString(),
                          e.value(QStringLiteral("api")).toString(), e.value(QStringLiteral("api_key")).toString(),
                          toStrings(e.value(QStringLiteral("models")))};
        if (!ep.id.isEmpty() && !ep.baseUrl.isEmpty()) c.customEndpoints << ep;
    }
    integer("bulk_renamer_batch_size", c.bulkRenamerBatchSize);
    integer("bulk_renamer_max_concurrent", c.bulkRenamerMaxConcurrent);
    c.clamp();
    return c;
}

bool RikuganConfig::save() const {
    RikuganConfig c = *this;
    c.clamp();
    QJsonArray eps;
    for (const CustomEndpoint& e : c.customEndpoints)
        eps.append(QJsonObject{{"id", e.id}, {"base_url", e.baseUrl}, {"api", e.api}, {"api_key", e.apiKey},
                               {"models", fromStrings(e.models)}});
    const QJsonObject o{
        {"schema_version", kSchemaVersion},
        {"model", c.model},
        {"thinking", c.thinking},
        {"use_omc_logins", c.useOmcLogins},
        {"use_external_logins", c.useExternalLogins},
        {"discover_local_models", c.discoverLocalModels},
        {"auto_allow_files", c.autoAllowFiles},
        {"auto_allow_commands", c.autoAllowCommands},
        {"approve_mutations", c.approveMutations},
        {"auto_context", c.autoContext},
        {"restore_sessions_on_start", c.restoreSessionsOnStart},
        {"dont_auto_load_chats", c.dontAutoLoadChats},
        {"session_storage_dir", c.sessionStorageDir},
        {"exploration_turn_limit", c.explorationTurnLimit},
        {"silent_retry_mode", c.silentRetryMode},
        {"disabled_skills", fromStrings(c.disabledSkills)},
        {"active_profile", c.activeProfile},
        {"custom_profiles", c.customProfiles},
        {"custom_endpoints", eps},
        {"bulk_renamer_batch_size", c.bulkRenamerBatchSize},
        {"bulk_renamer_max_concurrent", c.bulkRenamerMaxConcurrent},
    };
    // the file holds API keys of custom endpoints
    return writeFileAtomic(paths::configFile(), QJsonDocument(o).toJson(QJsonDocument::Indented), 0600);
}

}  // namespace rk
