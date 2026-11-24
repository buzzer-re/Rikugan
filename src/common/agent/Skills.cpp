#include "common/agent/Skills.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QStringList>

#include "common/config/Paths.h"
#include "common/util/Embedded.h"
#include "common/util/Files.h"

namespace rk {

namespace {

QString skillsRoot() { return paths::omcAgentDir() + QStringLiteral("/skills"); }

// "---\nkey: value\n...\n---\nbody"
void parse(const QString& text, QString* body, QString* description, QString* mode) {
    if (!text.startsWith(QLatin1String("---"))) {
        if (body) *body = text;
        return;
    }
    const qsizetype end = text.indexOf(QLatin1String("\n---"), 3);
    if (end < 0) {
        if (body) *body = text;
        return;
    }
    const QStringList lines = text.mid(3, end - 3).split(QLatin1Char('\n'));
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QString& l = lines[i];
        if (l.startsWith(QLatin1Char(' ')) || l.startsWith(QLatin1Char('\t'))) continue;  // continuation of a block
        const qsizetype c = l.indexOf(QLatin1Char(':'));
        if (c <= 0) continue;
        const QString k = l.left(c).trimmed();
        QString v = l.mid(c + 1).trimmed();
        if (v == QLatin1String(">") || v == QLatin1String(">-") || v == QLatin1String("|") || v == QLatin1String("|-")) {
            // block scalar: the indented lines that follow (folded with spaces; good enough for one-line uses)
            QStringList parts;
            while (i + 1 < lines.size() && (lines[i + 1].startsWith(QLatin1Char(' ')) || lines[i + 1].startsWith(QLatin1Char('\t'))))
                parts << lines[++i].trimmed();
            v = parts.join(v.startsWith(QLatin1Char('|')) ? QStringLiteral("\n") : QStringLiteral(" "));
        } else if (v.size() >= 2 && (v.startsWith(QLatin1Char('"')) || v.startsWith(QLatin1Char('\''))) && v.endsWith(v[0])) {
            v = v.mid(1, v.size() - 2);
        }
        if (k == QLatin1String("description") && description) *description = v;
        if (k == QLatin1String("mode") && mode) *mode = v;
    }
    if (body) *body = text.mid(text.indexOf(QLatin1Char('\n'), end + 1) + 1).trimmed();
}

}  // namespace

QList<SkillInfo> listSkills() {
    QList<SkillInfo> out;
    const QDir root(skillsRoot());
    for (const QString& slug : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QString md = root.filePath(slug + QStringLiteral("/SKILL.md"));
        if (!QFileInfo::exists(md)) continue;
        SkillInfo s;
        s.slug = slug;
        s.builtin = QFileInfo::exists(root.filePath(slug + QStringLiteral("/.rikugan-builtin")));
        parse(QString::fromUtf8(readFile(md)), nullptr, &s.description, &s.mode);
        out << s;
    }
    return out;
}

QString skillBody(const QString& slug) {
    if (slug.contains(QLatin1Char('/')) || slug.contains(QLatin1String(".."))) return {};
    const QString md = skillsRoot() + QLatin1Char('/') + slug + QStringLiteral("/SKILL.md");
    QString body;
    parse(QString::fromUtf8(readFile(md)), &body, nullptr, nullptr);
    return body;
}

QStringList builtinSkillSlugs() {
    QSet<QString> slugs;
    for (const EmbeddedFile& f : embeddedFiles()) {
        const QString p = QString::fromUtf8(f.path.data(), qsizetype(f.path.size()));
        if (p.startsWith(QLatin1String("skills/"))) slugs.insert(p.section(QLatin1Char('/'), 1, 1));
    }
    QStringList l = slugs.values();
    l.sort();
    return l;
}

}  // namespace rk

namespace rk {

QString builtinSkillDescription(const QString& slug) {
    QString description;
    parse(embeddedText(("skills/" + slug + "/SKILL.md").toStdString()), nullptr, &description, nullptr);
    return description;
}

}  // namespace rk
