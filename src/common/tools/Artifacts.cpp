#include "common/tools/Artifacts.h"

#include <QRegularExpression>

#include "common/util/Files.h"

namespace rk {

QString capResult(const QString& text, int limit, const QString& artifactsDir, const QString& toolCallId) {
    if (limit <= 0 || text.size() <= limit) return text;
    QString id = toolCallId;
    id.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")), QStringLiteral("_"));
    if (id.isEmpty()) id = QStringLiteral("result");
    const QString path = artifactsDir + QLatin1Char('/') + id + QStringLiteral(".txt");
    const bool saved = !artifactsDir.isEmpty() && writeFileAtomic(path, text.toUtf8());
    const int head = limit * 6 / 10;
    const int tail = limit - head;
    const qsizetype elided = text.size() - head - tail;
    const QString note = saved ? QStringLiteral("\n… [%1 chars elided; full output: %2; read it with the read tool using line ranges] …\n")
                                     .arg(elided)
                                     .arg(path)
                               : QStringLiteral("\n… [%1 chars elided] …\n").arg(elided);
    return text.left(head) + note + text.right(tail);
}

}  // namespace rk
