#include "common/omc/AgentHome.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

#include "common/config/Paths.h"
#include "common/config/RikuganConfig.h"
#include "common/omc/AuthSync.h"
#include "common/omc/ExternalLogins.h"
#include "common/omc/YamlWriter.h"
#include "common/util/Embedded.h"
#include "common/util/Env.h"
#include "common/util/Files.h"
#include "common/util/Log.h"

namespace rk {

namespace {

QString g_standaloneAgentDir;
constexpr char kMarker[] = ".rikugan-builtin";

QString firstExisting(std::initializer_list<const char*> files) {
    for (const char* f : files)
        if (QFileInfo::exists(QString::fromLatin1(f))) return QString::fromLatin1(f);
    return {};
}

void writeManagedConfig() {
    // omc reads its global settings once per process: these keep omc's side costs and host-unsafe paths off.
    // Approvals: omc's own mode stays yolo (Rikugan gates host tools itself); the disk and shell tools are set to
    // "prompt", so omc asks before each call and Rikugan answers from the chat's grants (Permissions.h).
    QJsonObject approval;
    for (const char* t : {"read", "write", "edit", "ast_edit", "grep", "glob", "bash"}) approval.insert(QLatin1String(t), "prompt");
    const QJsonObject cfg{
        {"tools", QJsonObject{{"approvalMode", "yolo"}, {"approval", approval}}},
        {"goal", QJsonObject{{"continuationModes", QJsonArray{"rpc"}}}},
        {"power", QJsonObject{{"sleepPrevention", "off"}}},
        {"providers", QJsonObject{{"cacheWarming", "off"}}},
        {"skills", QJsonObject{{"compressDescriptions", false}}},
        {"memory", QJsonObject{{"backend", "off"}}},
        {"mcp", QJsonObject{{"enableProjectConfig", false}}},
    };
    writeFileAtomic(paths::omcAgentDir() + QStringLiteral("/config.yml"), toYaml(cfg));
}

void writeProjectPrompt() {
    const QString text = embeddedText("prompts/omc/project-prompt.md");
    if (text.isEmpty()) return;
    writeFileAtomic(paths::omcAgentDir() + QStringLiteral("/prompts/coding-agent/prompts/system/project-prompt.md"),
                    text.toUtf8());
}

// Custom connections' trusted certificates (a self-signed server, a private CA): omc loads one extra CA file
// (OMC_EXTRA_CA_CERTS, read when its first HTTPS connection opens), so they join the file already named there (the
// system bundle on Linux, or the user's own) in one bundle. Verification stays on for every connection.
void writeTrustBundle() {
    QByteArray certs;
    for (const CustomEndpoint& e : RikuganConfig::instance().customEndpoints) {
        if (e.caFile.isEmpty()) continue;
        bool ok = false;
        const QByteArray pem = readFile(expandHome(e.caFile), &ok);
        if (!ok || !pem.contains("-----BEGIN CERTIFICATE-----")) {
            log(QStringLiteral("Rikugan: the certificate %1 of the connection %2 is not a readable PEM certificate")
                    .arg(e.caFile, e.id));
            continue;
        }
        certs += pem + '\n';
    }
    if (certs.isEmpty()) return;
    QByteArray bundle;
    for (const char* var : {"OMC_EXTRA_CA_CERTS", "NODE_EXTRA_CA_CERTS", "SSL_CERT_FILE"})
        if (const QString base = qEnvironmentVariable(var); !base.isEmpty()) {
            bundle = readFile(base) + '\n';
            break;
        }
    const QString path = paths::omcAgentDir() + QStringLiteral("/trusted-ca.pem");
    if (writeFileAtomic(path, bundle + certs)) setEnv("OMC_EXTRA_CA_CERTS", path);
}

}  // namespace

QString AgentHome::standaloneAgentDir() { return g_standaloneAgentDir; }

void AgentHome::prepare() {
    static bool done = false;
    if (done) return;
    done = true;

    const QString userAgent = qEnvironmentVariable("OMC_CODING_AGENT_DIR");
    const QString userConfig = qEnvironmentVariable("OMC_CONFIG_DIR");
    g_standaloneAgentDir = !userAgent.isEmpty()   ? expandHome(userAgent)
                           : !userConfig.isEmpty() ? expandHome(userConfig) + QStringLiteral("/agent")
                                                   : homeDir() + QStringLiteral("/.omc/agent");

    setEnv("OMC_CONFIG_DIR", paths::omcConfigDir());
    unsetEnv("OMC_CODING_AGENT_DIR");
    unsetEnv("PI_CODING_AGENT_DIR");
    // chats are named by omc after their first exchange; a stale value from the environment would stop it
    unsetEnv("OMC_NO_TITLE");
    unsetEnv("PI_NO_TITLE");
    if (!RikuganConfig::instance().discoverLocalModels) setEnv("OMC_NO_LOCAL_DISCOVERY", QStringLiteral("1"));
    if (qEnvironmentVariableIsEmpty("OMC_EXTRA_CA_CERTS")) {
        // the libcrypto a host loads first (Binary Ninja bundles one) may look for CAs elsewhere
        const QString ca = firstExisting(
            {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/cert.pem"});
        if (!ca.isEmpty()) setEnv("OMC_EXTRA_CA_CERTS", ca);
    }

    ensureDir(paths::omcAgentDir());
    writeTrustBundle();
    writeManagedConfig();
    syncSkills();
    writeProjectPrompt();
    writeModelsYaml();
    // Claude Code's and Codex's logins before the first session reads auth.json
    if (RikuganConfig::instance().useExternalLogins) ExternalLogins::syncFiles(rikuganAuthPath(), {}, false);
}

void AgentHome::writeModelsYaml() {
    const RikuganConfig& c = RikuganConfig::instance();
    const QString path = paths::omcAgentDir() + QStringLiteral("/models.yml");
    // the service session's model: no key and no `auth: none`, so omc never counts it as available or picks it as a
    // default, while naming it explicitly still opens a session (app_resolve_model_pattern searches the whole catalog)
    QJsonObject providers{{serviceProvider(), QJsonObject{{"baseUrl", "http://127.0.0.1:9/v1"},
                                                          {"api", "openai-completions"},
                                                          {"models", QJsonArray{QJsonObject{{"id", "settings"}}}}}}};
    for (const CustomEndpoint& e : c.customEndpoints) {
        QJsonArray models;
        for (const QString& m : e.models) {
            // catalog.c fill_model: contextWindow defaults to 128000, maxTokens to min(window, 32000)
            QJsonObject model{{"id", m}};
            if (e.contextWindow > 0) model.insert(QStringLiteral("contextWindow"), e.contextWindow);
            if (e.maxTokens > 0) model.insert(QStringLiteral("maxTokens"), e.maxTokens);
            models.append(model);
        }
        QJsonObject p{{"baseUrl", e.baseUrl},
                      {"api", e.api.isEmpty() ? QStringLiteral("openai-completions") : e.api},
                      {"apiKey", e.apiKey.isEmpty() ? QStringLiteral("rikugan-no-key") : e.apiKey},
                      {"models", models}};
        providers.insert(e.id, p);
    }
    writeFileAtomic(path, toYaml(QJsonObject{{"providers", providers}}), 0600);
}

QString AgentHome::serviceProvider() { return QStringLiteral("rikugan-service"); }
QString AgentHome::serviceModel() { return serviceProvider() + QStringLiteral("/settings"); }

void AgentHome::syncSkills() {
    const QString root = paths::omcAgentDir() + QStringLiteral("/skills");
    const QStringList disabled = RikuganConfig::instance().disabledSkills;
    const QByteArray version = RIKUGAN_VERSION;

    // group embedded files by skill slug: skills/<slug>/<rest>
    QSet<QString> slugs;
    for (const EmbeddedFile& f : embeddedFiles()) {
        const QString p = QString::fromUtf8(f.path.data(), qsizetype(f.path.size()));
        if (!p.startsWith(QLatin1String("skills/"))) continue;
        slugs.insert(p.section(QLatin1Char('/'), 1, 1));
    }
    for (const QString& slug : slugs) {
        const QString dir = root + QLatin1Char('/') + slug;
        const QString marker = dir + QLatin1Char('/') + QLatin1String(kMarker);
        const bool exists = QFileInfo::exists(dir);
        const bool ours = QFileInfo::exists(marker);
        if (exists && !ours) continue;  // the user's own skill of that name
        if (disabled.contains(slug)) {
            if (exists) QDir(dir).removeRecursively();
            continue;
        }
        if (exists && readFile(marker) == version) continue;
        QDir(dir).removeRecursively();
        for (const EmbeddedFile& f : embeddedFiles()) {
            const QString p = QString::fromUtf8(f.path.data(), qsizetype(f.path.size()));
            const QString prefix = QStringLiteral("skills/") + slug + QLatin1Char('/');
            if (!p.startsWith(prefix)) continue;
            writeFileAtomic(dir + QLatin1Char('/') + p.mid(prefix.size()),
                            QByteArray(f.data.data(), qsizetype(f.data.size())));
        }
        writeFileAtomic(marker, version);
    }
}

}  // namespace rk
