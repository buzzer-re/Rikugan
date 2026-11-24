#pragma once
// "Use my omc logins": keeps Rikugan's omc auth.json (R) in step with standalone omc's (O).
//   - a provider only in O is copied to R
//   - an OAuth login in both: the one that expires later is written to the other side
//   - API keys in both: R stays as it is
//   - R-only entries never go to O; nothing is ever deleted
#include <QJsonObject>
#include <QString>

namespace rk {

struct AuthMergeResult {
    bool rChanged = false;
    bool oChanged = false;
};

AuthMergeResult mergeAuth(QJsonObject& r, QJsonObject& o);

class AuthSync {
public:
    // Reads both files, merges, writes the changed side(s) atomically (0600) and makes omc reload when R
    // changed. No-op unless config.useOmcLogins. Any thread; touches only files.
    static void run();
    // The same whatever the setting (the user just turned it on); returns whether Rikugan's logins changed.
    static bool syncNow();
    // Same with explicit paths (tests); returns whether R changed.
    static bool syncFiles(const QString& rPath, const QString& oPath);
};

// Rikugan's auth.json (omc/agent/auth.json): the type of `provider`'s credential ("oauth", "api_key" or "").
QString storedCredentialType(const QString& provider);
// Stores an API key for `provider` there (atomically, 0600) and makes omc re-read it; replaces a login.
// Returns "" or what went wrong, for the user.
QString storeApiKey(const QString& provider, const QString& key);

}  // namespace rk
