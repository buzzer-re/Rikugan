#pragma once
// /plan, /explore, /modify and /research: one busy job on the chat's session
// worker that sends prompts in sequence, gates tools by phase (ChatContext::setPhase), adds the mode-only tools
// between prompts (set_host_tools) and asks the user through ChatUi::askDecision.
#include <QString>

namespace rk {

class ChatSession;

enum class Mode { Plan, Explore, Modify, Research };

class ModeDriver {
public:
    // UI thread. `skillSlug` names a skill whose frontmatter selected the mode (its body is prepended to the goal).
    static void start(ChatSession& chat, Mode mode, const QString& goal, const QString& skillSlug = {});
};

}  // namespace rk
