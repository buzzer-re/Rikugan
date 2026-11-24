# Prompt manifest

The texts here are what the model sees. Each section names the source under `src/` that loads them.

Conventions:
- Every file ends with exactly one newline. Strip that newline when you load the file.
- `{{name}}` is a plain string substitution. No other template syntax is used.
- Addresses are written as `rk::hex(ea)`, for example `0x401000`.
- "Section" files are appended to the prompt they extend, with one blank line (`"\n\n"`) between sections, unless a line below says otherwise.

Skills live in `skills/<slug>/` and are not listed here. Their frontmatter `name` is the slug, because omc uses `name` as the skill id for `/slug` and `skill://slug`. The display names in Rikugan 1.x were:

| Slug | Display name |
|---|---|
| binja-scripting | Binary Ninja Scripting |
| ctf | CTF Challenge |
| deobfuscation | Deobfuscation (`mode: plan`) |
| driver-analysis | Driver Analysis |
| generic-re | General Reverse Engineering |
| ida-scripting | IDA Scripting |
| linux-malware | Linux Malware Analysis |
| malware-analysis | Malware Analysis |
| modify | Binary Modification (`mode: exploration`) |
| smart-patch-binja | Smart Patch (Binary Ninja) |
| smart-patch-ida | Smart Patch (IDA Pro) |
| vuln-audit | Vulnerability Audit |

## System prompt (`buildSystemPrompt` order, `src/common/agent/SystemPrompt.cpp`)

- `system/base.md` — the base prompt.
  - `{{intro}}` = `system/<host>/intro.md`.
  - `{{tool_usage}}` = `system/<host>/tool_usage.md`.
  - `{{capabilities}}` = `system/<host>/capabilities.md`.
  - `<host>` is `ida` (IDA Pro) or `binja` (Binary Ninja).
- `system/ida/intro.md`, `system/binja/intro.md` — persona and host engine line. No placeholders.
- `system/ida/tool_usage.md`, `system/binja/tool_usage.md` — the "Tool Usage -- CRITICAL" section (execute_python as a last resort). No placeholders.
- `system/ida/capabilities.md`, `system/binja/capabilities.md` — the "Capabilities" section. No placeholders.
- `system/memory.md` — section with the RIKUGAN.md contents, added when the file exists and is non-empty.
  - `{{memory}}` = `sanitizeMemory()` of the first 200 lines of `<binary dir>/RIKUGAN.md`, trimmed. When the file is cut, add a line made from `system/memory_truncated.md`.
- `system/memory_truncated.md` — truncation line for the memory text.
  - `{{max_lines}}` = `200`.
  - It is appended as `"\n" + line`, after the 200th line.
- `system/current_binary.md` — section, skipped when the profile hides binary metadata.
  - `{{binary_info}}` = `sanitizeBinaryContext()` of the host's binary summary.
- The cursor position is not in the system prompt: each user prompt starts with a `<rikugan_context>` block
  (current address and function) built in `SystemPrompt.cpp` (`contextPrefix`), so the prompt stays fixed for the
  session and the position is always current.
- `system/available_tools.md` — section.
  - `{{tool_names}}` = the tool names joined with `", "`.
- `system/filesystem.md` — section, added when the chat has the omc file tools (`read`, `write`, `edit`, `grep`, `glob`).
  - `{{scratch_dir}}` = absolute path of the session working directory, which is the per-database scratch dir.
  - `{{binary_dir}}` = absolute path of the analysed binary's directory (injection markers stripped, since a path can carry a hostile file name).
  - `{{notes_dir}}` = absolute path of the research notes directory (`<binary dir>/notes`).
- `system/shell.md` — section, added after `filesystem.md` when `bash` is enabled. No placeholders.
- `system/databases.md` — section, added when the host can open a second database (Binary Ninja) and the chat is not a
  subagent: `list_databases`, `open_database`, `ask_database`. No placeholders.
- `system/host_mcp.md` — section, added when the user switched the database to the host's own MCP tools (Binary
  Ninja's MCP server, `HostMcp`) and the server listed tools.
  - `{{server}}` = the host's name (`Binary Ninja`).
- Skills are listed by omc itself (it appends them to a custom system prompt), so there is no skills section here.
- `system/profile/singular_analysis.md` — section, added when `profile.singular_analysis` is set. No placeholders.
- `system/profile/instructions.md` — section, added when `profile.custom_filters` is non-empty.
  - `{{custom_filters}}` = the filters joined with `"\n"`.
- `system/profile/restricted_functions.md` — section, added when `profile.denied_functions` is non-empty.
  - `{{denied_functions}}` = one `- <name>` line per function.
- `system/profile/active.md` — section, added when the profile name is not `default`.
  - `{{profile_name}}` = the profile name.
  - `{{profile_description}}` = the description followed by an extra `"\n"`, which gives a blank line before "You are operating…". When the description is empty, remove the placeholder together with its line break.
- `system/profile/ioc_filters.md` — appended to `active.md` after a blank line, when at least one IOC category is on.
  - `{{ioc_categories}}` = one `- <label>` line per enabled category. The labels, from `IOC_FILTER_CATEGORIES`:
    - hashes: File hashes (MD5, SHA1, SHA256)
    - ipv4: IPv4 addresses
    - ipv6: IPv6 addresses
    - domains: Domain names
    - urls: URLs (http/https/ftp)
    - registry_keys: Windows registry keys
    - file_paths: File paths (Windows & Unix)
    - emails: Email addresses
    - crypto_wallets: Cryptocurrency wallet addresses
    - mutexes: Mutex / named object names

## Plan mode (`src/common/agent/ModeDriver.cpp`)

- `modes/plan/generation.md` — user message that asks for a numbered plan (text-only turn, no tools).
  - `{{user_message}}` = the user's request.
- `modes/plan/skill_generation.md` — replaces `generation.md` when the plan is started by a skill with `mode: plan`.
  - `{{skill_name}}` = the skill slug.
  - `{{skill_body}}` = the SKILL.md body without its frontmatter.
  - `{{user_message}}` = the request text that follows the slash command.
- `modes/plan/regenerate.md` — user message sent after a rejected plan, followed by another text-only turn.
  - `{{feedback}}` = `" Their feedback: <text>"`, or empty when the user only pressed Regenerate.
- `modes/plan/step.md` — user message for each approved step. Each step runs at most 10 turns and ends when the model makes no tool calls.
  - `{{index}}` = 1-based step number.
  - `{{description}}` = the step text.
- Plan parsing: every line matching `^\d+[.)]\s*(.+)$` is a step, and parsing stops at a line `END_PLAN`.

## Exploration mode (`src/common/agent/ModeDriver.cpp`)

- `modes/exploration/explore.md` — exploration instructions. No placeholders. Never part of the system prompt: in-chat exploration puts it before the user request in the explore phase's first message; the isolated explore subagent gets it as its system addendum, and the parent then puts it at the start of its plan-synthesis message.
  - The plugin drives the phases. Explore ends when the model replies without tool calls, or when the turn limit is hit.
  - Moving on to plan needs at least one `function_purpose` finding with an address, and at least one hypothesis with `relevance="high"`.
- `modes/exploration/prior_context.md` — first user message of an isolated explore subagent.
  - `{{conversation_digest}}` = the last 6 user/assistant messages, each `User: <text>` or `Rikugan: <text>` cut to 600 chars, joined with a blank line. Omit the message entirely when there are none.
- `modes/exploration/subagent_summary.md` — user message injected into the parent after explore ran as a subagent.
  - `{{knowledge_summary}}` = the knowledge summary, described at the end of this section.
- `modes/exploration/explore_only_summary.md` — user message after an explore-only run (`/explore`).
  - `{{knowledge_summary}}` = as above.
- `modes/exploration/plan_synthesis.md` — user message that starts the plan phase (text-only turn).
  - `{{knowledge_summary}}` = as above.
- `modes/exploration/plan_regenerate.md` — user message after a rejected modification plan.
  - `{{feedback}}` = `" Their feedback: <text>"` or empty.
  - `{{plan_synthesis}}` = the rendered `plan_synthesis.md`.
- `modes/exploration/execute.md` — user message for each approved change. Each change runs at most 10 turns, and there are at most 20 changes.
  - `{{index}}` = 1-based change number.
  - `{{total}}` = number of changes.
  - `{{description}}` = the plan line.
- `modes/exploration/save.md` — user message sent after the user chooses Save. No placeholders.
- `modes/exploration/discard_restored.md` — user message after Discard when the original bytes were written back. No placeholders.
- `modes/exploration/discard_kept.md` — user message after Discard when the rollback could not run. No placeholders.
- Knowledge summary format (`KnowledgeBase::to_summary`), with sections joined by `"\n"`:
  - The header `## Exploration Summary\n\nGoal: <goal>\n`.
  - `### Relevant Functions`, then `` - `0x<addr>` <name>: <summary>[ [relevance]] ``. The relevance tag is shown unless the relevance is medium.
  - `\n### Relevant Strings`, then `` - `0x<addr>`: "<value>" ``.
  - `\n### Relevant Imports`, then `- <name>`.
  - `\n### Hypotheses`, then `N. <hypothesis>`.
  - `\n### All Findings (<n> total)`, then `- [<category>] <0xaddr|N/A>: <summary>`.
  - Empty sections are omitted.

## Research mode (`src/common/agent/ModeDriver.cpp`)

- `modes/research/explore.md` — section appended to the system prompt in all research phases. No placeholders.
  - During explore, `research_note` is removed from the tool list.
  - If fewer than 2 explore turns run, the note-writing phase is skipped.
- `modes/research/document.md` — user message that starts note writing. The phase runs at most 15 turns and ends on a reply without tool calls.
  - `{{knowledge_summary}}` = as in exploration mode.
- `modes/research/index.md` — contents of `<notes dir>/index.md`, written when at least one note exists.
  - `{{binary_name}}` = the database file basename.
  - `{{goal}}` = the user's request.
  - `{{genre_sections}}` = the rendered `index_genre.md` blocks in genre order, joined with a blank line.
- `modes/research/index_genre.md` — one genre block in the index.
  - `{{genre_title}}` = the genre with `-` replaced by a space, then title-cased.
  - `{{note_lines}}` = one `- [[<slug>]] — <title>` line per note.
- `modes/research/complete.md` — final text, shown to the user and added to the session as `"[SYSTEM] " + text`.
  - `{{note_count}}` = number of notes written.
  - `{{notes_dir}}` = the notes directory.
  - `{{note_lines}}` = one `- **<title>** (<genre>)` line per note.
- `modes/research/complete_empty.md` — the same final text, used when no notes were written.
  - `{{note_count}}` = `0`.
  - `{{notes_dir}}` = the notes directory.
- Note file location: `<notes dir>/<slug(genre)>/<slug(title)>.md`. Slug: NFKD → ASCII, lowercase, drop `[^\w\s-]`, collapse `[-\s]+` to `-`, trim `-`, use `untitled` when empty.

## Subagents (`src/common/agent/Subagents.cpp`)

- `agents/subagent.md` — the task message a subagent receives.
  - `{{max_turns}}` = the turn limit (default 20).
  - `{{task}}` = the task text.
  - When there is a system addendum, the message is `addendum + "\n\n" + this`.
- `agents/network_recon.md` — addendum for agent type `network_recon`. No placeholders.
  - Default max turns: 30.
  - Append `perks/header.md` plus the perks `import_mapping`, `string_harvesting` and `deep_decompilation`, after a blank line.
- `agents/report_writer.md` — addendum for agent type `report_writer`. No placeholders. Default max turns: 5.
- `agents/perks/header.md` — first line of the perks addendum.
  - Addendum = header + `"\n\n"` + the selected perk texts joined with `"\n\n"`.
  - Unknown keys are skipped; when none remain, there is no addendum.
- `agents/perks/deep_decompilation.md`, `string_harvesting.md`, `import_mapping.md`, `memory_layout.md`, `hypothesis_mode.md` — one perk text each. No placeholders.

## Bulk renamer (`src/common/agent/BulkRenamer.cpp`)

- `bulk_renamer/quick.md` — single user message with no tools and no history.
  - `{{functions}}` = the rendered `quick_function.md` parts joined with `"\n"`.
  - A batch holds at most `batch_size` functions and 180,000 chars.
  - Parse the reply line by line against `^0x([0-9a-fA-F]+)\s+(\S+)$`.
- `bulk_renamer/quick_function.md` — one function in a quick batch; the whole part is capped at 6,000 chars.
  - `{{address}}` = `0x<addr>`.
  - `{{current_name}}` = the current name.
  - `{{decompiled}}` = `decompile_function` output.
  - `{{disassembly_block}}` = the rendered `quick_disassembly.md`, or empty when there is no disassembly.
  - A part that is too long is cut and gets `"\n// ... (truncated)\n"` appended.
- `bulk_renamer/quick_disassembly.md` — the disassembly part of a quick-batch function.
  - `{{disassembly}}` = `read_disassembly` output at the function address.
  - `read_disassembly` is called with `limit: 100`.
- `bulk_renamer/deep.md` — subagent task, framed by `agents/subagent.md` with max turns 10.
  - `{{address}}` = `0x<addr>`.
  - `{{current_name}}` = the current name.
  - `{{decompiled}}` = `decompile_function` output.
  - Parse the final text with `RENAME:\s*0x([0-9a-fA-F]+)\s+(\S+)`.

## Context-menu actions (`src/common/agent/Actions.cpp`)

For every action:
- `{{func_name}}` = the function name at the cursor, or `sub_<hex ea>` when there is none.
- `{{address}}` = `0x<hex>` of the function start, or of the cursor address when there is no function.

| File | Action | Extra placeholders and notes |
|---|---|---|
| `actions/send_to.md` | Send to Rikugan (not auto-submitted) | Used when there is no selection and a function name exists. Here `{{address}}` is the cursor address. With a selection, the input is the selected text verbatim. In IDA a range selection is `0x<start>-0x<end>`. |
| `actions/send_to_address.md` | Send to Rikugan, no function | `{{address}}` = the cursor address. |
| `actions/explain.md` | Explain this | |
| `actions/rename.md` | Rename with Rikugan | |
| `actions/deobfuscate.md` | Deobfuscate with Rikugan | `{{optimizer_term}}` = `microcode` (IDA) or `IL` (Binary Ninja). |
| `actions/vuln_audit.md` | Find vulnerabilities | |
| `actions/suggest_types.md` | Suggest types | |
| `actions/annotate.md` | Annotate function | |
| `actions/clean.md` | Clean microcode (IDA) / Clean IL (Binary Ninja) | `{{ir_term}}` = `microcode` or `IL`. |
| `actions/xref_analysis.md` | Xref analysis | |
| `actions/smart_patch.md` | Smart Patch (Binary Ninja only, not auto-submitted) | The text ends with `"Desired behavior: "`, trailing space included, so the user can type after it. |
