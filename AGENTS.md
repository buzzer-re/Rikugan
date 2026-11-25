# AGENTS.md — Rikugan Developer Guide

## Project Overview

Rikugan (六眼) is a reverse-engineering agent that lives inside **IDA Pro** and **Binary Ninja** as a native C++ plugin.
The agent itself — the turn loop, every model provider and login, sessions, compaction, retries, skills, MCP, and the
file/shell/search tools — is **libomc** ([oh-my-c](https://github.com/buzzer-re/Oh-My-C), expected next to this
repository as `../ohmyc`), linked into the plugin. Rikugan adds what is specific to reverse engineering: the host
analysis tools, the Qt chat panel, per-database chats, undo of database changes, the plan/explore/modify/research
modes, subagents that can use the host tools, the bulk renamer, analysis profiles, and the RE prompts and skills.

Platforms: Linux, macOS and Windows 10/11 x64 (libomc.dll, omc's MinGW-w64 build, next to the MSVC-built plugin).
Hosts: IDA Pro 9.3 or later (built against the 9.3 SDK), Binary Ninja (built against the API revision in
`<BN install>/api_REVISION.txt`).

## Directory Structure

```
CMakeLists.txt            one host per build tree: RIKUGAN_HOST = binja | ida | none (tests)
cmake/                    omc.cmake (libomc: prelink on Linux/macOS, build_omc.sh; DLL + import library on Windows,
                          build_omc_win.cmake), embed.cmake (skills/ + prompts/ -> plugin bytes), host_stubs.cmake +
                          host_stubs.py (RIKUGAN_SDK_ONLY: stand-ins for the hosts' libraries, no host installed)
mk/omc-vars.mk            asks omc's Makefile for its library objects and link flags
third_party/              binaryninja-api (pinned to BN's api_REVISION), ida-sdk (v9.3.0-sdk.3) — git submodules
tools/                    fetch_qt.sh / fetch_qt.ps1 (official Qt headers/tools matching each host: 6.11.1 BN, 6.8.2
                          IDA), vsenv.ps1 (MSVC environment for PowerShell)
build.sh, build.ps1       build the plugins and package the release assets into dist/ (--sdk / -Sdk: without the hosts)
install.sh, install.ps1   install a release's packages (or dist/ with --from / -From) into the hosts' plugins folders
build-tools/              ci.sh / ci.ps1 (every CI and release step), omc.ref (the omc commit releases build against),
                          release_notes.sh (release text: docs/releases/v<version>.md + install section + assets)
.github/workflows/        ci.yml (five platforms on every push and PR), release.yml (tag v<version> -> GitHub release)
prompts/                  every model-facing text (MANIFEST.md lists files and placeholders)
skills/                   built-in skills (synced into Rikugan's omc agent dir at start)
src/common/
  omc/                    OmcSession (one libomc session + its worker), EventQueue, AgentHome, ServiceSession, AuthSync,
                          ExternalLogins (Claude Code / Codex), LoginFlow (provider logins), UserErrors, YamlWriter,
                          OmcLibrary (loads libomc.dll on Windows)
  tools/                  Tool.h (ToolSpec/ToolCall), ToolRegistry, ToolDispatcher, MutationLog, ResultCache, Artifacts
  host/                   Host interface (one open database), MainThreadCall, onMain()
  agent/                  DbContext, ChatIndex, ChatSession, ChatContext, ChatTools, Permissions, Subagents, ModeDriver,
                          ModeState, SystemPrompt, CommandRouter, Skills, Actions, BulkRenamer, ChatExport, HostMcp,
                          Databases (every open database; Binary Ninja's tab opener), SessionListener
  mcp/                    McpClient (streamable HTTP, for the host's own MCP server)
  security/               Sanitize (injection markers, IOC redaction)
  config/                 RikuganConfig (~/.rikugan/config.json), Profile, Paths
  update/                 Update (find, download, check and swap in a release package), Updater (daily check, offer)
  ui/                     PanelCore, Theme (colours from the host palette), Syntax, WelcomeView;
                          transcript/ (TranscriptModel reducer, TranscriptView + delegate, Cards);
                          console/ (ConsoleHeader, ActivityStrip, Composer, Palette, ModelPicker, StatusLine,
                          AgentsPanel, LoginSheet, GetStarted, HostMcpBar, ChatManager); tools/ (bulk renamer page);
                          settings/; ChatHub (every live chat, outside the panels)
  util/                   Files, Env (environment shared with libomc, home folder), Log, Embedded, CrashGuard,
                          ModuleLifetime, ModulePath (the plugin's own file)
src/binja/                Plugin.cpp (sidebar, notifications, commands), host/ (BnHost, Python, BnHostMcp), tools/
src/ida/                  sdk/Plugin.cpp (dock, hooks, actions), host/ (IdaHost, tools), sdk/ (SDK subclasses, -fno-rtti)
tests/native/             QtTest suite (none tree) against omc's mock model server
tests/ida/                idalib tool smoke test (rikugan_ida_tooltest)
```

## Building

```bash
tools/fetch_qt.sh 6.11.1            # Binary Ninja's Qt (also used by the test tree); Windows: tools\fetch_qt.ps1
tools/fetch_qt.sh 6.8.2             # IDA 9.3's Qt (built with -qtnamespace QT; we compile with QT_NAMESPACE=QT)
cmake -S . -B build/none  -G Ninja -DRIKUGAN_HOST=none  -DCMAKE_PREFIX_PATH=~/Qt/6.11.1/<kit> && cmake --build build/none
cmake -S . -B build/binja -G Ninja -DRIKUGAN_HOST=binja -DCMAKE_PREFIX_PATH=~/Qt/6.11.1/<kit> && cmake --build build/binja
cmake -S . -B build/ida   -G Ninja -DRIKUGAN_HOST=ida   -DCMAKE_PREFIX_PATH=~/Qt/6.8.2/<kit>  && cmake --build build/ida
./ci-local.sh                       # tests + every host plugin available on this machine (Windows: .\ci-local.ps1)
./build.sh                          # build and package the hosts found into dist/ (Windows: .\build.ps1)
./build.sh --sdk                    # both hosts without them installed, as CI and releases build (Windows: -Sdk)
./install.sh --from dist            # install them (without --from: the latest GitHub release; Windows: install.ps1)
```

- libomc is prelinked into one object whose only globals are `omc_*` (`cmake/build_omc.sh`, the same recipe as
  omc's `make lib`), so omc's internals never clash with the host's symbols. The plugins export only their entry
  points (`PLUGIN`; `CorePluginABIVersion`, `CorePluginInit`, `UIPluginABIVersion`, `UIPluginInit`).
- Windows: the plugins are MSVC builds (the hosts' Qt and C++ ABI); libomc is omc's MinGW-w64 `libomc.dll` behind its
  C API, linked through an import library made from `libomc.def` and delay-loaded. `rk::loadOmcLibrary` loads it at
  plugin init from the plugin's folder (IDA: `plugins\rikugan\`) or its `rikugan\` subfolder (Binary Ninja, which
  loads every top-level DLL in `plugins\` as a plugin). The DLL has its own C runtime (msvcrt.dll): memory omc returns
  is freed with `omc_free`, and environment variables omc reads are set with `rk::setEnv` (`util/Env.h`), which updates
  that runtime's copy too.
- Rikugan needs libomc's C API version 2 (`OMC_API_VERSION`, for `omc_set_result_filter`); `rk::loadOmcLibrary`
  refuses another version. A release must ship a libomc built from an omc that has it.
- Each host links its own Qt at runtime. The official Qt headers only provide declarations and moc; IDA's Qt is
  namespaced, Binary Ninja's is not, so the UI code is compiled once per host tree. On Windows the IDA tree links the
  SDK's import libraries for IDA and its Qt, so no IDA install is needed to build it.
- A Binary Ninja update that bumps the core/UI ABI needs the `third_party/binaryninja-api` submodule moved to the new
  `api_REVISION.txt` commit and a rebuild.
- `-DRIKUGAN_SDK_ONLY=ON` (`build.sh --sdk`, `build.ps1 -Sdk`) builds both plugins with neither host installed:
  `cmake/host_stubs.cmake` links a probe of the plugin with the host's symbols unresolved, and `host_stubs.py` makes
  stand-in libraries with the hosts' real names and exactly those symbols (Binary Ninja's core from the API's stubs,
  IDA's Qt matched against the official Qt of the same version). The plugin binds to the real libraries at load.
- `-DRIKUGAN_UPDATES=ON` (set by `build.sh` / `build.ps1`, so by every package) gives the plugin its package name
  (`binja-linux-x86_64` ...), so it can update itself from the GitHub releases. A tree configured without it never
  updates (Settings greys the option out).
- CI (`.github/workflows/ci.yml`) runs `build-tools/ci.sh` on Linux x86_64/arm64, macOS arm64/x86_64 and Windows x64:
  tests, both plugins without the hosts and a smoke check of each package, with zero compiler warnings. A tag
  `v<version>` runs `release.yml`: the same builds, 20 packages (with `-symbols` packages) and `SHA256SUMS` on a draft
  release that is published once complete. A manual run of it is a dry run on a draft it deletes.

## How a Chat Works

```
composer text -> CommandRouter -> ChatSession::sendPrompt -> OmcSession worker: omc_send({"type":"prompt"})
omc agent loop -> host_tool_call -> SessionListener -> ToolDispatcher (8 workers) -> host_tool_result
omc events -> EventQueue -> one coalesced QEvent -> ChatSession::frame -> TranscriptModel -> TranscriptView
```

1. `PanelCore` holds the current `DbContext` (one per open database: `Host`, `ToolRegistry`, `ResultCache`,
   `ChatIndex`). Each chat is a `ChatSession` (with its `TranscriptModel`) owned by `ChatHub`, not by the panel:
   switching Binary Ninja tabs or closing IDA's dock leaves running chats, their undo history and grants alone, and
   a database's chats close only when the host closes it. Its omc session is opened lazily and resumes the chat's
   session file. At most 4 omc sessions are live per process; idle ones beyond that close and resume on demand.
2. `OmcSession` owns one `omc_session_t` and one worker thread. Every `omc_session_new`, `omc_session_free` and
   non-control `omc_send` runs on that worker. Control frames (`host_tool_result`, `host_tool_update`,
   `extension_ui_response`) and `omc_abort` may come from any thread.
3. The session is opened with Rikugan's system prompt (`SystemPrompt.cpp` from `prompts/system/**`), the omc built-in
   tools (`read`, `write`, `edit`, `ast_edit`, `grep`, `glob`, `bash`, `web_search`, `todo`; each disk/shell kind asks
   once per chat, see Security) and the host tools (`set_host_tools`). Mutating and script tools are `exclusive`: omc
   runs them alone, in message order. omc names a chat after its first exchange (a `notice` with level `title`);
   `ChatSession` adopts it unless the user renamed the chat. Every other session that prompts (subagents, renamer
   workers) is named when it opens, so omc spends no model call titling it.
4. omc's event callback only peeks at `"type"`, answers request waiters and queues the raw frame. `message_update`
   comes as deltas (`set_event_filter` with `messageUpdates:"delta"`); `agent_end` is filtered out. A prompt ends at
   its `prompt_result`, or at a response with `agentInvoked:false` (an omc command that ran without the agent, such
   as `/compact`, sends no `prompt_result`).
5. Tool calls run through `ToolDispatcher::execute`: profile denial → mode phase gate → decompiler requirement →
   script approval → `approve_mutations` → undo pre-state → run (on the host main thread for `MainThread` tools) →
   view refresh (`Host::databaseChanged`, after a change: IDA re-decompiles its open pseudocode views) → mutation
   record → result cache → injection-marker strip → IOC filter → artifact cap (oversized results keep head and tail;
   the whole text goes to a file in the session's working directory that omc's `read` can page).
6. Cards (approvals, `ask_user`, plan/save decisions, disk/shell permissions) block the asking worker, never the UI
   thread. They are live widgets in their transcript rows.

The panel: `TranscriptModel` turns frames, the replayed transcript and cards into flat rows (timeline nodes, tool
rows, subagent task cards); which user messages show is decided by `typedUserText` (`agent/ChatExport.h`), shared
with the Markdown export, so omc's injected messages stay out of all three; streaming changes only the tail row, repainted at most every 16 ms. `TranscriptView` is a
virtualized `QListView`; its delegate paints rows from cached `QTextDocument`s. Pickers (slash, Ctrl+K, Ctrl+M,
chat switcher) are child widgets built once and only filtered. Timers run only while a chat works.

Full window: the header's expand button (`PanelCore::setWindowToggle`) moves the chat into a window-sized tab
(Binary Ninja: a tab of its own, the Rikugan sidebar closes; IDA: the dock tabbed beside the disassembly) and shows
`ChatManager` at its right: every open database's chats (`databases::all()`), refreshed on `ChatHub::chatsChanged` /
`databasesChanged`. Choosing a chat turns the panel to its database. Binary Ninja registers every tab's database while
the full-window tab is open, and `open_database` opens a file in a new tab (`databases::setTabOpener`,
which waits for Binary Ninja's asynchronous `OnAfterOpenFile`).

## Commands and Modes

| Input | Handled by |
|---|---|
| `/plan`, `/explore`, `/modify`, `/research` | `ModeDriver` (one job on the chat's worker; phases gate tools; decisions via cards) |
| `/undo [N]` | `MutationLog::undo` (reverse operations off the UI thread); a tool row's Undo chip runs it |
| `/memory`, `/mcp`, `/doctor`, `/rename`, `/export` | `CommandRouter` (`/export` is omc's HTML export) |
| `/goal`, `/btw` | omc's goal mode (runs until settled) and side questions |
| `/agents`, `/login`, `/logout`, `/new`, `/fork`, `/todo`, `/model` | the panel (agents panel, login sheet, model picker ...) |
| `/<skill> args` | omc's `/skill:<skill>`; skills with `mode: plan`/`exploration` start that mode |
| `/compact`, `/handoff`, `/retry`, `/effort`, `/fast`, `/advisor`, `/model <id>` | omc |
| any other `/command` | refused locally (omc's `/move` and `/wt` would `chdir` the host process) |

## Settings and Files

- `~/.rikugan/config.json` — `RikuganConfig` (`RIKUGAN_HOME` overrides the root; `~` is `$HOME` when set, else the
  user's home, on Windows `%USERPROFILE%`).
- `~/.rikugan/omc/` — Rikugan's own omc configuration (`OMC_CONFIG_DIR`); `omc/agent/` holds `auth.json`,
  `models.yml` (custom endpoints), `mcp.json`, `skills/`, and a `config.yml` Rikugan rewrites at every start to keep
  omc's host-unfriendly behaviours off (cache warming, sleep prevention, skill-description compression, project MCP).
- "Use my omc logins" (`AuthSync`) copies standalone omc's logins (`~/.omc/agent/auth.json`) into Rikugan's and keeps
  refreshed OAuth tokens in step; it never copies Rikugan-only entries back and never deletes.
- "Use my Claude Code / Codex login" (`ExternalLogins`, config `use_external_logins`, asked once with the providers'
  policy links) shares those tools' subscription logins the same way, writing refreshed tokens back to them.
- Custom connections may set `context_window` / `max_tokens` (written to `models.yml` as `contextWindow` /
  `maxTokens` per model); 0 keeps omc's default.
- `~/.rikugan/sessions/<db id>/` — omc session files, `chats.json` and `database.json` (host and file, so the panel
  can reopen the database's chats while nothing is open); `~/.rikugan/scratch/<db id>/` — the session's working
  directory (artifacts, files the agent writes).
- `~/.rikugan/logs/crash-log-<YYYYMMDD-HHMMSS>-<pid>.log` — crash logs (`util/CrashGuard.h`); a crash in Rikugan
  also saves `<database>.rikugan-recovered.<ext>` beside each open database. New threads call
  `crash::guardThread()` first.
- `<database dir>/RIKUGAN.md` — persistent memory (`save_memory`), first 200 lines in the system prompt.
- Updates (`update/Updater.h`, release packages only): 20 s after the host starts, then hourly, a check is made when a
  day has passed since the last one (`~/.rikugan/update.json`), while `check_for_updates` (Settings › Behavior) is on.
  A newer release is offered in the panel's update line and installed only on the user's Update: the release's
  package for this host and platform, downloaded with the system's curl, checked against its `.sha256`, unpacked with
  tar, required to hold this plugin's file with the release's version marker, then swapped in all-or-nothing
  (Windows renames the loaded DLL aside and deletes it at the next start). `RIKUGAN_UPDATE_URL` points it at another
  releases page.

## How to Add a Tool

Host tools are `rk::ToolSpec`s added in the host's `registerTools()` (`src/binja/tools/`, `src/ida/host/tools/`):

```cpp
registry.add(rk::ToolSpec{
    "jump_to", "navigation", "Jump to the specified address.",
    {rk::ToolParam{"address", rk::ToolParam::String, "Target address (hex string, e.g. '0x401000')"}},
    rk::MainThread, 20000,
    [this](rk::ToolCall& call) {
        const uint64_t ea = call.address("address");
        ...
        return rk::ToolResult::ok(QStringLiteral("Jumped to %1").arg(rk::hex(ea)));
    }});
```

- Flags: `Mutating` (changes the database: undo record, cache flush, `approve_mutations`), `MainThread`,
  `RequiresDecompiler`, `Script` (runs model-written code: always asks first, and that card also covers its database
  changes), `Internal` (never shown to the model), `Exclusive`, `Blocking` (waits on the user or another agent: own
  thread instead of the pool), `External` (another program serves it: Binary Ninja's MCP tools, see below).
- Every `Mutating` tool must be in `MutationLog::reverseBuilders()` (with its pre-state reader in
  `capturePreState`) or in `MutationLog::knownIrreversible()`, or be `External` (recorded as not reversible). A reversible tool's success text starts with one of
  "Renamed", "Named", "Set ".
- Arguments go through `ToolCall` accessors (`str`, `integer`, `boolean`, `address`, `array`), which apply the
  parameter defaults and the coercions the model needs. Report bad input with `throw rk::ToolError(...)` (the model
  sees `Error: ...`) or a returned message.
- Results are text for the model; addresses print as `rk::hex(ea)`.

## Binary Ninja's MCP Server

Binary Ninja runs its own MCP server when `ui.mcp.enabled` is set or it is started from Plugins → MCP
(`http://127.0.0.1:<ui.mcp.port><ui.mcp.endpoint>`, default `:24642/mcp`, optional bearer token `ui.mcp.token`).
Rikugan probes the server itself (an MCP handshake) whatever the setting says. `HostMcp` (`src/common/agent/`) talks
to it through `McpClient` (`src/common/mcp/`, a blocking streamable-HTTP client on plain sockets). The host hooks are in
`src/binja/host/BnHostMcp.cpp`. They cover where the server listens, starting it (`MCP\Start Server`), and
selecting the chat's view, since the server's active view is global. They also mark tools read-only by name
(the server sends no `readOnlyHint`) and hide its file and view switching.

- `HostMcpBar` (`ui/console/`) looks for the server whenever a database shows and when the panel shows again, then
  again after 2, 5, 10 and 30 s and every 60 s until it answers once for that database (not after an explicit no).
  With an earlier yes for that database (`hostMcp` in its `database.json`) the tools turn on. With no answer yet it
  asks. The header's MCP button toggles, and a click counts as consent; a click that finds no server offers to start
  it. Settings → MCP → Address (`binja_mcp_url`) overrides the address read from Binary Ninja's settings.
- Server tool descriptions are clipped (140 characters, 60 per parameter): the server's long manuals would otherwise
  go out with every request.
- On: the server's tools join the database's registry (`category` `host_mcp`, flag `External`, `Mutating` unless
  read-only) and `chatToolVisible` hides every non-`chat` Rikugan tool for that database. The system prompt gets
  `prompts/system/host_mcp.md`. omc cannot change a live session's system prompt, so `ChatSession::hostToolsChanged`
  closes idle sessions (they resume on demand). A busy session gets `set_host_tools` at once and closes when its
  run ends.
- Calls run through `ToolDispatcher` like every host tool, so the profile, phase gate, approval, sanitizing and
  result cap all apply. `HostMcp::call` serializes them and selects the view first. A change they make is
  recorded as not reversible.

## Development Standards

- C++20, Qt Widgets (`QT_NO_KEYWORDS`: `Q_SIGNALS`, `Q_SLOTS`, `Q_EMIT`). Shared code lives in `src/common`; host
  code never leaks into it.
- **Threads.** The host UI thread never waits on omc, a session worker or a tool worker (the only exception is
  plugin shutdown, after `Host::closing` is set). UI updates arrive through `EventQueue` (one coalesced `postEvent`
  per burst) or `QMetaObject::invokeMethod(rk::uiAnchor(), ..., Qt::QueuedConnection)` guarded by `QPointer`. No
  polling timers while idle. omc callbacks never block.
- **Unload safety** (`util/ModuleLifetime.h`). IDA unloads `rikugan.so` / `rikugan.dll` while its event loop keeps
  running, so nothing may run plugin code afterwards. Never `std::thread(...).detach()`: use `rk::spawnThread`, which
  unload joins. Post cross-thread results to `rk::uiAnchor()`, not `qApp`, and parent module-wide timers to it. A
  widget removed with `deleteLater` stays parented (`hide()` it, don't `setParent(nullptr)`).
- **IDA**: every SDK call on the main thread (`IdaHost::runOnMainThread`: `execute_sync` with `MFF_NOWAIT` and
  `MFF_READ`/`MFF_WRITE`). Files that subclass SDK types live in `src/ida/sdk/` (built with `-fno-rtti` by
  GCC/clang). Include Qt and STL headers before `ida/host/IdaSdk.h`. Never let a C++ exception unwind through an IDA
  callback.
- **Binary Ninja**: the core API is thread-safe, so every tool runs on a tool worker, mutating ones and
  `execute_python` included (exclusive, so in order). Only tools that touch the UI (`jump_to`) run on the main
  thread (`ExecuteOnMainThread`): a tool that waits on analysis or a script there freezes Binary Ninja.
- **Windows**: platform code stays behind `#ifdef _WIN32` in `util/Env`, `omc/OmcLibrary`, `mcp/McpClient` (Winsock)
  and `util/Files`. Variables omc reads are set with `rk::setEnv` / `rk::unsetEnv`, never `qputenv`/`setenv` alone
  (libomc.dll's C runtime keeps its own environment copy); the home folder is `rk::homeDir()` (`$HOME` first, as omc
  does). Embedded prompts and skills have their CRLF line ends normalized at build time.
- No stubs, placeholders or `TODO: implement`. Errors are reported, never swallowed.

## Security

Rikugan processes adversarial binaries. Their strings, names and code reach the model and the UI.

- Tool results pass `rk::sanitize::stripInjectionMarkers`, then the active profile's IOC filter: host tools in
  `ToolDispatcher`, omc's own tools (files, shell, web, MCP servers) through `omc_set_result_filter`
  (`SessionListener::filterToolResult`). A profile's denied tools may name omc built-ins too. Saved settings reach
  open chats (`ChatSession::settingsChanged`): idle ones take the new profile at once and reopen on demand, busy
  ones when their run ends. Memory and binary
  context in the system prompt go through `sanitizeMemory` / `sanitizeBinaryContext`. The system prompt's
  data-integrity section tells the model that tool results are data, not instructions.
- `execute_python` and the other `Script` tools always ask the user first. Code runs in the host's own Python after
  the guard prelude (`prompts/python/guard_prelude.py`: the AST blocklist and restricted builtins of the former
  `script_guard`).
- Filesystem and shell tools are omc's. Each kind (read, write, edit, shell) asks the first time in a chat
  (`Permissions`: Allow once / Allow for this chat / Deny; a chat's subagents share its answers); Settings can
  allow files or shell commands without asking. They are not sandboxed: once allowed, prompt injection from a
  sample can reach a shell.
- The omc session's working directory is Rikugan's scratch directory, never the sample's directory, and project
  context files, rules and project MCP are off, so a sample tree cannot inject settings or start processes.

## Tests

`ctest --test-dir build/none` runs the QtTest suite against omc's mock model (`../ohmyc/tests/e2e/mock_openai.py`)
in a temporary home: the omc bridge, the chat core with a fake host (host tools, exclusivity, cancellation, undo,
artifacts, transcript replay, omc's chat titles), permissions, commands, logins, sanitization, modes, the bulk
renamer, settings, the theme, the transcript reducer, the updater (against a local release server) and the UI
(offscreen: cards, permissions, chat switcher).
`build/ida/rikugan_ida_tooltest` runs every IDA tool on a real database through idalib (`/bin/ls`; Windows
`notepad.exe` with IDA's folder on `PATH`).

## Commit Practices

- Prefix: `fix(scope)`, `feat(scope)`, `refactor(scope)`, `security`, `docs`. Scopes: `ida`, `binja`, `agent`, `ui`,
  `omc`, `tools`, `build`.
- One logical change per commit. Run `./ci-local.sh` (Windows: `.\ci-local.ps1`) before opening a PR.
