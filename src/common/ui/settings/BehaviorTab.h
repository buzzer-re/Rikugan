#pragma once
// "Behavior": context, chat restore, session storage, exploration, retries, filesystem/shell, approvals,
// bulk renamer defaults.
#include <QWidget>

#include "common/config/RikuganConfig.h"

QT_FORWARD_DECLARE_CLASS(QCheckBox)
QT_FORWARD_DECLARE_CLASS(QLineEdit)
QT_FORWARD_DECLARE_CLASS(QSpinBox)

namespace rk {

class BehaviorTab : public QWidget {
    Q_OBJECT
public:
    explicit BehaviorTab(const RikuganConfig& cfg, QWidget* parent = nullptr);
    void apply(RikuganConfig& c) const;

private:
    QCheckBox* autoContext_ = nullptr;
    QCheckBox* restoreSessions_ = nullptr;
    QCheckBox* dontAutoLoad_ = nullptr;
    QLineEdit* sessionStorage_ = nullptr;
    QSpinBox* exploreTurns_ = nullptr;
    QCheckBox* silentRetry_ = nullptr;
    QCheckBox* filesystem_ = nullptr;
    QCheckBox* shell_ = nullptr;
    QCheckBox* approveMutations_ = nullptr;
    QSpinBox* renamerBatch_ = nullptr;
    QSpinBox* renamerConcurrent_ = nullptr;
};

}  // namespace rk
