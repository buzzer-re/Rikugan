#pragma once
// Rikugan's updates on the UI thread (the steps are Update.h's, on a worker thread). Once a day, while Settings ›
// Behavior › Check for updates is on, and whenever the user asks (Ctrl+K › Check for updates, or Settings), it asks
// GitHub for the latest release. A newer one is installed only when the user says so in the panel's update line:
// downloaded, checked, put in place of the installed plugin; the host uses it once restarted.
//
// Only a release package updates itself (update::packageName()): a developer's own build never does.
#include <QObject>
#include <QString>

#include "common/update/Update.h"

QT_FORWARD_DECLARE_CLASS(QTimer)

namespace rk::update {

class Updater : public QObject {
    Q_OBJECT
public:
    enum class State {
        Idle,        // nothing to say
        Checking,    // asking GitHub (said only when the user asked)
        UpToDate,    // the user asked; nothing newer
        Available,   // a newer release: the panel offers it
        Installing,  // downloading, checking, putting it in place (progress())
        Installed,   // the new version is on disk; the host uses it after a restart
        Failed,      // a check the user asked for, or an install, did not work (message())
    };

    // Process-wide; a child of uiAnchor(), so it goes when the module unloads.
    static Updater& instance();

    // What gets updated: this build's package and the plugin's own file (tests point both elsewhere).
    void setTarget(const QString& package, const QString& module);
    bool supported() const { return !package_.isEmpty() && !module_.isEmpty(); }

    // Plugin init: removes what an earlier update left (Windows), then checks 20 s later and every hour whether a day
    // has passed since the last check (~/.rikugan/update.json), while the setting is on.
    void start();
    // Now. `byUser`: whatever the setting says, and the outcome is shown even when nothing is newer.
    void check(bool byUser);
    // The user's answer to an offer.
    void installAvailable();
    void later();
    // The user read the outcome (up to date, installed, failed): the panels stop showing it.
    void acknowledge();

    State state() const { return state_; }
    const Release& release() const { return release_; }
    QString message() const { return message_; }  // Failed: why; Installing: what it is doing
    bool dismissed() const { return dismissed_; }  // Available, but the user said Later
    QString package() const { return package_; }

Q_SIGNALS:
    void changed();

private:
    explicit Updater(QObject* parent);
    void set(State s, const QString& message = {});
    void checkIfDue();
    void landed(const QUrl& landing, const QString& error, const QString& onDisk, bool byUser);

    QString package_;
    QString module_;
    State state_ = State::Idle;
    Release release_;
    QString message_;
    bool dismissed_ = false;
    bool busy_ = false;
    QTimer* timer_ = nullptr;
};

}  // namespace rk::update
