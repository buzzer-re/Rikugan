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
    // Applied to each of its models in models.yml; 0 leaves omc's defaults (a 128000-token window, output up to
    // 32000 or the window).
    int contextWindow = 0;
    int maxTokens = 0;
    // A PEM file (the server's self-signed certificate or its private CA) trusted for HTTPS, on top of the system's.
    QString caFile{};
};

struct RikuganConfig {
    static constexpr int kSchemaVersion = 4;

    QString model;     // "provider/id" or empty for omc's default
    QString thinking;  // off|minimal|low|medium|high|xhigh|max or empty
    bool useOmcLogins = false;
    bool useExternalLogins = false;  // Claude Code's and Codex's logins (ExternalLogins.h)
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
    // Streamed text: each burst spread over smoothStreamingMs instead of shown as it arrives. Off by default: a fast
    // (local) model sends text faster than any pacing, and paced text then trails behind the model.
    bool smoothStreaming = false;
    int smoothStreamingMs = 100;
    QStringList disabledSkills;
    QString activeProfile = QStringLiteral("default");
    QJsonObject customProfiles;  // name -> AnalysisProfile object (Profile.h)
    QList<CustomEndpoint> customEndpoints;
    int bulkRenamerBatchSize = 10;
    int bulkRenamerMaxConcurrent = 3;
    // Binary Ninja's MCP server (HostMcp), e.g. "http://127.0.0.1:24642/mcp"; empty: from Binary Ninja's settings.
    QString hostMcpUrl;
    // Look for a newer release once a day (update/Updater.h); an update is installed only when the user says so.
    bool checkForUpdates = true;

    // Loads from disk (defaults for anything missing or invalid).
    static RikuganConfig load();
    bool save() const;
    void clamp();

    // Process-wide instance: read on the UI thread at start; copies go to workers.
    static RikuganConfig& instance();
};

}  // namespace rk
