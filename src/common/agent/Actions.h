#pragma once
// Context-menu actions shared by both hosts:
// each turns the cursor context into a prompt that is sent, or put in the composer.
#include <cstdint>
#include <optional>
#include <vector>

#include <QString>

namespace rk {

struct ActionContext {
    uint64_t ea = 0;                    // cursor address
    std::optional<uint64_t> funcEa;     // start of the function at the cursor
    QString funcName;                   // empty when there is no function
    QString selectedText;               // selection (IDA: "0x<start>-0x<end>" for a range)
};

struct ActionDef {
    QString id;        // IDA action name: "rikugan:<id>"
    QString label;
    QString tooltip;
    bool autoSubmit;   // false: prefill the composer
    QString shortcut;  // IDA hotkey
    bool disasm;       // offered in disassembly views
    bool pseudo;       // offered in pseudocode views
};

// The actions of each host, in menu order.
const std::vector<ActionDef>& idaActionDefs();
const std::vector<ActionDef>& binjaActionDefs();
// The prompt of action `id` for `ctx` on host "ida" or "binja".
QString actionPrompt(const QString& host, const QString& id, const ActionContext& ctx);

}  // namespace rk
