#pragma once
// "Provider": pick a provider, sign in to it or give it an API key, then pick its model and thinking level. Custom
// connections (any OpenAI-compatible, Anthropic or Ollama endpoint) are providers too, added with "+". Below: sharing
// logins with the omc command-line agent and local model discovery.
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QStringList>
#include <QWidget>

#include "common/config/RikuganConfig.h"

QT_FORWARD_DECLARE_CLASS(QCheckBox)
QT_FORWARD_DECLARE_CLASS(QComboBox)
QT_FORWARD_DECLARE_CLASS(QFormLayout)
QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QLineEdit)
QT_FORWARD_DECLARE_CLASS(QPushButton)

namespace rk {

class ProviderTab : public QWidget {
    Q_OBJECT
public:
    explicit ProviderTab(const RikuganConfig& cfg, QWidget* parent = nullptr);

    // Empty when the custom connections can be saved, else what is wrong with them.
    QString validate() const;
    void apply(RikuganConfig& c) const;

private:
    struct LoginProvider {
        QString id, name;
        bool authenticated = false;
    };

    void refreshModels();
    void refreshProviders();
    void refreshThinkingLevels();
    void readAuthFile();

    void fillProviderCombo();
    void onProviderChanged();
    void fillModelCombo();
    void fillThinkingCombo();
    void updateAccountRow();
    void updateConnectionRows();

    QString currentProvider() const;
    int customIndex(const QString& id) const;
    QString modelKey() const;  // "provider/id" from the model combo, empty for automatic
    QStringList modelIdsFor(const QString& provider) const;
    QStringList levelsFor(const QString& model) const;

    void addConnection();
    void removeConnection();
    void saveApiKey();
    void login();
    void logout();

    void setStatus(QLabel* l, const QString& text, const char* style);

    QString wantedModel_, wantedThinking_, wantedProvider_;
    QJsonArray models_;               // get_available_models: credentialed models only
    QList<LoginProvider> logins_;     // get_login_providers, in omc's order
    QJsonObject auth_;                // auth.json: which providers hold a login or a key
    QList<CustomEndpoint> customs_;   // edited in place, saved on OK
    QStringList serviceLevels_;
    bool filling_ = false;

    QFormLayout* form_ = nullptr;
    QComboBox* provider_ = nullptr;
    QPushButton* addConn_ = nullptr;
    QPushButton* removeConn_ = nullptr;
    QLabel* account_ = nullptr;
    QPushButton* loginBtn_ = nullptr;
    QPushButton* logoutBtn_ = nullptr;
    QLineEdit* key_ = nullptr;
    QPushButton* saveKey_ = nullptr;
    QLineEdit* apiBase_ = nullptr;
    QComboBox* apiType_ = nullptr;
    QLineEdit* connModels_ = nullptr;
    QComboBox* model_ = nullptr;
    QComboBox* thinking_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* restartHint_ = nullptr;
    QCheckBox* useOmcLogins_ = nullptr;
    QCheckBox* useExternal_ = nullptr;
    QCheckBox* discover_ = nullptr;
};

}  // namespace rk
