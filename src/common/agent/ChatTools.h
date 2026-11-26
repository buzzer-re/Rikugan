#pragma once
// Rikugan's own host tools, registered on every database next to the analysis tools:
//   ask_user, save_memory, spawn_subagent, exploration_report, research_note.
#include <QJsonArray>
#include <QString>

namespace rk {

class ToolRegistry;
struct ChatContext;

void registerChatTools(ToolRegistry& registry);

// Whether a chat's model is offered `name` (analysis tools: always; chat tools: by state).
bool chatToolVisible(const ChatContext& chat, const QString& name);

// set_host_tools "tools" for a chat in its current state (call between prompts to change mode-only tools).
QJsonArray chatHostTools(const ChatContext& chat);

// The header a new RIKUGAN.md starts with (save_memory, /modify plan persistence).
const char* memoryFileHeader();

}  // namespace rk
