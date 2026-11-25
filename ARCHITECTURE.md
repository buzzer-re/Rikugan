# ARCHITECTURE.md — Rikugan Internals

Rikugan 2.x is a native C++20 / Qt Widgets plugin for IDA Pro (9.3 or later) and Binary Ninja (Linux, macOS, Windows). The
agent — turn loop, providers and logins, session files, compaction, retries, skills, MCP and the file/shell/search
tools — is **libomc** ([oh-my-c](https://github.com/buzzer-re/Oh-My-C)), linked into the plugin. Rikugan supplies the
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
- **Windows.** The hosts are MSVC programs (Qt, C++ ABI), while omc builds with MinGW-w64, so libomc stays a DLL:
  `cmake/build_omc_win.cmake` runs omc's `make WINDOWS=1` (through WSL or MSYS2) or takes a prebuilt `libomc.dll`
  (`OMC_LIB_DIR`, e.g. omc's Windows release), makes an MSVC import library from `libomc.def` and copies the DLL next
  to the modules. Only the C API crosses the boundary. The plugins delay-load the DLL (`/DELAYLOAD:libomc.dll`) because
  neither host searches the plugin's folder for dependencies: `rk::loadOmcLibrary` (`omc/OmcLibrary.h`) loads it by
  full path at plugin init (IDA: `plugins\rikugan\libomc.dll`; Binary Ninja: `plugins\rikugan\libomc.dll` beside
  `plugins\rikugan.dll`, since it loads every top-level DLL as a plugin), checks `omc_api_version`, and keeps it
  loaded for the life of the process. libomc's C runtime is msvcrt.dll with its own copy of the environment, so
  every variable omc reads (`OMC_CONFIG_DIR` …) is set through `rk::setEnv`, which also calls msvcrt's `_putenv_s`.
  In library mode omc installs no crash handler; it adds a console control handler (inert in a GUI host), starts
  Winsock and sets a 1 ms timer resolution.
- The C API Rikugan calls (`OMC_API_VERSION` 2): `omc_session_new/free`, `omc_send`, `omc_abort`, `omc_free`,
  `omc_version`, `omc_api_version`, and `omc_set_result_filter`, which hands Rikugan the text of every tool result
  (omc's built-ins and MCP tools included) before the model, the session file and the events see it. A libomc of
  another API version is refused at load.
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
- **Unload** (`util/ModuleLifetime.h`). IDA unloads `rikugan.so` / `.dll` at exit with its Qt event loop still
  running, so no plugin thread, posted event or timer may outlive it. Background threads start through `spawnThread`;
  results from workers are posted to `uiAnchor()`, a plugin-owned QObject that also parents module-wide timers.
- **Shutdown (IDA).** `~RikuganPlugmod` sets `Host::closing`, deletes the panel synchronously (and with it every
  event posted to its widgets and chats), drops the database, closes the widget, runs `ServiceSession::shutdown` and
  `ToolDispatcher::shutdown`, then `shutdownModule(5000)`: joins the spawned threads and deletes the anchor. Closing
  a database sets `Host::closing` first so queued main-thread work cannot touch it. Binary Ninja never unloads
  plugins; the thread registry is never destroyed, so exiting with threads still running does not terminate.
- **IDA events.** HT_UI and HT_IDB number their events independently (HT_UI 0 is `ui_range`, HT_IDB 0 is
  `closebase`), so the plugin listens to each with its own listener; one shared switch closed the database whenever
  the disassembly range changed.
- **Crashes** (`util/CrashGuard.h`). Installed at plugin init: a fault whose instruction is in Rikugan or libomc
  (Windows: a first-chance vectored handler; Linux/macOS: SIGSEGV/SIGBUS/SIGILL/SIGFPE checked with `dladdr`) or a
  `std::terminate` on a Rikugan thread (every thread calls `crash::guardThread()`) writes
  `~/.rikugan/logs/crash-log-<YYYYMMDD-HHMMSS>-<pid>.log` (reason, address as module + offset, thread, versions,
  the faulting thread's stack), then saves a recovery copy of each open database beside that database
  (`<db>.rikugan-recovered.i64` / `.bndb`; the database file itself keeps its last save) on the host main thread,
  which never waits on Rikugan's threads (30 s deadline), and passes the crash on to the previous handler. Faults in
  host code are left alone. The next start reports logs not seen yet in the host's log. IDA removes the handlers at
  unload.

## 4. Chat lifecycle

```
composer -> CommandRouter::submit -> ChatSession::sendPrompt -> worker: OmcSession::prompt
  prompt -> (omc agent loop: model, host_tool_call ...) -> prompt_result -> get_last_assistant_text
  prompt -> (an omc command that runs without the agent: /compact ...) -> response agentInvoked:false (no prompt_result)
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
  lists them, `open_database` opens a file in a new tab (`databases::setTabOpener`: `UIContext::openFilename`, then
  waits for that file's `OnAfterOpenFile`; the user approves the path first), and `ask_database` runs a subagent on
  another database with that database's tools in a read-only phase and its own undo list. They are offered only where
  the host can open another tab, and not to subagents.
- A session omc refused to start (no model yet) is opened again on the next prompt, so a model connected afterwards
  works without restarting the host. Logins and keys reach omc at once: Rikugan makes it re-read `auth.json`.
- Each typed prompt gets a `<rikugan_context>` block (cursor address, function) unless `auto_context` is off or the
  profile hides metadata; text starting with `/` is sent bare so omc recognises its commands.
- After each prompt the worker runs `AuthSync::run()` and posts `rikugan_prompt_done` with the cached state (model,
  context usage).
- Fork asks omc to `fork` and creates a new `ChatEntry` pointing at the copy. Changing the model sends `set_model`,
  the thinking level `set_thinking_level`.
- Compaction, retries and context limits are omc's (`/compact`, `/retry`, automatic compaction and retry frames are
  shown in the chat). A command omc runs itself without the agent (`/compact`) answers the prompt with
  `agentInvoked:false` and sends no `prompt_result`; that answer ends the prompt.
- **What the user typed.** omc records more user messages than the user typed (goal context, async results, agent
  steering, `[SYSTEM]` notes). `typedUserText` (`agent/ChatExport.h`) decides which ones are the user's and how they
  read (a skill as `/<slug> args`); the live transcript, the replayed one and the Markdown export all use it.
- **Settings changes** reach open chats (`ChatHub::settingsChanged` → `ChatSession::settingsChanged`): an idle chat
  takes the active profile at once and closes its session, so the next prompt reopens it with the new system prompt,
  tools and MCP servers; a busy one does the same when its run ends (tool workers read the profile while it runs).
- **Panels and the full window.** A panel shows one database at a time. Binary Ninja: the sidebar widget, a pane
  ("Open in pane") or the full-window tab; IDA: the dock. The header's expand button (`PanelCore::setWindowToggle`)
  moves the chat into a window-sized tab (Binary Ninja: its own tab, one per window, and the Rikugan sidebar closes;
  IDA: the dock tabbed beside the disassembly) and back. The full-window panel shows `ChatManager` at its right: the
  chats of every database in `databases::all()`, with what each is doing, a "+ New chat" link per database and "Open
  a binary…". Choosing a chat turns the panel to its database (`setDatabase`, then that chat). It refreshes once per
  burst of `ChatHub::chatsChanged` / `databasesChanged`, only while shown. Binary Ninja registers the database of
  every open tab when the full-window tab opens and of each file opened while it exists.

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
  `exploration_report` and `research_note` (only while a mode shows them), and on Binary Ninja `list_databases`,
  `open_database`, `ask_database` (depth 0 only).
- **omc built-ins** — `read`, `write`, `edit`, `ast_edit`, `grep`, `glob`, `bash`, `web_search` and `todo` (not for
  subagents). MCP tools come from omc too. omc asks before each disk or shell call (`tools.approval: prompt` in the
  managed `config.yml`); `Permissions` (`agent/Permissions.h`) intercepts that question: a kind the chat (or a global
  setting) already allows is answered at once, otherwise the transcript shows a permission card (Allow once / Allow
  for this chat / Deny). A chat's subagents share its permissions.
- **Binary Ninja's MCP tools** (`agent/HostMcp.cpp`, `binja/host/BnHostMcp.cpp`) — the `bn_*` tools of Binary
  Ninja's own MCP server. Rikugan probes it with an MCP handshake whatever `ui.mcp.enabled` says, at the address in
  Settings → MCP → Address (`binja_mcp_url`) or else Binary Ninja's `ui.mcp.port` / `ui.mcp.endpoint` (default
  `127.0.0.1:24642/mcp`). `HostMcpBar` probes when a database or the panel shows, then after 2, 5, 10 and 30 s and
  every 60 s until the server answers once for that database (not after an explicit no; one probe at a time, off the
  UI thread). Server tool descriptions are clipped to 140 characters (60 per parameter). The tools are offered per
  database once the user agrees (the panel's offer line or
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
6. `approve_mutations` asks before each `Mutating` tool that is not a `Script` (Allow / Allow for this chat / Deny): a
   script's own card, which shows its code, is its approval. The chat-wide answers, for scripts and for database
   changes, are shared with the chat's subagents both ways.
7. Undo pre-state captured for reversible mutations (internal readers via `runDirect`).
8. Result cache lookup (`list_functions`, `list_strings`, `get_binary_info`, `decompile_function`, `function_xrefs`;
   64-entry LRU per database, flushed by any successful mutation).
9. Run — on the host main thread for `MainThread` tools. `ToolError` becomes `Error: <message>`; other exceptions
   become `Unexpected error: ...` and are logged. A successful `Mutating` tool then calls `Host::databaseChanged`
   (also called after `/undo`). IDA re-decompiles its open pseudocode views there, because Hex-Rays does not redraw
   them for a change made outside them. Binary Ninja's views redraw on their own.
10. Mutation record pushed to the chat's `MutationLog`.
11. `stripInjectionMarkers`, then the profile's IOC filter. omc's own tools (files, shell, web, MCP servers) get the
    same through `omc_set_result_filter` (`SessionListener::filterToolResult`).
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

A cancelled mode leaves `exploration_report` / `research_note` shown until the next prompt finishes, with a
`ModeState` whose `cancelledNotice` they answer with ("the run was cancelled; nothing recorded"), so a "continue"
after Stop does not end in an unknown-tool error. The next mode run clears it.

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
- A rename the host answers with failure text ("No function at …", "Failed to rename") counts as failed
  (`MutationLog::mutationSucceeded`), not done, and Undo All skips it.

## 11. Settings and files

`RikuganConfig` (`~/.rikugan/config.json`, `RIKUGAN_HOME` overrides the root) is shared by both hosts. The Settings
dialog has five tabs:

- **Provider** — one provider at a time, as in Rikugan 1.x: the provider list is omc's `get_login_providers` plus the
  custom connections; its account status, Log in (the same login sheet as the panel's "Connect a provider") / Log
  out and API key (written to `auth.json`); then its models
  (from `get_available_models`, which lists credentialed models only) and thinking level. Custom connections (+/−)
  hold base URL, API type, key, model ids and optionally the context window and maximum output (0: omc's default),
  written to `models.yml` (`contextWindow` / `maxTokens` per model). Below: "Also use the logins of the omc
  command-line agent" (AuthSync) and local model discovery. omc's setup errors are reworded for Rikugan
  (`omc/UserErrors.h`) here and in the chat.
- **Behavior** — auto context, restore chats on start, don't auto-load chats, session storage dir, exploration turn
  limit, quiet retries, allow file access / shell commands without asking, approve mutations, bulk renamer batch
  size and concurrency, and Updates: check for updates once a day (`check_for_updates`), Check now and the last
  outcome (packaged builds only, greyed out in a tree configured without `RIKUGAN_UPDATES`).
- **Skills** — enable/disable built-ins (removed from omc's skill dir when disabled), list user skills.
- **MCP** — servers in omc's `mcp.json`; on Binary Ninja, the address of its own MCP server (`binja_mcp_url`).
- **Profiles** — built-in `default` and `private`, custom profiles: denied tools (Rikugan's and omc's built-ins) and
  functions, hide binary metadata, IOC filter categories, custom regex rules, custom instructions, singular analysis.

Saving the dialog applies to open chats (see Chat lifecycle → Settings changes).

| Path | Content |
|---|---|
| `~/.rikugan/config.json` | Rikugan settings |
| `~/.rikugan/omc/agent/` | `auth.json` (0600), `models.yml`, `mcp.json`, `skills/`, managed `config.yml` |
| `~/.rikugan/sessions/<db id>/` | omc session files, `chats.json`, `database.json`, `subagents/` |
| `~/.rikugan/scratch/<db id>/` | session cwd, `artifacts/`, files the agent writes |
| `~/.rikugan/logs/` | crash logs, `crash-log-<YYYYMMDD-HHMMSS>-<pid>.log` |
| `~/.rikugan/update.json` | when the updater last checked for a release |
| `<database dir>/RIKUGAN.md` | persistent memory (`save_memory`, `/memory`) |
| `<database dir>/notes/` | `/research` notes |

Built-in skills are embedded in the plugin (`cmake/embed.cmake`) and synced into `omc/agent/skills/<slug>/` with a
`.rikugan-builtin` version marker; a user folder of the same name is never overwritten.

**AuthSync** ("Use my omc logins") merges Rikugan's `auth.json` (R) with standalone omc's (O, `~/.omc/agent/`):
providers only in O are copied to R; for an OAuth login in both, the one that expires later wins on both sides; API keys
in both keep R's; R-only entries never go to O; nothing is deleted. Writes are atomic and 0600, and omc reloads
credentials when R changed. It runs after each prompt, after a login and when Settings closes.

**ExternalLogins** ("Use my Claude Code / Codex login", `omc/ExternalLogins.h`, asked once with the providers'
policy links) shares the subscription logins of Claude Code (`~/.claude/.credentials.json`, the macOS keychain item
"Claude Code-credentials" when there is no file) and the Codex CLI (`~/.codex/auth.json`) as R's `anthropic` and
`openai-codex` OAuth credentials: imported when R has none; for the same account the later expiry wins on both sides
(refresh tokens are single-use, so a token omc refreshed is written back to the tool's file or keychain item, other
fields kept); an API key or another account's login in R is left alone. It runs at start, after each prompt and when
turned on.

**Updates** (`update/Update.h`, `update/Updater.h`, `ui/console/UpdateBar.h`). A release package (built with
`RIKUGAN_UPDATES`, which gives it its package name such as `binja-linux-x86_64`) checks the GitHub releases 20 s after
the host starts and then every hour, when a day has passed since the last check and `check_for_updates` is on. Ctrl+K
and Settings check at once. The check is a `curl -I` on `<releases>/latest`: the page it lands on, `tag/v<version>` on
the same site, names the release, and the package URL follows from it. A newer version shows in each panel's update
line (Update, Release notes, Later). Update runs on a worker thread: the package and its `.sha256` downloaded with the
system's curl (https only), the hash checked, the archive unpacked with tar into a scratch folder (regular files
only), the package required to hold the plugin's own file (`util/ModulePath.h`) at the same relative path, and that
file's `RIKUGAN-VERSION:` marker required to say the release's version. Then every file is staged next to its target
as `.rikugan-new`, the old ones renamed to `.rikugan-old-<stamp>` and the new ones moved in. Any failure puts the old
files back. On Linux and macOS the running host keeps its mapped copy. On Windows the loaded DLL can be renamed but not
deleted, so the next start removes the leftovers. The host loads the new version after a restart. `Updater` lives on
the UI thread and only posts results back to `uiAnchor()`. `RIKUGAN_UPDATE_URL` replaces the releases page (the tests
serve one locally).

## 12. Security model

Rikugan processes adversarial binaries; strings, names and code from the sample reach the model and the UI.

- **Injection.** Every tool result (host tools in `ToolDispatcher`; omc's file, shell, web and MCP tools through
  `omc_set_result_filter`) passes `stripInjectionMarkers` (zero-width characters, role markers, chat
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
  is 0600. On Windows the files inherit the user profile's ACL (the user, SYSTEM and administrators).

**Residual risks.** Sanitisation is pattern-based and cannot catch every phrasing of an injection. With shell access
granted, an injected instruction can reach `bash` with the user's privileges, and file changes are not undoable. The
guard prelude is a blocklist inside the host's Python, not a sandbox: an approved script can do anything the host
can. MCP servers the user configures run as the user. Binary Ninja's own MCP tools are not undoable: their
changes stay unless the user reverts them in Binary Ninja. "Allow for this chat" on a script or database-change card
removes that prompt for the rest of the chat.

## 13. Tests

- `tests/native/` (QtTest, `build/none`): omc's mock model (`../ohmyc/tests/e2e/mock_openai.py`) in a temporary home;
  the omc bridge and managed config, AuthSync merge and Claude Code / Codex login sharing, the chat core with
  `FakeHost` (host tool round trip, exclusivity and order, cancellation, undo, artifacts, transcript replay, local
  commands such as `/compact`), disk/shell permissions and the profile's filtering of omc's tools, other databases,
  Binary Ninja's MCP client (handshake, retries, description clipping, address override), the crash guard (in a child
  process), sanitisation and IOC filters, modes (cancelled-mode tools included), the bulk renamer, settings, the
  updater (a local release server: newer, corrupt, wrong-version and plugin-less packages, an unwritable folder, a
  developer build), the transcript reducer and widgets offscreen (cards, chat manager, image paste).
- `tests/ida/` (`build/ida/rikugan_ida_tooltest`): every model-visible IDA tool against a fresh idalib database of
  `/bin/ls` (Windows: `notepad.exe`) with the cases in `tool_cases.json`, then `/undo` of a rename.

## 14. Builds, CI and releases

Each host links its own Qt and its own libraries at load time, so a plugin can be built where the host is not
installed. `-DRIKUGAN_SDK_ONLY=ON` (`build.sh --sdk`, `build.ps1 -Sdk`) does that through `cmake/host_stubs.cmake`: it
links a probe of the plugin (`<module>_probe`) with the host's symbols left unresolved (ELF `-z,defs
--warn-unresolved-symbols`, Mach-O `-undefined,dynamic_lookup` read back with `nm -m`, MSVC `/FORCE:UNRESOLVED`), and
`cmake/host_stubs.py` turns what it needed into stand-in libraries under the hosts' real names (`libbinaryninjaui.so.1`,
`@rpath/libbinaryninjaui.1.dylib`, an import library on Windows, `libQt6Core.so.6` ...). Binary Ninja's core comes from
the API's own stubs. IDA's libraries come from the SDK, and IDA's namespaced Qt (`QT::`, `_QT` in C names) is matched
against the exports of the official Qt of the same version, so a symbol IDA would not have fails the build. The
plugin's NEEDED list is the same as a build against the installed hosts.

CI (`.github/workflows/ci.yml`) and releases (`release.yml`) run `build-tools/ci.sh` (`ci.ps1` for the MSVC steps) on
Linux x86_64 and arm64, macOS arm64 and x86_64 and Windows x64: omc at `build-tools/omc.ref`, the native suite, both
plugins without the hosts, zero compiler warnings, and a smoke check of each package (allowed libraries only, no RPATH,
no debug info, only the host entry points exported, the Windows zip layout). Packages are stripped, with the debug
information in `-symbols` packages. A tag `v<version>` that matches CMakeLists.txt and ida-plugin.json builds the 20
packages onto a draft release, adds `SHA256SUMS` and publishes it once all are there. `install.sh` / `install.ps1` and
the updater install from those assets.
