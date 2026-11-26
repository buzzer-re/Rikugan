#pragma once
// A provider login driven over omc's RPC `login`, the way omc's own /login does it: omc sends the steps
// (open_url, progress notes, text inputs) and Rikugan shows them. Each login runs on its own short-lived omc
// session, because freeing that session is the only way omc stops a login.
//
// Not reachable through RPC (omc would need a change): providers whose login waits for a pasted authorization
// code, secret prompts, and flows that ask a question before showing any URL. Those end with
// finished(false, kTerminalOnly) so the UI can offer an API key instead.
#include <functional>
#include <memory>

#include <QList>
#include <QObject>
#include <QString>

namespace rk {

class OmcSession;

struct LoginProviderInfo {
    QString id, name;
    bool authenticated = false;
    QString source;  // "login" | "api key" | "env" | "" (not set up)
};

class LoginFlow : public QObject {
    Q_OBJECT
public:
    static const QString kTerminalOnly;

    explicit LoginFlow(QObject* parent = nullptr);
    ~LoginFlow() override;

    void start(const QString& providerId);
    void answerInput(const QString& value);  // the pending inputRequested
    void cancel();                           // ends the login; finished(false, "cancelled")
    bool running() const { return running_; }

Q_SIGNALS:
    // The UI opens the browser itself (omc does not in library mode); launchUrl is a local shortcut when set.
    void openUrl(const QString& url, const QString& launchUrl, const QString& instructions);
    void progress(const QString& message);
    void inputRequested(const QString& title, const QString& placeholder);
    void finished(bool ok, const QString& error);  // error is user-facing; "cancelled" after cancel()

private:
    void onRequest(const QJsonObject& f);
    void finish(bool ok, const QString& error);
    void closeSession();

    std::shared_ptr<OmcSession> session_;
    QString inputId_;
    bool running_ = false;
    bool sawUrlWithoutLaunch_ = false;
    int generation_ = 0;  // results of an earlier start() are ignored
};

// get_login_providers in omc's order, each with where its credential comes from (auth.json: oauth = login,
// api_key = api key; authenticated without an auth.json entry = env). `done` runs on the UI thread while `guard`
// lives.
void fetchLoginProviders(QObject* guard, std::function<void(const QList<LoginProviderInfo>&, const QString& error)> done);

}  // namespace rk
