#pragma once
// "Use my Claude Code and Codex logins": the subscription logins of Anthropic's Claude Code and OpenAI's Codex CLI,
// shared with Rikugan's omc (its auth.json, R) the way AuthSync shares standalone omc's.
//   Claude Code  <CLAUDE_CONFIG_DIR or ~/.claude>/.credentials.json (macOS: the "Claude Code-credentials" keychain item
//                when there is no file) -> R["anthropic"]
//   Codex        <CODEX_HOME or ~/.codex>/auth.json (a ChatGPT login)                -> R["openai-codex"]
// Per provider:
//   - nothing in R: the tool's login is copied in
//   - an OAuth login of the same account in R: the one that expires later wins, on both sides. OAuth refresh tokens
//     are single-use, so a login omc refreshed is written back to the tool, which keeps working
//   - an API key, or another account's login, in R: both stay as they are
// Off unless config.useExternalLogins. Any thread; touches only files (and the macOS keychain).
#include <QJsonObject>
#include <QList>
#include <QString>

namespace rk {

struct ExternalLogin {
    QString provider;  // omc's provider id: anthropic | openai-codex
    QString tool;      // "Claude Code" | "Codex"
    QJsonObject credential;  // omc's shape: {type: oauth, access, refresh, expires (ms), accountId}
};

class ExternalLogins {
public:
    // Where each tool keeps its login (tests point them elsewhere). Empty: the default place.
    struct Sources {
        QString claudeFile;
        QString codexFile;
        bool keychain = true;  // macOS: Claude Code's keychain item when the file is missing
    };

    // The logins found (credentials included): for the offer in Connect a model and Settings.
    static QList<ExternalLogin> find(const Sources& sources = {});
    // Merges them into Rikugan's auth.json as described above and makes omc re-read it when it changed. No-op unless
    // config.useExternalLogins. Returns whether Rikugan's logins changed.
    static bool run();
    // The same whatever the setting (the user just turned it on).
    static bool syncNow();
    // The merge itself, with explicit paths; `reload`: make omc re-read R when it changed.
    static bool syncFiles(const QString& rPath, const Sources& sources, bool reload);
};

}  // namespace rk
