#include "common/omc/UserErrors.h"

#include <QRegularExpression>

namespace rk {

namespace {

const QString kFix = QStringLiteral("Connect one under Get started, or in Settings → Provider.");

}  // namespace

QString userFacingError(const QString& message) {
    const QString m = message.trimmed();
    // app.c app_create: no credentials anywhere and no model named
    if (m.startsWith(QLatin1String("No model available")))
        return QStringLiteral("No model is connected yet. ") + kFix;
    // app.c app_create: the configured model is not in the catalog
    static const QRegularExpression unknown(QStringLiteral("^Unknown model: (.+?) \\(see `omc models`\\)$"));
    if (const QRegularExpressionMatch u = unknown.match(m); u.hasMatch())
        return QStringLiteral("Unknown model: %1. Choose another in Settings → Provider.").arg(u.captured(1));
    // app.c prompt refusal: "No API key found for P.\n\nUse /login, set an API key environment variable, or create …"
    static const QRegularExpression refusal(QStringLiteral("^No API key found for (\\S+?)\\.(\\s|$)"));
    if (const QRegularExpressionMatch r = refusal.match(m); r.hasMatch())
        return QStringLiteral("No API key for %1. ").arg(r.captured(1)) + kFix;
    // providers and app_set_model_temporary: "No API key for provider P", "No API key for provider: P", "No API key for P/id"
    static const QRegularExpression noKey(QStringLiteral("^No API key (?:available )?for (?:provider:? )?(\\S+)$"));
    if (const QRegularExpressionMatch k = noKey.match(m); k.hasMatch())
        return QStringLiteral("No API key for %1. ").arg(k.captured(1)) + kFix;
    return message;
}

bool needsModelSetup(const QString& message) {
    const QString m = message.trimmed();
    return m.startsWith(QLatin1String("No model available")) || m.startsWith(QLatin1String("No API key"));
}

}  // namespace rk
