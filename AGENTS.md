# AGENTS.md — Rikugan Developer Guide

## Project Overview

Rikugan (六眼) is a multi-host reverse-engineering agent plugin that integrates an LLM-powered assistant directly inside **IDA Pro** and **Binary Ninja**. It has its own agentic loop, in-process tool orchestration, streaming UI, multi-tab chat, session persistence, MCP client support, and host-native tool sets.

## Directory Structure

```
rikugan/
├── agent/                    # Agent loop & prompt logic (host-agnostic)
│   ├── loop.py               # AgentLoop: generator-based turn cycle
│   ├── turn.py               # TurnEvent / TurnEventType definitions
│   ├── context_window.py     # Context-window management (threshold compaction)
│   ├── exploration_mode.py   # Exploration state machine (4 phases)
│   ├── mutation.py           # MutationRecord, build_reverse_record, capture_pre_state
│   ├── plan_mode.py          # Plan-mode step orchestration
│   ├── subagent.py           # SubagentRunner — isolated AgentLoop for tasks
│   ├── system_prompt.py      # build_system_prompt() dispatcher
│   └── prompts/              # Host-specific system prompts
│       ├── base.py           # Shared prompt sections (discipline, renaming, etc.)
│       ├── ida.py            # IDA Pro base prompt
│       └── binja.py          # Binary Ninja base prompt
│
├── core/                     # Shared infrastructure (host-agnostic)
│   ├── config.py             # RikuganConfig — settings, provider config, paths
│   ├── constants.py          # Constants (CONFIG_DIR_NAME, etc.)
│   ├── errors.py             # Exception hierarchy (ToolError, AgentError, etc.)
│   ├── host.py               # Host context (BV, address, navigate callback)
│   ├── logging.py            # Logging utilities
│   ├── thread_safety.py      # Thread-safety helpers (@idasync, etc.)
│   └── types.py              # Core data types (Message, ToolCall, StreamChunk, etc.)
│
├── ida/                      # IDA Pro host package
│   ├── tools/
│   │   └── registry.py       # IDA create_default_registry() — imports rikugan.tools.*
│   └── ui/
│       ├── panel.py          # IDA PluginForm wrapper
│       ├── actions.py        # IDA UI hooks & context menu actions
│       └── session_controller.py  # IDA SessionController
│
├── binja/                    # Binary Ninja host package
│   ├── tools/
│   │   ├── registry.py       # BN create_default_registry() — imports rikugan.binja.tools.*
│   │   ├── common.py         # BN shared helpers (get_bv, get_function_at, etc.)
│   │   ├── navigation.py     # Navigation tools
│   │   ├── functions.py      # Function listing/search tools
│   │   ├── strings.py        # String tools
│   │   ├── database.py       # Segments, imports, exports, binary info
│   │   ├── disassembly.py    # Disassembly tools
│   │   ├── decompiler.py     # Decompiler/HLIL tools
│   │   ├── xrefs.py          # Cross-reference tools
│   │   ├── annotations.py    # Rename/comment/set_type tools
│   │   ├── types_tools.py    # Struct/enum/typedef tools
│   │   ├── il.py             # IL core tools (get_il, get_il_block, nop_instructions, redecompile_function)
│   │   ├── il_analysis.py    # IL analysis tools (get_cfg, track_variable_ssa)
│   │   ├── il_transform.py   # IL transform tools (il_replace_expr, il_set_condition, il_nop_expr, patch_branch, etc.)
│   │   └── scripting.py      # execute_python tool
│   └── ui/
│       ├── panel.py          # BN QWidget panel
│       ├── actions.py        # BN action handlers
│       └── session_controller.py  # BN BinaryNinjaSessionController
│
├── tools/                    # IDA tool implementations
│   ├── base.py               # @tool decorator, ToolDefinition, JSON schema generation
│   ├── registry.py           # Shared ToolRegistry class
│   ├── navigation.py         # IDA navigation tools
│   ├── functions.py          # IDA function tools
│   ├── strings.py            # IDA string tools
│   ├── database.py           # IDA database tools (segments, imports, exports)
│   ├── disassembly.py        # IDA disassembly tools
│   ├── decompiler.py         # IDA decompiler tools (Hex-Rays)
│   ├── xrefs.py              # IDA xref tools
│   ├── annotations.py        # IDA annotation tools (rename, comment, set type)
│   ├── types_tools.py        # IDA type tools (structs, enums, typedefs, TILs)
│   ├── microcode.py          # IDA Hex-Rays microcode tools
│   ├── microcode_format.py   # Microcode formatting helpers
│   ├── microcode_optim.py    # Microcode optimizer framework
│   └── scripting.py          # IDA execute_python tool
│
├── hosts/                    # Backward-compat shims → rikugan.ida.ui.* / rikugan.binja.ui.*
│
├── providers/                # LLM provider integrations (host-agnostic)
│   ├── base.py               # LLMProvider ABC
│   ├── registry.py           # ProviderRegistry
│   ├── anthropic_provider.py # Claude (Anthropic) — supports OAuth auto-detection
│   ├── openai_provider.py    # OpenAI
│   ├── gemini_provider.py    # Google Gemini
│   ├── ollama_provider.py    # Ollama (local)
│   ├── minimax_provider.py   # MiniMax (subclasses OpenAICompatProvider)
│   └── openai_compat.py      # OpenAI-compatible endpoints
│
├── mcp/                      # MCP client (host-agnostic)
│   ├── config.py             # MCP server config loader
│   ├── client.py             # MCP protocol client
│   ├── bridge.py             # MCP ↔ ToolRegistry bridge
│   ├── manager.py            # MCPManager — lifecycle management
│   └── protocol.py           # MCP JSON-RPC protocol types
│
├── skills/                   # Skill system (host-agnostic)
│   ├── registry.py           # SkillRegistry — discovery & loading
│   ├── loader.py             # SKILL.md frontmatter parser (mode field support)
│   └── builtins/             # 12 built-in skills
│       ├── malware-analysis/
│       ├── linux-malware/
│       ├── deobfuscation/
│       ├── vuln-audit/
│       ├── driver-analysis/
│       ├── ctf/
│       ├── generic-re/
│       ├── ida-scripting/    # IDAPython API skill with full reference
│       ├── binja-scripting/  # Binary Ninja Python API skill with full reference
│       ├── modify/           # Exploration mode: autonomous binary modification
│       ├── smart-patch-ida/  # IDA-specific binary patching workflow
│       └── smart-patch-binja/ # Binary Ninja-specific patching workflow
│
├── state/                    # Session persistence (host-agnostic)
│   ├── session.py            # SessionState — message history, token tracking
│   └── history.py            # SessionHistory — auto-save/restore per file
│
└── ui/                       # Shared UI widgets (Qt, host-agnostic)
    ├── panel_core.py         # PanelCore — multi-tab chat, export, mutation log, event routing
    ├── session_controller_base.py  # SessionControllerBase — multi-session, fork support
    ├── chat_view.py          # Chat message display widget (queued message support)
    ├── input_area.py         # User input text area with skill autocomplete
    ├── context_bar.py        # Binary context status bar
    ├── message_widgets.py    # Message bubble widgets (tool calls, exploration, approval)
    ├── mutation_log_view.py  # MutationLogPanel — mutation history with undo
    ├── markdown.py           # Markdown rendering for assistant messages
    ├── plan_view.py          # Plan-mode UI
    ├── settings_dialog.py    # Settings dialog (screen-aware sizing)
    ├── styles.py             # Qt stylesheet constants
    └── qt_compat.py          # Qt compatibility layer (PySide6)
```

Entry points (root directory):
- **IDA Pro**: `rikugan_plugin.py` — `PLUGIN_ENTRY()` → `RikuganPlugin` → `RikuganPlugmod`
- **Binary Ninja**: `rikugan_binaryninja.py` — registers sidebar widget + commands at import time

## How the Agent Loop Works

The agent uses a **generator-based turn cycle** (`rikugan/agent/loop.py`):

```
User message → command detection → skill resolution → build system prompt
    → stream LLM response → intercept tool calls → execute tools → feed results back → repeat
```

1. **User sends a message** — the UI calls `SessionControllerBase.start_agent(user_message)`
2. **Command detection** — `/plan`, `/modify`, `/explore`, `/memory`, `/undo`, `/mcp`, `/doctor` are handled as special commands
3. **Skill resolution** — `/slug` prefixes are matched to skills; the skill body is injected into the prompt
4. **System prompt is built** — `build_system_prompt()` selects the host-specific base prompt and appends binary context, current position, available tools, active skills, and persistent memory (RIKUGAN.md)
5. **AgentLoop.run()** is a generator that yields `TurnEvent` objects to the UI:
   - `TEXT_DELTA` / `TEXT_DONE` — streaming/complete assistant text
   - `TOOL_CALL_START` / `TOOL_CALL_DONE` — LLM requested a tool call
   - `TOOL_RESULT` — tool execution result
   - `TURN_START` / `TURN_END` — turn boundaries
   - `EXPLORATION_*` — exploration mode events (phase changes, findings)
   - `MUTATION_RECORDED` — mutation tracked for undo
   - `ERROR` / `CANCELLED` — error or user cancellation
6. **Tool calls** are intercepted from the LLM stream, dispatched via `ToolRegistry.execute()` (with per-tool timeout), and the results are appended to the conversation
7. **Pseudo-tools** (`exploration_report`, `phase_transition`, `save_memory`, `spawn_subagent`) are handled inline
8. **Mutating tools** have their pre-state captured and reverse operations recorded for `/undo`
9. **Context compaction** kicks in when token usage exceeds 80% of the window
10. **The loop repeats** until the LLM produces a response with no tool calls, or the user cancels
11. **BackgroundAgentRunner** wraps the generator in a background thread; IDA API calls are marshalled to the main thread via `@idasync`

### Modes

| Mode | Trigger | Behavior |
|------|---------|----------|
| **Normal** | Any message | Standard stream → tool → repeat loop |
| **Plan** | `/plan <msg>` | Generate plan → user approves → execute steps (reject → regenerate or cancel) |
| **Exploration** | `/modify <msg>` | 4-phase: EXPLORE (subagent) → PLAN → EXECUTE → SAVE (reject → regenerate or cancel) |
| **Explore-only** | `/explore <msg>` | Autonomous read-only investigation, no patching |

See [ARCHITECTURE.md](ARCHITECTURE.md) for full technical details on all modes, subagents, mutation tracking, and internal data flows.

## Multi-Tab Chat & Session Persistence

- Each tab is an independent `SessionState` with its own message history and token tracking
- `SessionControllerBase` manages a dict of `_sessions: Dict[str, SessionState]` keyed by tab ID
- `PanelCore` uses a `QTabWidget` with closable tabs and a "+" button for new tabs
- **Session fork**: right-click a tab → "Fork Session" to deep copy the conversation into a new tab (branch from a checkpoint)
- Sessions are auto-saved per file (IDB/BNDB path) and restored when re-opening the same file
- Opening a different file resets all tabs and attempts to restore that file's saved sessions

## Approval Gates

### Plan & Save Approval (Button-Only)

When the agent enters plan mode (`/plan`, `/modify`) or requests save approval, the UI
enters a **button-only approval state**:
- Text input is **disabled** — the user MUST click the **Approve/Reject** buttons
- Free-text messages ("continue", "redo", etc.) are silently ignored while awaiting approval
- This prevents accidental plan execution if the agent crashes and the user types into the chat
- The input is re-enabled when: a button is clicked, the agent finishes, the user cancels, or an error occurs
- Any `USER_QUESTION` with predefined options also enforces button-only mode

### Script Approval

The `execute_python` tool always requires explicit user approval before execution:
- The agent proposes Python code → a syntax-highlighted preview is shown in the chat
- The user clicks **Allow** or **Deny**
- Blocked patterns (subprocess, os.system, etc.) are rejected before reaching the approval step

### Prompt Injection Mitigation

Rikugan analyzes untrusted binaries whose content (strings, function names, decompiled code, comments) flows into LLM prompts. A malicious binary could embed adversarial text to manipulate the agent. Mitigations are implemented in `rikugan/core/sanitize.py`:

| Layer | What it does | Where applied |
|-------|-------------|---------------|
| **Delimiter quoting** | Wraps untrusted content in XML-like tags (`<tool_result>`, `<binary_info>`, `<mcp_result>`, `<persistent_memory>`, `<skill>`) | All tool results, system prompt context, MCP results, memory, skills |
| **Injection marker stripping** | Removes sequences mimicking LLM role markers (`[SYSTEM]`, `<\|im_start\|>`, etc.) and instruction override patterns | All untrusted data at point of entry |
| **Length capping** | Truncates data items to configurable limits | Tool results (50K), MCP results (30K), binary data (2K per item), memory (20K), skills (50K) |
| **Model awareness** | `DATA_INTEGRITY_SECTION` in the system prompt instructs the model to treat delimited content as data, not instructions | Both IDA and Binary Ninja base prompts |
| **Memory write sanitization** | `save_memory` tool strips injection markers before writing to RIKUGAN.md | `_handle_save_memory_tool` in loop.py |
| **Compaction sanitization** | Context window compaction strips markers from summary snippets | `context_window.py` |

**Key files:**
- `rikugan/core/sanitize.py` — all sanitization functions
- `rikugan/agent/prompts/base.py` — `DATA_INTEGRITY_SECTION`
- Integration points: `loop.py` (tool results, skills, memory), `system_prompt.py` (binary context), `mcp/client.py` (external results)

## Message Queuing

Users can send follow-up messages while the agent is working. Queued messages appear as `[queued]` in the chat and auto-submit when the current turn finishes. Cancelling discards all queued messages.

## How to Add New Tools

### 1. Create a tool function with the `@tool` decorator

```python
from typing import Annotated
from rikugan.tools.base import tool

@tool(category="navigation")
def jump_to(
    address: Annotated[str, "Target address (hex string, e.g. '0x401000')"],
) -> str:
    """Jump to the specified address."""
    ea = parse_addr(address)
    # ...
    return f"Jumped to 0x{ea:x}"
```

The `@tool` decorator:
- Generates a `ToolDefinition` with JSON schema from the function signature
- Uses `typing.Annotated` metadata for parameter descriptions
- Wraps the handler with `@idasync` for thread-safe IDA API access
- Attaches the definition as `func._tool_definition`

Optional `@tool` parameters:
- `category` — grouping (e.g., `"navigation"`, `"decompiler"`, `"il"`)
- `requires_decompiler` — marks the tool as needing decompiler/Hex-Rays availability
- `mutating` — marks the tool as modifying the database (used for `execute_python` approval)

### 2. Register in the host's registry

**For IDA** — add the module import to `rikugan/ida/tools/registry.py`:
```python
from rikugan.tools import my_new_module
_TOOL_MODULES = (..., my_new_module)
```

**For Binary Ninja** — add the module import to `rikugan/binja/tools/registry.py`:
```python
from rikugan.binja.tools import my_new_module
_TOOL_MODULES = (..., my_new_module)
```

The registry calls `register_module()` on each module, which discovers all `@tool`-decorated functions.

## How to Add a New Host

1. Create `rikugan/<host>/` with `tools/` and `ui/` sub-packages
2. Implement tool modules under `rikugan/<host>/tools/` — use `from rikugan.tools.base import tool`
3. Create `rikugan/<host>/tools/registry.py` with a `create_default_registry()` factory
4. Subclass `SessionControllerBase` in `rikugan/<host>/ui/session_controller.py`
5. Create a panel widget in `rikugan/<host>/ui/panel.py` — embed the shared `PanelCore` widget
6. Add a host-specific prompt in `rikugan/agent/prompts/<host>.py` and register it in `system_prompt.py`'s `_HOST_PROMPTS` dict
7. Create an entry point script (e.g., `rikugan_<host>.py`) that bootstraps the plugin

## How to Add a New Skill

Skills are Markdown files with YAML frontmatter:

```
rikugan/skills/builtins/<slug>/
  SKILL.md            # Required — frontmatter + prompt body
  references/         # Optional — .md files auto-appended to prompt
    api-notes.md
```

Skill format:
```markdown
---
name: My Skill
description: What it does in one line
tags: [analysis, custom]
allowed_tools: [decompile_function, rename_function]
---
Task: <instruction for the agent>
```

Users can also create custom skills in their host config directory (`~/.idapro/rikugan/skills/` or `~/.binaryninja/rikugan/skills/`).

## Import Conventions

- **Cross-package imports** use absolute paths: `from rikugan.tools.base import tool`
- **Within the same package** use absolute imports: `from rikugan.binja.tools.common import get_bv`
- **IDA tool modules** (`rikugan/tools/*.py`) use relative imports within `rikugan.tools`
- **Host API modules** (ida_*, binaryninja) are imported via `importlib.import_module()` inside `try/except ImportError` blocks to avoid crashes when loaded in the wrong host
- **Backward-compat shims** in `rikugan/tools_bn/` and `rikugan/hosts/` re-export from canonical locations

## System Prompt Structure

System prompts are built from **shared sections** + **host-specific content**:

```
rikugan/agent/prompts/
├── base.py     # Shared sections:
│               #   DISCIPLINE_SECTION  — "Do exactly what was asked"
│               #   RENAMING_SECTION    — Renaming/retyping guidelines
│               #   ANALYSIS_SECTION    — Analysis approach
│               #   SAFETY_SECTION      — Safety guidelines
│               #   TOKEN_EFFICIENCY_SECTION — Prefer search over listing
│               #   CLOSING_SECTION     — Final reminders
├── ida.py      # IDA_BASE_PROMPT: IDA intro + IDA tool usage + shared sections
└── binja.py    # BINJA_BASE_PROMPT: BN intro + BN tool usage + shared sections
```

`build_system_prompt()` in `system_prompt.py` selects the correct base prompt by host name, then appends runtime context (binary info, cursor position, tool list, active skills).

## Key Files

| File | Role |
|------|------|
| `rikugan/agent/loop.py` | Core agent loop — generator-based turn cycle |
| `rikugan/tools/base.py` | `@tool` decorator, `ToolDefinition`, JSON schema generation |
| `rikugan/tools/registry.py` | `ToolRegistry` — registration, dispatch, argument coercion |
| `rikugan/ui/session_controller_base.py` | `SessionControllerBase` — multi-session orchestration |
| `rikugan/ui/panel_core.py` | `PanelCore` — multi-tab chat, export, event routing |
| `rikugan/ui/chat_view.py` | `ChatView` — message display, queued messages |
| `rikugan/ui/message_widgets.py` | Message widgets including approval dialog |
| `rikugan/core/config.py` | `RikuganConfig` — all settings, provider config, host paths |
| `rikugan/core/host.py` | Host context singleton (BinaryView, address, navigate callback) |
| `rikugan/core/thread_safety.py` | `@idasync` decorator for main-thread marshalling |
| `rikugan/providers/base.py` | `LLMProvider` ABC — interface for all LLM providers |
| `rikugan/mcp/manager.py` | `MCPManager` — starts MCP servers, bridges tools into registry |
| `rikugan/skills/registry.py` | `SkillRegistry` — discovers and loads SKILL.md files |
| `rikugan/state/session.py` | `SessionState` — message history, token usage tracking |
| `rikugan/state/history.py` | `SessionHistory` — auto-save/restore per file |
| `rikugan_plugin.py` | IDA Pro plugin entry point |
| `rikugan_binaryninja.py` | Binary Ninja plugin entry point |

## IDA API Notes

IDA tool modules use `importlib.import_module()` for all `ida_*` imports to avoid Shiboken UAF crashes. Key considerations:

- **IDA 9.x** removed `ida_struct` and `ida_enum` — use `ida_typeinf` with `tinfo_t.add_udm()`/`udm_t`/`edm_t`/`iter_struct()`/`iter_enum()`. Note: `idc` still has enum wrapper functions (`add_enum`, `get_enum`, etc.)
- **IDA 9.x** `ida_bytes` has both `get_byte()` and `get_wide_byte()`; `idc` only has `get_wide_byte`
- **IDA 9.x** `modify_user_lvar_info(ea, MLI_TYPE, lsi)` is the preferred way to retype local variables (persists to DB); `lvar_t.set_lvar_type()` is in-memory only
- **Segment permissions** use raw bit flags on `seg.perm` (4=R, 2=W, 1=X), not named constants
- **`idautils.Entries()`** yields 4 values: `(index, ordinal, ea, name)`
- **`ida_hexrays.decompile()`** can raise `DecompilationFailure` — always wrap in try/except
- All IDA API calls must run on the main thread — the `@idasync` wrapper handles this automatically

### Python Version Warning (IDA Pro)

IDA Pro's Qt/PySide6 binding (Shiboken) has a known Use-After-Free bug triggered when Python > 3.10 imports C-extension modules during Qt signal dispatch. Rikugan mitigates this by:

1. Routing all `ida_*` imports through `importlib.import_module()` to bypass Shiboken's `__import__` hook
2. Installing a re-entrancy guard on `builtins.__import__` to prevent nested imports during signal dispatch

**Python 3.10 is the safest choice for IDA Pro.** Higher versions may still work with the mitigations in place, but can exhibit instability. See [upstream report](https://community.hex-rays.com/t/ida-9-3-b1-macos-arm64-uaf-crash/646).

### IDA 9.x Type API Changes

The following IDA 9.x API changes are handled by the codebase:

| Module Change | Migration |
|--------------|-----------|
| `ida_struct` removed | All struct ops use `ida_typeinf` UDT API (`tinfo_t.create_udt()`, `add_udm()`, `find_udm()`, etc.) |
| `ida_enum` removed | Enum tools use `idc` wrappers (still present in 9.x) + `ida_typeinf` native API (`edm_t`, `iter_enum()`) |
| UDT offsets are in **bits** | All offset parameters multiply by 8 before passing to `udm_t` / `add_udm()` |
| `lvar_t.set_user_type()` takes **no args** | Just sets the user-defined flag, doesn't set a type |
| `apply_type_to_variable` | Uses `modify_user_lvar_info(ea, MLI_TYPE, lsi)` (persistent) with callback fallback |
| `tinfo_t.parse(decl)` | Convenience method, `til` defaults to `None` (valid — uses default IDB TIL) |
| `tinfo_t.add_udm(name, type_str, offset_bits)` | Accepts string types directly in IDA 9.x |
| `tinfo_t.iter_struct()` / `iter_enum()` | Generator-based iteration (preferred over `get_udt_details`) |
