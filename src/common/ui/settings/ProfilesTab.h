#pragma once
// "Profiles": pick the active analysis profile and edit custom ones.
#include <QJsonObject>
#include <QMap>
#include <QWidget>

#include "common/config/RikuganConfig.h"

QT_FORWARD_DECLARE_CLASS(QCheckBox)
QT_FORWARD_DECLARE_CLASS(QComboBox)
QT_FORWARD_DECLARE_CLASS(QGroupBox)
QT_FORWARD_DECLARE_CLASS(QHBoxLayout)
QT_FORWARD_DECLARE_CLASS(QLineEdit)
QT_FORWARD_DECLARE_CLASS(QListWidget)
QT_FORWARD_DECLARE_CLASS(QPlainTextEdit)
QT_FORWARD_DECLARE_CLASS(QPushButton)

namespace rk {

class ProfilesTab : public QWidget {
    Q_OBJECT
public:
    explicit ProfilesTab(const RikuganConfig& cfg, QWidget* parent = nullptr);
    void apply(RikuganConfig& c);

private:
    QHBoxLayout* buildProfileSelector();
    QGroupBox* buildDescriptionSection();
    QGroupBox* buildIocFiltersSection();
    QGroupBox* buildCustomRulesSection();
    QGroupBox* buildDeniedToolsSection();
    QGroupBox* buildAdvancedSection();

    void populateCombo(const QString& select);
    void loadProfile(const QString& name);
    void saveCurrentToWorkingCopy();
    void addRule();
    void removeRule();
    void newProfile();
    void cloneProfile();
    void deleteProfile();
    // name + description, or false when cancelled
    bool promptNewProfile(const QString& title, QString* name, QString* description);

    QJsonObject custom_;
    QString displayed_;

    QComboBox* combo_ = nullptr;
    QPushButton* deleteBtn_ = nullptr;
    QPlainTextEdit* desc_ = nullptr;
    QCheckBox* hideMetadata_ = nullptr;
    QMap<QString, QCheckBox*> ioc_;
    QPushButton* iocSelectAll_ = nullptr;
    QPushButton* iocDeselectAll_ = nullptr;
    QLineEdit* ruleName_ = nullptr;
    QLineEdit* rulePattern_ = nullptr;
    QComboBox* ruleType_ = nullptr;
    QLineEdit* ruleReplacement_ = nullptr;
    QPushButton* addRule_ = nullptr;
    QListWidget* rules_ = nullptr;
    QPushButton* removeRule_ = nullptr;
    QPlainTextEdit* deniedTools_ = nullptr;
    QPlainTextEdit* deniedFuncs_ = nullptr;
    QPlainTextEdit* customFilters_ = nullptr;
};

}  // namespace rk
