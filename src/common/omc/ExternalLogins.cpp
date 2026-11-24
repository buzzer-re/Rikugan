#include "common/omc/ExternalLogins.h"

#include <optional>

#include <QDateTime>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>

#include "common/config/RikuganConfig.h"
#include "common/omc/AuthSync.h"
#include "common/omc/ServiceSession.h"
#include "common/util/Env.h"
#include "common/util/Files.h"
#include "common/util/Log.h"

namespace rk {

namespace {

const QString kKeychainService = QStringLiteral("Claude Code-credentials");

QString claudePath(const ExternalLogins::Sources& s) {
    if (!s.claudeFile.isEmpty()) return s.claudeFile;
    const QString dir = qEnvironmentVariable("CLAUDE_CONFIG_DIR");
    return (dir.isEmpty() ? homeDir() + QStringLiteral("/.claude") : expandHome(dir)) + QStringLiteral("/.credentials.json");
}

QString codexPath(const ExternalLogins::Sources& s) {
    if (!s.codexFile.isEmpty()) return s.codexFile;
    const QString dir = qEnvironmentVariable("CODEX_HOME");
    return (dir.isEmpty() ? homeDir() + QStringLiteral("/.codex") : expandHome(dir)) + QStringLiteral("/auth.json");
}

QString str(const QJsonObject& o, const char* k) { return o.value(QLatin1String(k)).toString(); }
double expires(const QJsonObject& e) { return e.value(QStringLiteral("expires")).toDouble(0); }

// exp of a JWT, in ms (0 when it has none)
double jwtExpiryMs(const QString& jwt) {
    const QStringList parts = jwt.split(QLatin1Char('.'));
    if (parts.size() < 2) return 0;
    const QByteArray payload = QByteArray::fromBase64(parts[1].toLatin1(), QByteArray::Base64UrlEncoding);
    return QJsonDocument::fromJson(payload).object().value(QStringLiteral("exp")).toDouble(0) * 1000.0;
}

// ---- macOS keychain (Claude Code keeps its login there instead of a file) ----

QString runSecurity(const QStringList& args, const QByteArray& input = {}) {
    QProcess p;
    p.start(QStringLiteral("/usr/bin/security"), args);
    if (!p.waitForStarted(3000)) return {};
    if (!input.isEmpty()) p.write(input);
    p.closeWriteChannel();
    if (!p.waitForFinished(5000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        p.kill();
        return {};
    }
    return QString::fromUtf8(p.readAllStandardOutput());
}

std::optional<QJsonObject> readKeychain() {
#ifdef __APPLE__
    const QString out = runSecurity({QStringLiteral("find-generic-password"), QStringLiteral("-s"), kKeychainService, QStringLiteral("-w")});
    QJsonParseError err{};
    const QJsonDocument d = QJsonDocument::fromJson(out.trimmed().toUtf8(), &err);
    if (err.error == QJsonParseError::NoError && d.isObject()) return d.object();
#endif
    return std::nullopt;
}

bool writeKeychain(const QJsonObject& whole) {
#ifdef __APPLE__
    // the item's account, so -U updates that item instead of adding a second one
    const QString attrs = runSecurity({QStringLiteral("find-generic-password"), QStringLiteral("-s"), kKeychainService});
    static const QRegularExpression acct(QStringLiteral("\"acct\"<blob>=\"([^\"]*)\""));
    const QRegularExpressionMatch m = acct.match(attrs);
    const QString account = m.hasMatch() ? m.captured(1) : qEnvironmentVariable("USER");
    auto quoted = [](QString s) {
        s.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        s.replace(QLatin1Char('"'), QStringLiteral("\\\""));
        return QLatin1Char('"') + s + QLatin1Char('"');
    };
    // through `security -i` (stdin): the secret never appears in a process list
    const QString cmd = QStringLiteral("add-generic-password -U -a %1 -s %2 -w %3\n")
                            .arg(quoted(account), quoted(kKeychainService),
                                 quoted(QString::fromUtf8(QJsonDocument(whole).toJson(QJsonDocument::Compact))));
    QProcess p;
    p.start(QStringLiteral("/usr/bin/security"), {QStringLiteral("-i")});
    if (!p.waitForStarted(3000)) return false;
    p.write(cmd.toUtf8());
    p.closeWriteChannel();
    return p.waitForFinished(5000) && p.exitCode() == 0;
#else
    (void)whole;
    return false;
#endif
}

// ---- one tool's login: where it lives, its own JSON, and omc's credential from it ----

struct Source {
    QString provider, tool, path;
    bool keychain = false;  // read from (and written back to) the macOS keychain
    QJsonObject whole;      // the tool's own document
    QJsonObject credential;
};

QJsonObject fromClaude(const QJsonObject& whole) {
    const QJsonObject c = whole.value(QStringLiteral("claudeAiOauth")).toObject();
    if (str(c, "accessToken").isEmpty() || str(c, "refreshToken").isEmpty()) return {};
    QJsonObject cred{{"type", "oauth"},
                     {"access", c.value(QStringLiteral("accessToken"))},
                     {"refresh", c.value(QStringLiteral("refreshToken"))},
                     {"expires", c.value(QStringLiteral("expiresAt")).toDouble(0)}};
    if (!str(c, "accountUuid").isEmpty()) cred.insert(QStringLiteral("accountId"), c.value(QStringLiteral("accountUuid")));
    return cred;
}

void toClaude(QJsonObject& whole, const QJsonObject& cred) {
    QJsonObject c = whole.value(QStringLiteral("claudeAiOauth")).toObject();
    c.insert(QStringLiteral("accessToken"), cred.value(QStringLiteral("access")));
    c.insert(QStringLiteral("refreshToken"), cred.value(QStringLiteral("refresh")));
    c.insert(QStringLiteral("expiresAt"), qint64(expires(cred)));
    whole.insert(QStringLiteral("claudeAiOauth"), c);
}

QJsonObject fromCodex(const QJsonObject& whole) {
    const QJsonObject t = whole.value(QStringLiteral("tokens")).toObject();
    if (str(t, "access_token").isEmpty() || str(t, "refresh_token").isEmpty()) return {};
    QJsonObject cred{{"type", "oauth"},
                     {"access", t.value(QStringLiteral("access_token"))},
                     {"refresh", t.value(QStringLiteral("refresh_token"))},
                     {"expires", jwtExpiryMs(str(t, "access_token"))}};
    if (!str(t, "account_id").isEmpty()) cred.insert(QStringLiteral("accountId"), t.value(QStringLiteral("account_id")));
    return cred;
}

void toCodex(QJsonObject& whole, const QJsonObject& cred) {
    QJsonObject t = whole.value(QStringLiteral("tokens")).toObject();
    t.insert(QStringLiteral("access_token"), cred.value(QStringLiteral("access")));
    t.insert(QStringLiteral("refresh_token"), cred.value(QStringLiteral("refresh")));
    whole.insert(QStringLiteral("tokens"), t);
    whole.insert(QStringLiteral("last_refresh"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
}

QList<Source> sources(const ExternalLogins::Sources& s) {
    QList<Source> out;
    {
        Source c{QStringLiteral("anthropic"), QStringLiteral("Claude Code"), claudePath(s)};
        bool ok = false;
        c.whole = readJsonObject(c.path, &ok);
        if (!ok && s.keychain && !QFileInfo::exists(c.path))
            if (std::optional<QJsonObject> k = readKeychain()) {
                c.whole = *k;
                c.keychain = true;
            }
        c.credential = fromClaude(c.whole);
        if (!c.credential.isEmpty()) out << c;
    }
    {
        Source c{QStringLiteral("openai-codex"), QStringLiteral("Codex"), codexPath(s)};
        c.whole = readJsonObject(c.path);
        c.credential = fromCodex(c.whole);
        if (!c.credential.isEmpty()) out << c;
    }
    return out;
}

// One login in both places: the same account (by id, else by a token both hold).
bool sameAccount(const QJsonObject& r, const QJsonObject& e) {
    if (!str(r, "accountId").isEmpty() && !str(e, "accountId").isEmpty()) return str(r, "accountId") == str(e, "accountId");
    return str(r, "refresh") == str(e, "refresh") || str(r, "access") == str(e, "access");
}

bool writeBack(Source& src, const QJsonObject& cred) {
    src.provider == QLatin1String("anthropic") ? toClaude(src.whole, cred) : toCodex(src.whole, cred);
    if (src.keychain) return writeKeychain(src.whole);
    QString err;
    if (writeFileAtomic(src.path, QJsonDocument(src.whole).toJson(QJsonDocument::Indented), 0600, &err)) return true;
    logOnce(QStringLiteral("extlogin-") + src.provider, QStringLiteral("Rikugan: cannot update %1: %2").arg(src.path, err));
    return false;
}

}  // namespace

QList<ExternalLogin> ExternalLogins::find(const Sources& s) {
    QList<ExternalLogin> out;
    for (const Source& src : sources(s)) out << ExternalLogin{src.provider, src.tool, src.credential};
    return out;
}

bool ExternalLogins::run() { return RikuganConfig::instance().useExternalLogins && syncNow(); }

bool ExternalLogins::syncNow() { return syncFiles(rikuganAuthPath(), {}, true); }

bool ExternalLogins::syncFiles(const QString& rPath, const Sources& s, bool reload) {
    QList<Source> found = sources(s);
    if (found.isEmpty()) return false;
    bool rChanged = false;
    {
        std::lock_guard<std::mutex> g(authFileMutex());
        bool ok = false;
        QJsonObject r = readJsonObject(rPath, &ok);
        if (!ok && QFileInfo::exists(rPath)) {
            logOnce(QStringLiteral("extlogin-r"), QStringLiteral("Rikugan: cannot read %1").arg(rPath));
            return false;
        }
        for (Source& src : found) {
            const QJsonObject e = src.credential;
            if (!r.contains(src.provider)) {
                r.insert(src.provider, e);
                rChanged = true;
                continue;
            }
            QJsonObject re = r.value(src.provider).toObject();
            if (str(re, "type") != QLatin1String("oauth") || !sameAccount(re, e)) continue;
            if (expires(e) > expires(re)) {
                // the tool refreshed it: Rikugan takes the new tokens and keeps what omc knows about the account
                for (const char* k : {"access", "refresh", "expires"}) re.insert(QLatin1String(k), e.value(QLatin1String(k)));
                if (str(re, "accountId").isEmpty() && !str(e, "accountId").isEmpty()) re.insert(QStringLiteral("accountId"), e.value(QStringLiteral("accountId")));
                r.insert(src.provider, re);
                rChanged = true;
            } else if (expires(re) > expires(e) && str(re, "refresh") != str(e, "refresh")) {
                // omc refreshed it, and the tool's refresh token is spent: the tool gets the new tokens
                writeBack(src, re);
            }
        }
        if (rChanged) {
            QString err;
            if (!writeFileAtomic(rPath, QJsonDocument(r).toJson(QJsonDocument::Indented), 0600, &err)) {
                log(QStringLiteral("Rikugan: cannot write %1: %2").arg(rPath, err));
                return false;
            }
        }
    }
    if (rChanged && reload) ServiceSession::reloadCredentials();
    return rChanged;
}

}  // namespace rk
