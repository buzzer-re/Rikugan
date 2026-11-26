#pragma once
// "Connect a provider": a sheet over the panel (or in Settings' dialog) with the providers (fuzzy search, which are
// logged in and how), then the login itself as omc drives it: the browser link (opened for the user, with Copy),
// a device code, a text input, progress notes; Cancel at every step. A provider omc can only log in from a
// terminal, or a failed login, offers an API key instead.
#include <QPointer>
#include <QWidget>

#include "common/omc/LoginFlow.h"

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QLineEdit)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QStackedWidget)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)

namespace rk::ui {

class PaletteList;

class LoginSheet : public QWidget {
    Q_OBJECT
public:
    // `overlay`: covers its parent and follows its size (the panel); otherwise an ordinary child (a dialog).
    LoginSheet(QWidget* parent, bool overlay);
    ~LoginSheet() override;

    void showList();
    void startLogin(const QString& providerId);
    bool busy() const;

Q_SIGNALS:
    void closed();
    void connected(const QString& providerId);

protected:
    bool eventFilter(QObject* o, QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    void loadProviders();
    void onOpenUrl(const QString& url, const QString& launchUrl, const QString& instructions);
    void onProgress(const QString& message);
    void onInput(const QString& title, const QString& placeholder);
    void onFinished(bool ok, const QString& error);
    void showFailure(const QString& error);
    void saveKey();
    void dismiss();
    void restyle();
    QString nameOf(const QString& id) const;

    bool overlay_;
    LoginFlow* flow_ = nullptr;
    QList<LoginProviderInfo> providers_;
    QString provider_;

    QLabel* heading_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    // list
    QLineEdit* search_ = nullptr;
    PaletteList* list_ = nullptr;
    QLabel* listNote_ = nullptr;
    // flow
    QLabel* flowTitle_ = nullptr;
    QWidget* urlBox_ = nullptr;
    QLabel* instructions_ = nullptr;
    QLineEdit* url_ = nullptr;
    QString openedUrl_;
    QWidget* inputBox_ = nullptr;
    QLabel* inputTitle_ = nullptr;
    QLineEdit* input_ = nullptr;
    QLabel* progress_ = nullptr;
    // result
    QLabel* resultTitle_ = nullptr;
    QLabel* resultText_ = nullptr;
    QWidget* keyBox_ = nullptr;
    QLineEdit* key_ = nullptr;
    QLabel* keyStatus_ = nullptr;
    QPushButton* chooseModel_ = nullptr;
    QPushButton* retry_ = nullptr;
};

}  // namespace rk::ui
