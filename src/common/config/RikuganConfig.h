#pragma once
// ~/.rikugan/config.json (both hosts share it).
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace rk {

struct CustomEndpoint {
    QString id;        // provider id in models.yml
    QString baseUrl;
    QString api;       // openai-completions | openai-responses | anthropic-messages | ollama-chat
    QString apiKey;
    QStringList models;
};

struct RikuganConfig {
    static constexpr int kSchemaVersion = 4;

    QString model;     // "provider/id" or empty for omc's default
    QString thinking;  // off|minimal|low|medium|high|xhigh|max or empty
    bool useOmcLogins = false;
    bool discoverLocalModels = false;
    // Disk and shell tools ask once per chat (Permissions.h) unless these are on: then every chat runs them freely.
    bool autoAllowFiles = false;     // read, write, edit
    bool autoAllowCommands = false;  // bash
    bool approveMutations = false;
    bool autoContext = true;
    bool restoreSessionsOnStart = true;
    bool dontAutoLoadChats = false;
    QString sessionStorageDir;  // empty: ~/.rikugan/sessions
    int explorationTurnLimit = 100;
    bool silentRetryMode = false;
    QStringList disabledSkills;
    QString activeProfile = QStringLiteral("default");
    QJsonObject customProfiles;  // name -> AnalysisProfile object (Profile.h)
    QList<CustomEndpoint> customEndpoints;
    int bulkRenamerBatchSize = 10;
    int bulkRenamerMaxConcurrent = 3;

    // Loads from disk (defaults for anything missing or invalid).
    static RikuganConfig load();
    bool save() const;
    void clamp();

    // Process-wide instance: read on the UI thread at start; copies go to workers.
    static RikuganConfig& instance();
};

}  // namespace rk
