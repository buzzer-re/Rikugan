#pragma once
// "Skills": built-in skills on/off
// (config.disabledSkills), the user's own skills listed.
#include <QMap>
#include <QWidget>

#include "common/config/RikuganConfig.h"

QT_FORWARD_DECLARE_CLASS(QCheckBox)

namespace rk {

class SkillsTab : public QWidget {
    Q_OBJECT
public:
    explicit SkillsTab(const RikuganConfig& cfg, QWidget* parent = nullptr);
    void apply(RikuganConfig& c) const;

private:
    QMap<QString, QCheckBox*> builtin_;
};

}  // namespace rk
