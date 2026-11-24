#pragma once
// Settings-side calls into the service session: the job runs on its worker, the result comes back to the UI
// thread through the event loop and is dropped when `guard` is gone by then.
#include <functional>
#include <utility>

#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>

#include "common/omc/ServiceSession.h"
#include "common/omc/UserErrors.h"
#include "common/util/ModuleLifetime.h"

namespace rk::settings {

inline void serviceJob(QObject* guard, std::function<QJsonObject(OmcSession&)> job,
                       std::function<void(const QJsonObject&)> done) {
    QPointer<QObject> g(guard);
    ServiceSession::get()->post([g, job = std::move(job), done = std::move(done)](OmcSession& s) {
        const QJsonObject r = job(s);
        QMetaObject::invokeMethod(
            uiAnchor(), [g, done, r] { if (g) done(r); }, Qt::QueuedConnection);
    });
}

inline void serviceRequest(QObject* guard, QJsonObject cmd, std::function<void(const QJsonObject&)> done,
                           int timeoutMs = 60000) {
    serviceJob(guard, [cmd = std::move(cmd), timeoutMs](OmcSession& s) { return s.request(cmd, timeoutMs); },
               std::move(done));
}

inline bool succeeded(const QJsonObject& r) { return r.value(QStringLiteral("success")).toBool(); }
inline QJsonObject dataOf(const QJsonObject& r) { return r.value(QStringLiteral("data")).toObject(); }
inline QString errorOf(const QJsonObject& r) {
    const QString e = r.value(QStringLiteral("error")).toString();
    return e.isEmpty() ? QStringLiteral("request failed") : userFacingError(e);
}

// Status label colours (ok, error, hint).
inline constexpr char kOkStyle[] = "color: #4ec9b0; font-size: 11px; font-weight: bold;";
inline constexpr char kErrStyle[] = "color: #f44747; font-size: 11px;";
inline constexpr char kHintStyle[] = "color: #808080; font-size: 10px;";

}  // namespace rk::settings
