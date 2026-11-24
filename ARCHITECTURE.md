# ARCHITECTURE.md — Rikugan Internals

Rikugan 2.x is a native C++20 / Qt Widgets plugin for IDA Pro 9.3 and Binary Ninja (Linux, macOS). The agent —
turn loop, providers and logins, session files, compaction, retries, skills, MCP and the file/shell/search tools —
is **libomc** ([oh-my-c](https://github.com/buzzer-re/Oh-My-C)), linked into the plugin. Rikugan supplies the
reverse-engineering half: host analysis tools, the chat panel, per-database chats, undo, modes, subagents with host
tools, the bulk renamer, analysis profiles, prompts and skills. (Rikugan 1.x was a Python plugin; it remains in the
git history.)

Build, layout and coding rules: [AGENTS.md](AGENTS.md). Contributor workflow: [DEVELOPMENT.md](DEVELOPMENT.md).
Every model-facing text and its placeholders: [prompts/MANIFEST.md](prompts/MANIFEST.md).

## 1. Layers

```
IDA (src/ida/sdk/Plugin.cpp)        Binary Ninja (src/binja/Plugin.cpp)
  dock widget, HT_UI/HT_IDB hooks     sidebar widget, UIContextNotification, PluginCommands
            \                          /
             PanelCore (src/common/ui)  — one per process, shows the current DbContext
                        |
   DbContext (one per open database): Host, ToolRegistry, ResultCache, ChatIndex
                        |
   ChatSession (UI thread) ── ChatContext (shared with tool workers) ── MutationLog
                        |
   OmcSession: one omc_session_t + its worker thread  ── omc RPC frames ──  libomc
                        |                                                   (agent loop, providers,
   SessionListener ── ToolDispatcher (8 workers) ── Host (main-thread calls)  sessions, MCP, skills)
```

- **Host** (`src/common/host/Host.h`) is everything shared code needs from one open database: paths, a stable
  database id (IDA netnode `$ rikugan`, BN metadata `rikugan_db_id`), cursor, navigation, `runOnMainThread`,
  decompiler presence, binary summary, `registerTools`, the host's own Python, byte read/write. `IdaHost` and
  `BnHost` implement it. Host code never leaks into `src/common`.
- **DbContext** (`agent/DbContext.h`) is created on the UI thread when the panel first shows a database: it registers
  the host's analysis tools and Rikugan's chat tools, and owns the chat index of that database.
- **PanelCore** (`ui/PanelCore.h`) is the panel: a header (chat switcher, Ctrl+K command palette, new chat, overflow
  menu), the activity strip (what the chat does now, agents, todo, goal, mode), the transcript, the agents panel
  (Ctrl+J), the composer (slash palette, model picker on Ctrl+M, queued prompts) and the status line; the bulk
  renamer page, the login sheet, Get Started (shown while no model can answer: sign in, API key, local server or
  the omc agent's logins, then the model) and Settings. The hosts feed it the current database, cursor moves and
  context-menu actions (`agent/Actions.h`: Explain, Rename, Deobfuscate, Find vulnerabilities, Suggest types,
  Annotate, Clean microcode/IL, Xref analysis, Send to Rikugan, Smart Patch on BN, Send to Bulk Rename).
- **Theme** (`ui/Theme.h`) derives every colour from the host's palette (IDA's and Binary Ninja's light and dark
  themes, switched at runtime). The tool-kind, status, address, symbol and code colours come from the host's theme
  when it names them (`setHostColors`: Binary Ninja's `getThemeColor`), else from Rikugan's defaults, all
  contrast-corrected against the host background. The panel's stylesheet is scoped to `#rk_root`.
- **Transcript** (`ui/transcript/`). `TranscriptModel` reduces a chat's frames, its replayed transcript and its cards
  into flat rows: timeline nodes (you, a rikugan step, permission, phase, notices) and step rows (thinking, text, tool
  calls with kind, address/symbol chips, Undo chip and status, a task card per batch of subagents). Consecutive calls
  of one tool fold into a group row with their count. Streamed text is revealed a few characters per display frame
  (a burst lands within about 100 ms), one repaint per growing row per frame. `TranscriptView` is a virtualized
  `QListView` whose delegate paints rows from `QTextDocument`s cached per row revision and width; pending cards are
  live widgets in their rows.

## 2. Embedding libomc

- `cmake/build_omc.sh` builds omc's objects with omc's own Makefile and prelinks them into one relocatable object
  whose only globals are the `omc_*` API (`ld -r` + `objcopy --keep-global-symbols`, `-exported_symbols_list` on
  macOS). omc's internals cannot collide with the host process; the plugin modules export only their entry points
  (`rk_export_only`).
- The whole conversation with omc is its RPC protocol: JSON frames sent with `omc_send` and received through one event
  callback per session. Rikugan uses `prompt`, `get_state`, `get_messages`, `get_last_assistant_text`, `set_model`,
  `fork`, `set_event_filter`, `set_host_tools`, login/account commands, and the control frames `host_tool_result`,
  `host_tool_update`, `extension_ui_response`.
- **Host tools.** `set_host_tools` gives omc the JSON schemas of the visible `ToolSpec`s. When the model calls one,
  omc emits `host_tool_call` and waits for a `host_tool_result` with the same id; `host_tool_cancel` aborts it.
  Mutating, script and `Exclusive` tools are marked exclusive: omc runs them alone and in message order, the rest may
  run in parallel.
- **Event filter.** Each session subscribes to the frames the UI needs (turn/message/tool execution, compaction,
  retry, queue, model and thinking changes, notices) with `messageUpdates:"delta"`; `agent_end` is not requested.
- **AgentHome** (`omc/AgentHome.cpp`) runs once at plugin init, before the first session: it points
  `OMC_CONFIG_DIR` at `~/.rikugan/omc`, disables local model discovery unless enabled, sets a CA bundle for hosts
  that bundle their own libcrypto, and writes `omc/agent/config.yml` (approval mode `yolo` because Rikugan gates
  tools itself; sleep prevention, cache warming, skill-description compression, omc memory and project MCP off),
  `models.yml` from the custom endpoints, omc's project prompt from `prompts/omc/`, and the built-in skills.
- **ServiceSession** is a tool-less, in-memory omc session per process for Settings (model lists, login, logout,
  accounts). Login dialogs arrive as `extension_ui_request` frames. omc refuses to open a session without a model, and
  Settings must work before any login exists, so it opens on `rikugan-service/settings`: a placeholder provider that
  `models.yml` always declares without a key, which omc never counts as available or picks as a default.

## 3. Threads

| Thread | Runs | Never |
|---|---|---|
| Host UI / main thread | Qt widgets, `ChatSession`, `PanelCore`, IDA SDK calls, BN UI calls (`jump_to`) | waits on omc, a session worker or a tool worker (only exception: plugin shutdown after `Host::closing` is set) |
| Session worker (one per `OmcSession`) | `omc_session_new/free`, every non-control `omc_send` (prompts, `compact`, `fork` block for up to a minute), mode jobs | runs on the UI thread |
| omc event threads | the event callback: peek `"type"`, answer request waiters, queue the raw frame | blocks |
| `ToolDispatcher` pool (8 workers, process-wide) | host tool calls through the pipeline; `Blocking` tools (`ask_user`, `spawn_subagent`) get their own thread | — |
| Host main thread via marshalling | `MainThread` tools, pre-state readers, binary summary | — |

- **Control frames** (`host_tool_result`, `host_tool_update`, `extension_ui_response`) and `omc_abort` may be sent from
  any thread; omc handles them inline without queueing behind a running turn.
- **Main-thread marshalling.** `onMain(host, access, f)` runs `f` inline on the main thread, otherwise posts a
  `MainThreadCall` (IDA: `execute_sync` with `MFF_NOWAIT` and `MFF_READ`/`MFF_WRITE`, from `src/ida/sdk/`; BN:
  `ExecuteOnMainThread`) and waits. When `Host::closing` is set, queued calls are abandoned and the caller gets
  "Rikugan is shutting down"; a call never runs against an abandoned caller's stack. Exceptions thrown on the main
  thread are rethrown on the worker.
- **EventQueue** (`omc/EventQueue.h`): omc threads `push()` raw frames; the first push into an empty queue posts one
  `DrainEvent` to the chat's QObject, which drains everything queued so far. One coalesced `postEvent` per burst, no
  timers, nothing while idle. Other cross-thread UI updates use `QMetaObject::invokeMethod(rk::uiAnchor(), ...,
  QueuedConnection)` guarded by `QPointer`.
- **Cards.** Approvals, `ask_user` and mode decisions go through the chat's `ChatUi` bridge:
  the asking worker blocks on a condition until the user answers or the call's `CancelToken` fires; the UI thread only
  shows the card.
- **Unload** (`util/ModuleLifetime.h`). IDA dlcloses `rikugan.so` at exit with its Qt event loop still running, so no
  plugin thread, posted event or timer may outlive it. Background threads start through `spawnThread`; results from
  workers are posted to `uiAnchor()`, a plugin-owned QObject that also parents module-wide timers.
- **Shutdown (IDA).** `~RikuganPlugmod` sets `Host::closing`, deletes the panel synchronously (and with it every
  event posted to its widgets and chats), drops the database, closes the widget, runs `ServiceSession::shutdown` and
  `ToolDispatcher::shutdown`, then `shutdownModule(5000)`: joins the spawned threads and deletes the anchor. Closing
  a database sets `Host::closing` first so queued main-thread work cannot touch it. Binary Ninja never unloads
  plugins; the thread registry is never destroyed, so exiting with threads still running does not terminate.

## 4. Chat lifecycle

```
composer -> CommandRouter::submit -> ChatSession::sendPrompt -> worker: OmcSession::prompt
  prompt -> (omc agent loop: model, host_tool_call ...) -> prompt_result -> get_last_assistant_text
omc frames -> SessionListener -> EventQueue -> ChatSession::frame -> TranscriptModel -> TranscriptView
```

- A chat is a `ChatEntry` in `<sessions>/<db id>/chats.json` (id, omc session file, title, timestamps, fork parent).
  The omc session file is written lazily by omc after the first answer. The title starts as the first prompt; omc
  names the session after its first exchange (as omc and omp do: a short side request, then a `notice` with level
  `title`) and the chat takes that name unless the user renamed it (`title_by_user`; the name is then set on the
  session when it opens, so omc does not rename it). Sessions that are not chats are named when they open.
- `<sessions>/<db id>/database.json` records the host and the file the database came from (`RecentSessions`,
  rewritten when Binary Ninja saves it elsewhere). With no database open, a host that gives the panel a file opener
  (`PanelCore::setFileOpener`, Binary Ninja) lists "Open a binary…" and that host's newest chats. Picking a chat
  opens its file, or focuses the tab that already shows it, and selects that chat. A binary that was never saved as
  a database has no stored id, so the panel tells the host which id to take for that file (`expectSession`).
- The omc session opens lazily (`ensureOpen`) with: cwd = `~/.rikugan/scratch/<db id>/`, session dir =
  `<sessions>/<db id>/`, `resume` = the chat's session file, the configured model and thinking level, the system
  prompt, the omc built-in allow-list and the host tools. On resume the transcript is replayed from `get_messages`
  (context blocks stripped).
- **Ownership.** Chats live in `ui/ChatHub` (one per process, under the module anchor), keyed by database. A panel
  attaches to the chats of the database it shows and detaches when it turns to another one, so a chat keeps running
  while the user looks at another file, and IDA's dock can close and reopen without touching it. The host closes a
  database's chats when it closes that database (`ChatHub::closeDatabase`), and all of them at unload (`closeAll`).
  Closing a chat cancels its mode job and every subagent it started. Host-call cancels are keyed by session and call
  id (omc numbers calls per session), so one chat's Stop never reaches another chat's tool.
- **Subagent conversations.** Every subagent frame is kept in `SubagentRegistry` (`appendFrame`, until "Clean"
  removes the agent). "Open chat" builds a `TranscriptModel` from them (`observeFrames`: the frames so far, then each
  new one, posted to the UI once per burst), so the transcript shows the agent's run like a chat. The composer
  steers it, and the chat switcher lists each chat's agents under it.
- **A message typed during a subagent.** `ChatContext::steerPending` counts the user's messages the agent has not
  taken. A subagent started by `spawn_subagent` or `ask_database` stops when one arrives and its call returns what it
  had so far, marked as cut short, so the agent reads the message at its next step (omc only hands steering over
  between tool calls). Mode subagents and manual ones keep going.
- **Live-session limit:** at most 4 chat sessions are open per process. `ChatSession::touch` keeps an LRU; idle chats
  beyond the limit close and resume from their session file on demand.
- A message typed while the chat works is steered into the running agent (omc `steer`), which reads it at its next
  turn. It shows as queued until omc's `queue_update` says the agent took it. Commands that run a job of their own
  (modes, `/undo`, `/doctor`, a new goal) wait in the queue and go through `CommandRouter` when the run ends; anything
  the run did not take goes out as the next prompt. Stop aborts the turn and drops both. When a run ends, cards still
  open are answered "cancelled" so nothing keeps the composer locked.
- A queued message's **Send now** stops the run and sends that message first, the rest after it.
- **Other databases** (`agent/Databases.h`, Binary Ninja): every `DbContext` registers itself. `list_databases`
  lists them, `open_database` loads a file in the background with `BinaryNinja::Load` (no tab, so the user's view and
  the panel's chats stay put; the user approves the path first), `ask_database` runs a subagent on another database
  with that database's tools in a read-only phase and its own undo list, and `close_database` closes what Rikugan
  opened. They are offered only where the host can load a second database, and not to subagents.
- A session omc refused to start (no model yet) is opened again on the next prompt, so a model connected afterwards
  works without restarting the host. Logins and keys reach omc at once: Rikugan makes it re-read `auth.json`.
- Each typed prompt gets a `<rikugan_context>` block (cursor address, function) unless `auto_context` is off or the
  profile hides metadata; text starting with `/` is sent bare so omc recognises its commands.
- After each prompt the worker runs `AuthSync::run()` and posts `rikugan_prompt_done` with the cached state (model,
  context usage).
- Fork asks omc to `fork` and creates a new `ChatEntry` pointing at the copy. Changing the model sends `set_model`,
  the thinking level `set_thinking_level`.
- Compaction, retries and context limits are omc's (`/compact`, `/retry`, automatic compaction and retry frames are
  shown in the chat).

### Commands

| Input | Handled by |
|---|---|
| `/plan`, `/explore`, `/modify`, `/research` | `ModeDriver` |
| `/undo [N]` | `MutationLog::undo` on a spawned thread (a tool row's Undo chip sends it) |
| `/memory`, `/mcp`, `/doctor`, `/rename` | `CommandRouter` (local output) |
| `/goal <objective>` / `pause` / `resume` / `drop` / `show` | omc's goal mode; the prompt waits for `session_settled` |
| `/btw <question>` | omc's side question, answered beside the running turn |
| `/export` | omc's `export_html`, next to the database (scratch dir without one) |
| `/agents`, `/login`, `/logout`, `/new`, `/fork`, `/todo`, `/model` | `rikugan_ui_action` for the panel |
| `/<skill> args` | omc's `/skill:<skill>`; skills with `mode: plan` / `mode: exploration` start `/plan` / `/modify` with the skill body |
| `/compact`, `/handoff`, `/retry`, `/effort`, `/fast`, `/advisor`, `/model <id>` | passed to omc |
| omc commands unsafe in a host process (`/move`, `/wt`, `/quit` ...) | refused with the reason |
| any other `/command` | refused |

## 5. System prompt

`buildSystemPrompt` (`agent/SystemPrompt.cpp`) assembles the prompt once per omc session from embedded
`prompts/system/**` files (order and placeholders in `prompts/MANIFEST.md`):

1. `base.md` with the host's `intro`, `tool_usage` and `capabilities`.
2. `memory.md`: the first 200 lines of `<database dir>/RIKUGAN.md` through `sanitizeMemory` (20,000-char cap).
3. `current_binary.md`: the host's `get_binary_info` text through `sanitizeBinaryContext` (skipped when the profile
   hides binary metadata).
4. `available_tools.md`: visible host tools plus omc built-ins.
5. `filesystem.md` / `shell.md` for omc's disk tools and `bash` (scratch dir, binary dir, notes dir; each kind asks the
   user once per chat).
6. `databases.md` when the chat can open other databases (Binary Ninja), `host_mcp.md` when the database uses
   Binary Ninja's own MCP tools.
7. Profile sections: singular analysis, custom instructions, restricted functions, active profile and IOC filters.

Skills are listed to the model by omc itself. Mode instructions (`modes/*/explore.md`) travel in the mode's prompts
(or a subagent's task addendum); the `<rikugan_context>` block (`contextPrefix`) is prepended to each user prompt.

## 6. Tools

### Kinds

- **Host analysis tools** — `rk::ToolSpec`s added in `Host::registerTools` (`src/binja/tools/`,
  `src/ida/host/tools/`, SDK-subclassing ones in `src/ida/sdk/`). Name, description and parameters are the
  model-facing contract, kept 1:1 with the 1.x tools.
- **Chat tools** (`agent/ChatTools.cpp`) — `ask_user`, `save_memory`, `spawn_subagent` (depth 0 only),
  `exploration_report` and `research_note` (only while a mode shows them).
- **omc built-ins** — `read`, `write`, `edit`, `ast_edit`, `grep`, `glob`, `bash`, `web_search` and `todo` (not for
  subagents). MCP tools come from omc too. omc asks before each disk or shell call (`tools.approval: prompt` in the
  managed `config.yml`); `Permissions` (`agent/Permissions.h`) intercepts that question: a kind the chat (or a global
  setting) already allows is answered at once, otherwise the transcript shows a permission card (Allow once / Allow
  for this chat / Deny). A chat's subagents share its permissions.
- **Binary Ninja's MCP tools** (`agent/HostMcp.cpp`, `binja/host/BnHostMcp.cpp`) — the `bn_*` tools of Binary
  Ninja's own MCP server (`ui.mcp.enabled`), offered per database once the user agrees (the panel's offer line or
  the header's MCP button, remembered as `hostMcp` in the database's `database.json`). They are host tools with
  the server's own schema (`ToolSpec::inputSchema`, flag `External`). One whose name does not end in a reading
  verb (`_list`, `_get`, `_info`, `_decompile` ...) is `Mutating`, because the server sends no `readOnlyHint`.
  While they are on, Rikugan's own host tools are hidden for that database (`chatToolVisible`) and the chat tools
  stay. The server's active view is global, so `HostMcp::call` serializes calls and first selects the chat's view
  (`bn_binary_view_set_active`, found by file session id). The view switching and file open/save/close tools are
  hidden. `McpClient` speaks streamable HTTP over plain sockets (JSON or event-stream replies, chunked bodies). It
  starts a new session when the server forgot the old one, and Stop cancels it. A system-prompt change needs a new
  omc session, so toggling closes the database's idle sessions (they resume on demand). A busy one gets
  `set_host_tools` at once and closes when its run ends.

Flags (`tools/Tool.h`): `Mutating`, `MainThread` (IDA: every tool; BN: UI and mutating tools), `RequiresDecompiler`,
`Cacheable`, `Script`, `Internal` (undo pre-state readers, never shown), `Exclusive`, `Blocking`, `External`
(served by another program: Binary Ninja's MCP tools).

### Pipeline (`ToolDispatcher::execute`)

1. Unknown or `Internal` tool → error.
2. Tool denied by the active profile → error.
3. Mode phase gate: a read-only phase refuses `Mutating` and `Script` tools.
4. `RequiresDecompiler` without a decompiler → error.
5. `Script` tools always ask (Allow / Always allow for this chat / Deny).
6. `approve_mutations` asks before each `Mutating` tool.
7. Undo pre-state captured for reversible mutations (internal readers via `runDirect`).
8. Result cache lookup (`list_functions`, `list_strings`, `get_binary_info`, `decompile_function`, `function_xrefs`;
   64-entry LRU per database, flushed by any successful mutation).
9. Run — on the host main thread for `MainThread` tools. `ToolError` becomes `Error: <message>`; other exceptions
   become `Unexpected error: ...` and are logged.
10. Mutation record pushed to the chat's `MutationLog`.
11. `stripInjectionMarkers`, then the profile's IOC filter.
12. Artifact cap (`capResult`): over the tool's limit, the model gets the head (60%) and tail (40%) plus the path of
    a file in `<scratch>/artifacts/` with the whole text, which omc's `read` can page.

Partial output goes out as `host_tool_update`. The queue is bounded; overflow and shutdown answer with an error.

## 7. Undo

`MutationLog` (one per chat, in memory, shared with the chat's subagents):

- Reversible: `rename_function`, `rename_variable`, `set_comment`, `set_function_comment`, `set_pseudocode_comment`,
  `rename_address`, `set_function_prototype`. Their pre-state (old name, comment, prototype) is read before the call;
  a reversible tool's success text starts with "Renamed", "Named" or "Set ".
- Known irreversible (recorded as such, `/undo` reports "Cannot undo"): `execute_python`, type creation/application
  tools, byte patches, IL/microcode transforms, optimizer/workflow installs, `redecompile_function`, and every
  `Mutating` tool of Binary Ninja's MCP server. A change that cannot be undone shows no Undo chip and is not counted
  as undoable.
- `/undo N` pops newest first on a non-UI thread; a reverse that can never succeed (`UndoNoLongerPossible`) is dropped,
  any other failure stops and keeps the record. The result cache is flushed after a successful undo.
- File changes made through omc's disk tools are not undone.

## 8. Modes

`ModeDriver::start` runs one busy job on the chat's session worker. It sends prompts in sequence, sets the phase in
`ChatContext` (read-only phases refuse mutations), shows mode-only tools with `set_host_tools` between prompts, counts
`turn_start` frames to cap each phase (aborting the turn on overflow), and asks through decision cards.

| Mode | Phases |
|---|---|
| `/plan` | plan (text-only, read-only) → approve / regenerate with feedback / cancel → execute each step (10 turns each) |
| `/explore` | explore (read-only, `exploration_report` shown, capped by `exploration_turn_limit`) → summary injected |
| `/modify` | explore as a subagent → plan synthesis from the knowledge base → approval → execute (≤ 20 changes, 10 turns each) → save card: Save keeps patches, Discard writes the original bytes back |
| `/research` | explore → document (`research_note`, 15 turns) → index: Obsidian-style notes under `<database dir>/notes/<genre>/` plus `index.md` |

Moving from explore to plan in `/modify` needs a `function_purpose` finding with an address and a high-relevance
hypothesis. `ModeState` holds findings, functions, patches and notes; it is shared with the mode's subagents.

## 9. Subagents

omc's own `task` subagents cannot see host tools, so Rikugan runs its own (`agent/Subagents.cpp`): `runSubagent` opens
a separate omc session (session dir `<sessions>/<db id>/subagents/`) with the parent's database, tools, approvals,
profile and mutation log, at depth + 1 (no further `spawn_subagent`). The task is framed by `prompts/agents/
subagent.md` plus an optional preset addendum (`network_recon`, `report_writer`) and perks. A turn cap aborts runaway
runs. `SubagentRegistry` tracks every subagent of the process with its live progress (current tool, tool count,
tokens, a log of up to 200 entries) for the transcript's task cards and the agents panel (tree, log, Stop, Steer,
send the summary to the chat, "+ New agent").

## 10. Bulk renamer

`BulkRenamerEngine` (`agent/BulkRenamer.h`), driven by the bulk renamer page and the "Send to Bulk Rename"
action:

- **Quick:** pending functions are decompiled one by one (plus `read_disassembly` at the start), cut to 6,000 chars
  each, packed into batches of `bulk_renamer_batch_size` functions and ≤ 180,000 prompt chars, and sent as one prompt
  each to a tool-less, session-less omc worker session. Up to `bulk_renamer_max_concurrent` workers run at once.
  Replies are `0x<addr> <name>` lines.
- **Deep:** one subagent per function (10 turns), answered with `RENAME: 0x<addr> <name>`.
- Pause, resume, cancel (aborts in-flight sessions) and Undo All (restores the original names). Each batch or function
  is a subagent of category `bulk_rename`.

## 11. Settings and files

`RikuganConfig` (`~/.rikugan/config.json`, `RIKUGAN_HOME` overrides the root) is shared by both hosts. The Settings
dialog has five tabs:

- **Provider** — one provider at a time, as in Rikugan 1.x: the provider list is omc's `get_login_providers` plus the
  custom connections; its account status, Log in (the same login sheet as the panel's "Connect a provider") / Log
  out and API key (written to `auth.json`); then its models
  (from `get_available_models`, which lists credentialed models only) and thinking level. Custom connections (+/−)
  hold base URL, API type, key and model ids, written to `models.yml`. Below: "Also use the logins of the omc
  command-line agent" (AuthSync) and local model discovery. omc's setup errors are reworded for Rikugan
  (`omc/UserErrors.h`) here and in the chat.
- **Behavior** — auto context, restore chats on start, don't auto-load chats, session storage dir, exploration turn
  limit, quiet retries, allow file access / shell commands without asking, approve mutations, bulk renamer batch
  size and concurrency.
- **Skills** — enable/disable built-ins (removed from omc's skill dir when disabled), list user skills.
- **MCP** — servers in omc's `mcp.json`.
- **Profiles** — built-in `default` and `private`, custom profiles: denied tools and functions, hide binary metadata,
  IOC filter categories, custom regex rules, custom instructions, singular analysis.

| Path | Content |
|---|---|
| `~/.rikugan/config.json` | Rikugan settings |
| `~/.rikugan/omc/agent/` | `auth.json` (0600), `models.yml`, `mcp.json`, `skills/`, managed `config.yml` |
| `~/.rikugan/sessions/<db id>/` | omc session files, `chats.json`, `database.json`, `subagents/` |
| `~/.rikugan/scratch/<db id>/` | session cwd, `artifacts/`, files the agent writes |
| `<database dir>/RIKUGAN.md` | persistent memory (`save_memory`, `/memory`) |
| `<database dir>/notes/` | `/research` notes |

Built-in skills are embedded in the plugin (`cmake/embed.cmake`) and synced into `omc/agent/skills/<slug>/` with a
`.rikugan-builtin` version marker; a user folder of the same name is never overwritten.

**AuthSync** ("Use my omc logins") merges Rikugan's `auth.json` (R) with standalone omc's (O, `~/.omc/agent/`):
providers only in O are copied to R; for an OAuth login in both, the one that expires later wins on both sides; API keys
in both keep R's; R-only entries never go to O; nothing is deleted. Writes are atomic and 0600, and omc reloads
credentials when R changed. It runs after each prompt, after a login and when Settings closes.

## 12. Security model

Rikugan processes adversarial binaries; strings, names and code from the sample reach the model and the UI.

- **Injection.** Every host tool result passes `stripInjectionMarkers` (zero-width characters, role markers, chat
  template delimiters, the Anthropic magic string, instruction-override phrases, homoglyph-normalised), then the
  profile's IOC filter. Memory and binary context in the system prompt are sanitised and capped; the cursor function
  name in the context block is stripped too. The system prompt's data-integrity section tells the model that tool
  output is data, not instructions.
- **Scripts.** `execute_python` (and any `Script` tool) always asks first, showing the code. It runs in the host's own
  Python (IDAPython; BN's scripting instance driven line by line) after `prompts/python/guard_prelude.py`: an AST
  blocklist and restricted builtins. The agent never runs the target binary.
- **Disk and shell.** omc's file and shell tools ask before their first use in a chat, per kind (read, write, edit,
  shell); "Allow for this chat" covers the chat and its subagents, and Settings can allow files or shell commands
  without asking. They are not sandboxed.
- **Process isolation of omc settings.** The session cwd is Rikugan's scratch dir, never the sample's directory;
  project context files, rules and project MCP are off, so a sample tree cannot inject settings or start processes.
  omc commands that would `chdir` the host process are refused.
- **Credentials** live in `~/.rikugan/omc/agent/auth.json` (0600), written atomically; models.yml with endpoint keys
  is 0600.

**Residual risks.** Sanitisation is pattern-based and cannot catch every phrasing of an injection. With shell access
granted, an injected instruction can reach `bash` with the user's privileges, and file changes are not undoable. The
guard prelude is a blocklist inside the host's Python, not a sandbox: an approved script can do anything the host
can. MCP servers the user configures run as the user. Binary Ninja's own MCP tools are not undoable: their
changes stay unless the user reverts them in Binary Ninja. Approving "Always allow" removes the per-script prompt for that
chat.

## 13. Tests

- `tests/native/` (QtTest, `build/none`): omc's mock model (`../ohmyc/tests/e2e/mock_openai.py`) in a temporary home;
  the omc bridge and managed config, AuthSync merge, the chat core with `FakeHost` (host tool round trip,
  exclusivity and order, cancellation, undo, artifacts, transcript replay), sanitisation and IOC filters, modes, the
  bulk renamer, settings and widgets offscreen.
- `tests/ida/` (`build/ida/rikugan_ida_tooltest`): every model-visible IDA tool against a fresh idalib database of
  `/bin/ls` with the cases in `tool_cases.json`, then `/undo` of a rename.
