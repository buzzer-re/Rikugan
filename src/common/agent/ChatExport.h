#pragma once
// A chat's omc messages as a Markdown file ("Export as Markdown"): user and assistant text, tool calls with their
// arguments and results (results cut at 2,000 characters, the sanitizer's wrapping removed).
#include <optional>

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace rk {

// The line a user-role omc message shows as the user's own (live transcript, replay, export), or nullopt when the
// user never typed it: Rikugan's internal prompts ([SYSTEM]), omc's injections (custom messages such as goal context
// or job results, shell and hook output, file mentions), turns marked synthetic or written by the agent. A /skill
// invocation shows as typed in Rikugan ("/<slug> args", not omc's "/skill:<slug>" or the expanded skill); attached
// images add "[N images]"; the per-prompt context block is stripped.
std::optional<QString> typedUserText(const QJsonObject& message);

bool writeMarkdownExport(const QJsonArray& messages, const QString& path, const QString& model, const QString& file,
                         QString* error);

}  // namespace rk
