#pragma once
// The system prompt of a chat (fixed for the life of its omc session) and the per-prompt context block.
#include <QString>
#include <QStringList>

namespace rk {

struct ChatContext;
struct DbContext;

// The omc built-in tools every session gets. Disk and shell tools are always present: the user approves each kind
// (read, write, edit, bash) once per chat (Permissions.h). Subagents get the same minus todo.
QStringList builtinTools(bool subagent = false);

// Base prompt + host intro/tool usage/capabilities + workspace + filesystem/shell sections + profile sections +
// RIKUGAN.md + current binary. Reads the host (binary info) on the calling thread through onMain when needed.
QString buildSystemPrompt(DbContext& db, const ChatContext& chat, const QStringList& builtinTools);

// "<rikugan_context>\nCurrent address: ...\nCurrent function: ...\n</rikugan_context>\n\n" or empty when
// auto_context is off or the profile hides metadata.
QString contextPrefix(DbContext& db, const ChatContext& chat);
// The user text without a leading context block (transcript replay).
QString stripContextPrefix(const QString& text);

}  // namespace rk
