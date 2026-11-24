#pragma once
// A chat's omc messages as a Markdown file ("Export as Markdown"): user and assistant text, tool calls with their
// arguments and results (results cut at 2,000 characters, the sanitizer's wrapping removed).
#include <QJsonArray>
#include <QString>

namespace rk {

bool writeMarkdownExport(const QJsonArray& messages, const QString& path, const QString& model, const QString& file,
                         QString* error);

}  // namespace rk
