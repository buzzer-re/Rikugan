#pragma once
// Skills as omc sees them: folders under Rikugan's omc agent dir (skills/<slug>/SKILL.md).
#include <QList>
#include <QString>
#include <QStringList>

namespace rk {

struct SkillInfo {
    QString slug;         // folder name (= frontmatter name for built-ins)
    QString description;
    QString mode;         // "plan" | "exploration" | empty
    bool builtin = false; // written by Rikugan (.rikugan-builtin marker)
};

QList<SkillInfo> listSkills();
// SKILL.md body without its frontmatter; empty when there is no such skill.
QString skillBody(const QString& slug);
// Every built-in slug shipped with this build (enabled or not).
QStringList builtinSkillSlugs();
// Description from the embedded copy of a built-in skill (also when it is disabled and not on disk).
QString builtinSkillDescription(const QString& slug);

}  // namespace rk
