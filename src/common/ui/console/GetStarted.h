#pragma once
// "Connect a model": what the panel shows while no model can answer (first run, or every login removed). In
// Rikugan's words, one step at a time: sign in with a subscription, paste an API key, use a local server, or
// reuse the logins of an omc agent already on this machine; then pick the model. Changes stay editable in
// Settings → Provider.
#include <QList>
#include <QString>
#include <QWidget>

#include "common/omc/LoginFlow.h"
#include "common/ui/console/ModelPicker.h"

QT_FORWARD_DECLARE_CLASS(QComboBox)
QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QLineEdit)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QStackedWidget)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)

namespace rk::ui {

class GetStarted : public QWidget {
    Q_OBJECT
public:
    explicit GetStarted(QWidget* parent = nullptr);

    // From the first step again (the account list is reloaded).
    void start();
    // The models that can run now (the model picker's list, after each refresh).
    void setModels(const QList<ModelInfo>& models);
    // A login the panel ran for loginRequested() succeeded: its models are next.
    void loggedIn(const QString& providerId);

Q_SIGNALS:
    void loginRequested(const QString& providerId);  // the panel runs its login sheet
    void refreshModels();                            // the panel reloads the model list (setModels follows)
    void modelChosen(const QString& key);            // provider/id: the setup is done
    void dismissed();                                // "Not now", or a step that ends without a model

private:
    enum Page { Choose, SignIn, ApiKey, Local, PickModel, Restart };

    void show(Page page);
    void buildChoose();
    void buildSignIn();
    void buildApiKey();
    void buildLocal();
    void buildPickModel();
    void buildRestart();
    void fillSignIn();
    void fillModels();
    void waitForModels(const QString& provider);
    void saveKey();
    void saveLocal();
    void useOmcLogins();
    void restyle();
    QWidget* page(const QString& title, const QString& text, QVBoxLayout** body);

    QStackedWidget* pages_ = nullptr;
    QList<ModelInfo> models_;
    QList<LoginProviderInfo> logins_;
    QString waitingFor_;  // provider whose models the model step lists; "*": any
    bool waiting_ = false;

    QVBoxLayout* chooseRows_ = nullptr;
    QVBoxLayout* signInRows_ = nullptr;
    QLabel* signInNote_ = nullptr;
    QComboBox* keyProvider_ = nullptr;
    QLabel* keyLink_ = nullptr;
    QLineEdit* key_ = nullptr;
    QLabel* keyStatus_ = nullptr;
    QComboBox* localKind_ = nullptr;
    QLineEdit* localUrl_ = nullptr;
    QLineEdit* localModels_ = nullptr;
    QLabel* localStatus_ = nullptr;
    QVBoxLayout* modelRows_ = nullptr;
    QLabel* modelNote_ = nullptr;
};

}  // namespace rk::ui
