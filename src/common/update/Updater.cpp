#include "common/update/Updater.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>
#include <QUuid>

#include "common/config/Paths.h"
#include "common/config/RikuganConfig.h"
#include "common/util/Files.h"
#include "common/util/Log.h"
#include "common/util/ModuleLifetime.h"
#include "common/util/ModulePath.h"

namespace rk::update {

namespace {

QString statePath() { return paths::home() + QStringLiteral("/update.json"); }

qint64 lastCheck() {
    return qint64(readJsonObject(statePath()).value(QStringLiteral("lastCheck")).toDouble(0));
}

void setLastCheck(qint64 ms) {
    QJsonObject o = readJsonObject(statePath());
    o.insert(QStringLiteral("lastCheck"), double(ms));
    writeFileAtomic(statePath(), compactJson(o));
}

}  // namespace

Updater& Updater::instance() {
    static QPointer<Updater> self;
    if (!self) self = new Updater(uiAnchor());
    return *self;
}

Updater::Updater(QObject* parent) : QObject(parent), package_(packageName()), module_(modulePath()) {}

void Updater::setTarget(const QString& package, const QString& module) {
    package_ = package;
    module_ = module;
}

void Updater::set(State s, const QString& message) {
    state_ = s;
    message_ = message;
    Q_EMIT changed();
}

void Updater::start() {
    if (!supported() || timer_) return;
    removeLeftovers(QFileInfo(module_).absolutePath());
    timer_ = new QTimer(this);
    timer_->setInterval(3600 * 1000);
    connect(timer_, &QTimer::timeout, this, &Updater::checkIfDue);
    timer_->start();
    QTimer::singleShot(20000, this, &Updater::checkIfDue);
}

void Updater::checkIfDue() {
    if (!RikuganConfig::instance().checkForUpdates || !isDue(lastCheck(), QDateTime::currentMSecsSinceEpoch())) return;
    check(false);
}

void Updater::check(bool byUser) {
    if (!supported()) {
        if (byUser) set(State::Failed, QStringLiteral("this build of Rikugan was not installed from a release package, so "
                                                      "it does not update itself"));
        return;
    }
    if (busy_ || state_ == State::Installing) return;
    busy_ = true;
    if (byUser) set(State::Checking);
    const QUrl releases = releasesUrl();
    const QString module = module_;
    QPointer<Updater> self(this);
    spawnThread([self, releases, module, byUser] {
        QString error;
        const QUrl landing = latestLanding(releases, &error);
        // a newer copy already on disk (another window installed it): the host only needs a restart
        const QString onDisk = versionOfFile(module);
        QMetaObject::invokeMethod(uiAnchor(), [self, landing, error, onDisk, byUser] {
            if (self) self->landed(landing, error, onDisk, byUser);
        }, Qt::QueuedConnection);
    });
}

void Updater::landed(const QUrl& landing, const QString& error, const QString& onDisk, bool byUser) {
    busy_ = false;
    if (landing.isEmpty()) {
        log(QStringLiteral("Rikugan: cannot check for updates: %1").arg(error));
        if (byUser) set(State::Failed, QStringLiteral("Rikugan could not reach GitHub: %1").arg(error));
        else if (state_ == State::Checking) set(State::Idle);
        return;
    }
    setLastCheck(QDateTime::currentMSecsSinceEpoch());
    const std::optional<Release> r = releaseFromLanding(landing, releasesUrl(), package_);
    const QString current = currentVersion();
    if (!r || compareVersions(r->version, current) <= 0) {
        log(QStringLiteral("Rikugan %1 is up to date (latest release: %2)").arg(current, r ? r->tag : QStringLiteral("none")));
        if (byUser) set(State::UpToDate);
        else if (state_ == State::Checking) set(State::Idle);
        return;
    }
    release_ = *r;
    if (compareVersions(onDisk, r->version) >= 0) {
        set(State::Installed);
        return;
    }
    log(QStringLiteral("Rikugan %1 is available (this is %2)").arg(r->version, current));
    dismissed_ = false;
    set(State::Available);
}

void Updater::installAvailable() {
    if (state_ != State::Available || busy_) return;
    busy_ = true;
    set(State::Installing, QStringLiteral("Downloading Rikugan %1\u2026").arg(release_.version));
    const Release r = release_;
    const QString module = module_;
    const QString work = QDir(QDir::tempPath()).filePath(QStringLiteral("rikugan-update-") + QUuid::createUuid().toString(QUuid::Id128).left(12));
    QPointer<Updater> self(this);
    spawnThread([self, r, module, work] {
        auto post = [self](std::function<void(Updater&)> fn) {
            QMetaObject::invokeMethod(uiAnchor(), [self, fn] {
                if (self) fn(*self);
            }, Qt::QueuedConnection);
        };
        QString error;
        const bool ok = install(r, module, work, &error, [post](const QString& step) {
            post([step](Updater& u) { u.set(State::Installing, step); });
        });
        if (!ok) QDir(work).removeRecursively();
        post([ok, error, r](Updater& u) {
            u.busy_ = false;
            if (ok) {
                log(QStringLiteral("Rikugan %1 installed; restart %2 to use it").arg(r.version, hostName(u.package_)));
                u.set(State::Installed);
            } else {
                log(QStringLiteral("Rikugan was not updated to %1: %2").arg(r.version, error));
                u.set(State::Failed, error);
            }
        });
    });
}

void Updater::later() {
    if (state_ != State::Available) return;
    dismissed_ = true;
    Q_EMIT changed();
}

void Updater::acknowledge() {
    if (state_ == State::UpToDate || state_ == State::Failed || state_ == State::Checking) set(State::Idle);
    else if (state_ == State::Installed) {
        dismissed_ = true;
        Q_EMIT changed();
    }
}

}  // namespace rk::update
