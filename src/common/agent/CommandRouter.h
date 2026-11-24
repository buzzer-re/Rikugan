#pragma once
// What happens to text the user submits in a chat:
//   /plan /explore /modify /research       -> ModeDriver
//   /undo /memory /mcp /doctor /rename     -> handled by Rikugan
//   /goal /btw /export                     -> omc's goal mode, side questions, HTML export (RPC)
//   /agents /login /logout /new /fork /todo, /model without argument
//                                          -> {"type":"rikugan_ui_action","action":...,"arg":...} for the panel
//   /compact /handoff /retry /effort /fast /advisor, /model <id>
//                                          -> omc's own commands
//   /<skill> args                          -> /skill:<skill> args (mode skills start their mode)
//   omc commands unsafe in a host process  -> refused with the reason (never sent to the model)
//   any other /command                     -> refused
//   anything else                          -> a prompt
#include <QJsonArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace rk {

class ChatSession;

struct CommandInfo {
    QString name;         // without the slash
    QString group;        // rikugan | agent | session | skill
    QString icon;         // one glyph
    QString description;
    QString argHint;      // "<objective>", "[N]" or empty
};

class CommandRouter {
public:
    // `images`: omc image blocks sent with a message (a command ignores them)
    static void submit(ChatSession& chat, const QString& text, const QJsonArray& images = {});
    // Every command the palettes offer, skills last.
    static QList<CommandInfo> catalog();
    // "/name" of every catalog entry (the composer's autocomplete).
    static QStringList completions();
};

}  // namespace rk
