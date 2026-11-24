#include "common/ui/settings/SkillsTab.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

#include "common/agent/Skills.h"
#include "common/config/Paths.h"
#include "common/ui/settings/ServiceRpc.h"

namespace rk {

namespace {

QString line(const QString& slug, const QString& description) {
    return QStringLiteral("%1  —  %2").arg(slug, description.isEmpty() ? QObject::tr("(no description)") : description);
}

}  // namespace

SkillsTab::SkillsTab(const RikuganConfig& cfg, QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    auto* note = new QLabel(tr("Changes apply to new chats."));
    note->setStyleSheet(QString::fromLatin1(settings::kHintStyle));
    outer->addWidget(note);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* container = new QWidget;
    auto* layout = new QVBoxLayout(container);

    auto* builtinGroup = new QGroupBox(tr("Rikugan Skills"));
    auto* bl = new QVBoxLayout(builtinGroup);
    const QStringList slugs = builtinSkillSlugs();
    if (slugs.isEmpty()) bl->addWidget(new QLabel(tr("No skills found")));
    for (const QString& slug : slugs) {
        auto* cb = new QCheckBox(line(slug, builtinSkillDescription(slug)));
        cb->setObjectName(QStringLiteral("skill:") + slug);
        cb->setChecked(!cfg.disabledSkills.contains(slug));
        builtin_.insert(slug, cb);
        bl->addWidget(cb);
    }
    layout->addWidget(builtinGroup);

    auto* userGroup = new QGroupBox(tr("Your Skills"));
    auto* ul = new QVBoxLayout(userGroup);
    int users = 0;
    for (const SkillInfo& s : listSkills()) {
        if (s.builtin || slugs.contains(s.slug)) continue;
        auto* l = new QLabel(line(s.slug, s.description));
        l->setWordWrap(true);
        ul->addWidget(l);
        ++users;
    }
    if (!users) ul->addWidget(new QLabel(tr("No skills found")));
    auto* where = new QLabel(tr("Add your own as %1/skills/<name>/SKILL.md").arg(paths::omcAgentDir()));
    where->setStyleSheet(QString::fromLatin1(settings::kHintStyle));
    where->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ul->addWidget(where);
    layout->addWidget(userGroup);

    layout->addStretch();
    scroll->setWidget(container);
    outer->addWidget(scroll);
}

void SkillsTab::apply(RikuganConfig& c) const {
    QStringList disabled;
    for (auto it = builtin_.begin(); it != builtin_.end(); ++it)
        if (!it.value()->isChecked()) disabled << it.key();
    c.disabledSkills = disabled;
}

}  // namespace rk
