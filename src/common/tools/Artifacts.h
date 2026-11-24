#pragma once
// Oversized tool results: the model gets the head (60%) and tail (40%) of the budget plus the path of a file
// holding the whole text, which omc's read tool can page through (it is inside the session cwd).
#include <QString>

namespace rk {

QString capResult(const QString& text, int limit, const QString& artifactsDir, const QString& toolCallId);

}  // namespace rk
