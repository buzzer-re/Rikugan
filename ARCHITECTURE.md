# ARCHITECTURE.md — Rikugan Agent Internals

This document describes the internal architecture of the Rikugan agent in full technical detail. It is intended for engineers who need to understand, modify, or extend the system.

---

## Table of Contents

1. [High-Level Overview](#high-level-overview)
2. [The Agentic Loop](#the-agentic-loop)
3. [TurnEvent System](#turnevent-system)
4. [Tool Framework](#tool-framework)
5. [Pseudo-Tools](#pseudo-tools)
6. [Skill System](#skill-system)
7. [Exploration Mode](#exploration-mode)
8. [Plan Mode](#plan-mode)
9. [Subagents](#subagents)
10. [Mutation Tracking and Undo](#mutation-tracking-and-undo) (see also [Files and Shell](#files-and-shell))
11. [Context Window Management](#context-window-management)
12. [Persistent Memory](#persistent-memory)
13. [Session Management](#session-management)
14. [MCP Integration](#mcp-integration)
15. [Provider Layer](#provider-layer)
16. [System Prompt Architecture](#system-prompt-architecture)
17. [UI Layer](#ui-layer)
18. [Thread Safety Model](#thread-safety-model)
19. [Error Handling and Retry](#error-handling-and-retry)
20. [Logging](#logging)
21. [Commands Reference](#commands-reference)
22. [Data Flow Diagrams](#data-flow-diagrams)

---

## High-Level Overview

Rikugan is a **generator-based agentic loop** embedded inside IDA Pro and Binary Ninja. The agent runs in a background thread and communicates with the Qt UI via a stream of `TurnEvent` objects. All host API calls (IDA/BN) are marshalled to the main thread via `@idasync`.

```
User Input
    │
    ▼
┌──────────────────────────────────────────┐
│            SessionControllerBase          │
│  (creates AgentLoop, manages tabs/state) │
└───────────────┬──────────────────────────┘
                │
                ▼
┌──────────────────────────────────────────┐
│  BackgroundAgentRunner (threading.Thread) │
│  ┌────────────────────────────────────┐  │
│  │          AgentLoop.run()           │  │
│  │  ┌──────────────────────────────┐  │  │
│  │  │ while has_tool_calls:        │  │  │
│  │  │   stream LLM response       │  │  │
│  │  │   parse tool calls          │  │  │
│  │  │   execute tools             │  │  │
│  │  │   feed results back         │  │  │
│  │  │   yield TurnEvents ─────────┼──┼──┼──→ Queue → UI poll
│  │  └──────────────────────────────┘  │  │
│  └────────────────────────────────────┘  │
└──────────────────────────────────────────┘
```

Key files:
- `rikugan/agent/loop.py` — `AgentLoop` + `BackgroundAgentRunner`
- `rikugan/agent/turn.py` — `TurnEvent` / `TurnEventType`
- `rikugan/tools/base.py` — `@tool` decorator, `ToolDefinition`
- `rikugan/tools/registry.py` — `ToolRegistry`
- `rikugan/ui/panel_core.py` — `RikuganPanelCore` (Qt UI)
- `rikugan/ui/session_controller_base.py` — `SessionControllerBase`

---

## The Agentic Loop

**File**: `rikugan/agent/loop.py`

### `AgentLoop.run(user_message) -> Generator[TurnEvent]`

The entry point is `AgentLoop.run()`, a Python generator. It yields `TurnEvent` objects that the UI consumes. The loop proceeds as follows:

1. **Command Detection** — The message is checked for command prefixes:
   - `/plan <msg>` → plan mode
   - `/modify <msg>` → exploration mode (4-phase, with patching)
   - `/explore <msg>` → exploration mode (explore-only, read-only)
   - `/memory` → show RIKUGAN.md contents
   - `/undo [N]` → undo last N mutations
   - `/mcp` → show MCP server health
   - `/doctor` → diagnose setup issues

2. **Skill Resolution** — `_resolve_skill()` checks if the message starts with a skill slug (e.g., `/malware-analysis`). If matched, the skill's body is prepended to the system prompt and `allowed_tools` is enforced.

3. **System Prompt Build** — `build_system_prompt()` assembles the prompt from host-specific base + binary context + cursor position + tool list + skill + persistent memory (RIKUGAN.md).

4. **Turn Loop** — The core loop:
   ```
   while True:
       yield TURN_START
       stream = provider.chat_stream(messages, tools, system)
       text, tool_calls = _stream_llm_turn(stream)  # yields TEXT_DELTA events
       yield TEXT_DONE
       if no tool_calls:
           break  # LLM is done
       results = _execute_tool_calls(tool_calls)  # yields TOOL_RESULT events
       append results to messages
       yield TURN_END
   ```

5. **Context Compaction** — At the start of each turn, `ContextWindowManager.should_compact()` is checked. If the context is above 80% of the window, messages are compacted (old middle messages summarized, head + tail preserved).

6. **Cancellation** — `_check_cancelled()` is called at multiple points. It raises `CancellationError` which is caught at the top level and yields a `CANCELLED` event.

### `_stream_llm_turn()`

Consumes the provider's `chat_stream()` generator. Each `StreamChunk` is processed:

- `chunk.text` → yields `TEXT_DELTA` events, accumulates full text
- `chunk.is_tool_call_start` → yields `TOOL_CALL_START`, starts accumulating tool call
- `chunk.tool_args_delta` → yields `TOOL_CALL_ARGS_DELTA`, accumulates JSON args
- `chunk.is_tool_call_end` → yields `TOOL_CALL_DONE`, finalizes the `ToolCall`
- `chunk.usage` → yields `USAGE_UPDATE`, updates context manager

Returns `(full_text, List[ToolCall])`.

### `_execute_tool_calls()`

Takes a list of `ToolCall` objects and executes each one. For each call:

1. **Pseudo-tool check** — Handled inline with `continue` (see [Pseudo-Tools](#pseudo-tools))
2. **Approval gate** — `execute_python` requires user approval unless previously allowed
3. **Pre-state capture** — For mutating tools, `capture_pre_state()` calls getter tools to record the current state before mutation
4. **Execution** — `ToolRegistry.execute(name, args)` dispatches to the handler
5. **Mutation recording** — If `defn.mutating`, `build_reverse_record()` creates a `MutationRecord` and appends to `session.mutation_log`. File tools append their own record. Either way a `MUTATION_RECORDED` event is yielded
6. **Error handling** — `ToolError` and general `Exception` are caught and returned as error results
7. **Result** — Each result becomes a `ToolResult` and yields a `TOOL_RESULT` event

### `BackgroundAgentRunner`

Wraps the generator in a `threading.Thread`. Events are forwarded to a `queue.Queue` that the UI polls at 50ms intervals via `QTimer`.

```python
class BackgroundAgentRunner:
    def start(self, user_message):
        self._thread = Thread(target=self._run, args=(user_message,))
        self._thread.start()

    def _run(self, message):
        for event in self.agent_loop.run(message):
            self._event_queue.put(event)

    def get_event(self, timeout=0):
        return self._event_queue.get(timeout=timeout)
```

---

## TurnEvent System

**File**: `rikugan/agent/turn.py`

All communication from the agent loop to the UI goes through `TurnEvent` objects. Each event has a `type` (enum) and optional payload fields.

### Event Types

| Type | Description | Key Fields |
|------|-------------|------------|
| `TEXT_DELTA` | Streaming text token | `text` |
| `TEXT_DONE` | Full assistant text complete | `text` |
| `TOOL_CALL_START` | LLM requested a tool call | `tool_call_id`, `tool_name` |
| `TOOL_CALL_ARGS_DELTA` | Streaming tool arguments | `tool_call_id`, `tool_args` |
| `TOOL_CALL_DONE` | Tool call arguments finalized | `tool_call_id`, `tool_name`, `tool_args` |
| `TOOL_RESULT` | Tool execution result | `tool_call_id`, `tool_name`, `tool_result`, `tool_is_error` |
| `TURN_START` | New turn begins | `turn_number` |
| `TURN_END` | Turn complete | `turn_number` |
| `ERROR` | Error occurred | `error` |
| `CANCELLED` | User cancelled | — |
| `USAGE_UPDATE` | Token usage update | `usage` (TokenUsage) |
| `USER_QUESTION` | Agent asks user a question | `text`, `metadata.options` |
| `PLAN_GENERATED` | Plan mode: plan ready | `plan_steps` |
| `PLAN_STEP_START` | Plan mode: executing step | `plan_step_index`, `text` |
| `PLAN_STEP_DONE` | Plan mode: step complete | `plan_step_index`, `text` |
| `TOOL_APPROVAL_REQUEST` | Script approval needed | `tool_call_id`, `tool_name`, `tool_args`, `text` |
| `EXPLORATION_PHASE_CHANGE` | Phase transition | `metadata.from_phase`, `metadata.to_phase` |
| `EXPLORATION_FINDING` | Discovery logged | `text`, `metadata.category`, `metadata.address`, `metadata.relevance` |
| `PATCH_APPLIED` | Binary patch applied | `text`, `metadata.address`, `metadata.original`, `metadata.new` |
| `PATCH_VERIFIED` | Patch verified | `text`, `metadata.address`, `metadata.success` |
| `SAVE_APPROVAL_REQUEST` | Save gate reached | `text`, `metadata.patch_count`, `metadata.total_bytes` |
| `SAVE_COMPLETED` | Patches saved to file | `text`, `metadata.patch_count` |
| `SAVE_DISCARDED` | Patches discarded | `text`, `metadata.rolled_back` |
| `MUTATION_RECORDED` | Mutation logged for undo | `tool_name`, `text`, `metadata.reversible`, `metadata.reverse_tool` |
| `MUTATION_UNDONE` | A record left the mutation log | `text` |

Each event type has a static factory method on `TurnEvent` for clean construction (e.g., `TurnEvent.text_delta("hello")`).

---

## Tool Framework

**Files**: `rikugan/tools/base.py`, `rikugan/tools/registry.py`

### `@tool` Decorator

Tools are defined with the `@tool` decorator on plain functions:

```python
@tool(category="annotations", mutating=True)
def rename_function(
    old_name: Annotated[str, "Current function name"],
    new_name: Annotated[str, "New name to assign"],
) -> str:
    """Rename a function in the database."""
    # ... implementation
```

The decorator:
1. Inspects the function signature using `typing.get_type_hints()`
2. Extracts parameter descriptions from `Annotated` metadata
3. Generates a `ToolDefinition` with JSON schema
4. Wraps the handler with `@idasync` for IDA thread-safety
5. Attaches the definition as `func._tool_definition`

### `ToolDefinition`

```python
@dataclass
class ToolDefinition:
    name: str
    description: str
    parameters: List[ParameterSchema]
    category: str = "general"
    requires_decompiler: bool = False
    mutating: bool = False              # marks tool as modifying the database
    timeout: Optional[float] = None     # per-tool timeout in seconds
    handler: Optional[Callable] = None
    internal: bool = False              # hidden from the model; only Rikugan calls it
    result_limit: Optional[int] = None  # characters allowed, if it bounds its own output
    long_running: bool = False          # runs on its own executor
```

Key flags:
- `mutating=True` — Triggers pre-state capture and mutation recording for undo
- `requires_decompiler=True` — Tool is excluded if decompiler is unavailable
- `timeout` — Per-tool timeout; wrapped in `ThreadPoolExecutor` during execution

### `ToolRegistry`

Central registry for all tool definitions. Core methods:
- `register(defn)` / `register_module(module)` — Registration
- `execute(name, args)` — Dispatches to handler with argument coercion and timeout
- `get(name)` → `ToolDefinition` — Lookup
- `to_provider_format()` → list of JSON schemas for LLM

Argument coercion in `execute()`:
- Hex strings (`"0x401000"`) → `int` for integer parameters
- `"true"`/`"false"` strings → `bool`
- `0`/`1` integers → `bool`

Timeout wrapping:
```python
context = contextvars.copy_context()   # carries the active chat onto the worker
executor = _long_running_executor if defn.long_running else _executor
future = executor.submit(lambda: context.run(handler, **arguments))
result = future.result(timeout=timeout)  # default 30s
```

### Tool Categories

Each host provides ~56 tools organized by category:

| Category | Examples |
|----------|----------|
| Navigation | `get_cursor_position`, `jump_to`, `get_name_at` |
| Functions | `list_functions`, `search_functions`, `get_function_info` |
| Strings | `list_strings`, `search_strings` |
| Database | `list_segments`, `list_imports`, `list_exports`, `read_bytes` |
| Disassembly | `read_disassembly`, `read_function_disassembly` |
| Decompiler | `decompile_function`, `get_pseudocode` |
| Xrefs | `xrefs_to`, `xrefs_from`, `function_xrefs` |
| Annotations | `rename_function`, `set_comment`, `set_type` |
| Types | `create_struct`, `modify_struct`, `set_function_prototype` |
| Scripting | `execute_python` (requires approval) |
| Microcode (IDA) | `get_microcode`, `nop_microcode` |
| IL (BN) | `get_il`, `get_il_block`, `nop_instructions`, `redecompile_function` |
| IL Analysis (BN) | `get_cfg`, `track_variable_ssa` |
| IL Transform (BN) | `il_replace_expr`, `il_set_condition`, `il_nop_expr`, `il_remove_block`, `patch_branch`, `write_bytes`, `install_il_workflow` |
| Filesystem | `read_file`, `write_file`, `edit_file`, `search_files`, `run_shell` (see [Files and Shell](#files-and-shell)) |

---

## Pseudo-Tools

Pseudo-tools are tool schemas injected into the LLM's tool list but handled directly in `_execute_tool_calls()` rather than dispatched through the registry. They use a `continue` statement to skip normal execution.

### `exploration_report`

Used during exploration mode to log structured findings:

```json
{
  "category": "function_purpose|hypothesis|data_structure|constant|string_ref|import_usage|patch_result",
  "summary": "Description of the finding",
  "address": 4198400,
  "function_name": "main",
  "relevance": "high|medium|low",
  "original_hex": "74 05",   // for patch_result only
  "new_hex": "75 05"         // for patch_result only
}
```

When `category="patch_result"`, the handler also creates a `PatchRecord` and appends it to `state.patches_applied` for the save gate.

### `phase_transition`

Requests a phase change in exploration mode. Validates via `ExplorationState.can_transition_to()`.

### `save_memory`

Persists a fact to `RIKUGAN.md` in the IDB/BNDB directory:
```json
{"fact": "sub_401230 is the snake initializer", "category": "function_purpose"}
```

### `spawn_subagent`

Creates an isolated `SubagentRunner` with its own `SessionState`:
```json
{"task": "Analyze the main function", "max_turns": 5}
```

---

## Skill System

**Files**: `rikugan/skills/loader.py`, `rikugan/skills/registry.py`

### Skill Format

Skills are Markdown files with YAML frontmatter:

```markdown
---
name: Malware Analysis
description: Windows PE malware analysis workflow
tags: [malware, windows]
allowed_tools: [decompile_function, list_imports, search_strings]
mode: exploration   # optional: exploration, plan
---
Task: Analyze this binary as potential malware.

## Approach
1. Check imports for suspicious APIs...
```

### Frontmatter Fields

| Field | Type | Description |
|-------|------|-------------|
| `name` | str | Display name |
| `description` | str | One-line description |
| `tags` | list | Categorization tags |
| `allowed_tools` | list | Tool whitelist (empty = all tools) |
| `mode` | str | `"exploration"` activates exploration mode, `"plan"` activates plan mode |

### Discovery

`SkillRegistry.discover()` scans:
1. Built-in skills: `rikugan/skills/builtins/*/SKILL.md`
2. User skills: `~/.idapro/rikugan/skills/*/SKILL.md` (IDA) or `~/.binaryninja/rikugan/skills/*/SKILL.md` (BN)

Reference files in `references/*.md` subdirectories are automatically appended to the skill body.

### Skill Activation

When a user types `/<slug>`, `_resolve_skill()` in `loop.py`:
1. Finds the matching `SkillDefinition`
2. Prepends the skill body to the system prompt
3. If `allowed_tools` is set, filters the tool list
4. If `mode: exploration`, activates exploration mode

### Built-in Skills (12)

| Slug | Purpose |
|------|---------|
| `/malware-analysis` | Windows PE malware triage |
| `/linux-malware` | ELF malware analysis |
| `/deobfuscation` | String decryption, CFF, opaque predicates |
| `/vuln-audit` | Buffer overflow, format string, integer bugs |
| `/driver-analysis` | Windows kernel driver analysis |
| `/ctf` | CTF challenge solving |
| `/generic-re` | General reverse engineering |
| `/ida-scripting` | IDAPython API reference |
| `/binja-scripting` | Binary Ninja Python API reference |
| `/modify` | Exploration mode: autonomous binary modification |
| `/smart-patch-ida` | IDA-specific binary patching |
| `/smart-patch-binja` | Binary Ninja-specific binary patching |

---

## Exploration Mode

**Files**: `rikugan/agent/exploration_mode.py`, `rikugan/agent/loop.py` (`_run_exploration_mode()`)

Exploration mode is a **4-phase autonomous agent flow** for binary modification:

```
EXPLORE ──→ PLAN ──→ EXECUTE ──→ SAVE
```

### Phase 1: EXPLORE

The agent autonomously investigates the binary to understand the user's goal.

- Triggered by `/modify <goal>` or `/explore <goal>` or skills with `mode: exploration`
- Uses all analysis tools + `exploration_report` pseudo-tool + `phase_transition` pseudo-tool
- Findings are accumulated in a `KnowledgeBase`:
  - `relevant_functions` — discovered functions with addresses and summaries
  - `findings` — structured findings (function_purpose, hypothesis, data_structure, etc.)
  - `hypotheses` — extracted from findings with `category="hypothesis"`
- Turn limit: 30 turns (`max_explore_turns`)
- For `/modify`: Phase 1 runs as a **subagent** (isolated context window)
- For `/explore`: Phase 1 runs inline (explore-only, no patching phases follow)
- System addendum: `EXPLORATION_SYSTEM_ADDENDUM` guides the agent's strategy

### Phase Transition Gate

To move from EXPLORE → PLAN, `KnowledgeBase.has_minimum_for_planning` must be true:
- At least 1 relevant function
- At least 1 hypothesis
- At least 1 hypothesis with `relevance="high"`

If the gate fails, the agent receives a gap description and continues exploring.

### Phase 2: PLAN

The agent synthesizes findings into a concrete modification plan.

- Receives `PLAN_SYNTHESIS_PROMPT` with `KnowledgeBase.to_summary()`
- Outputs a numbered list of changes, each with target address, current/proposed behavior, patch strategy
- Parsed into `ModificationPlan` with `PlannedChange` objects
- Addresses extracted from step text via regex
- User must approve the plan before execution proceeds
- Approved plans are persisted to `RIKUGAN.md` for cross-session reference

### Phase 3: EXECUTE

The agent applies patches in-memory for each planned change.

- Iterates over `ModificationPlan.changes`
- Each step uses `EXECUTE_STEP_PROMPT` with the change description
- Activates the platform-specific patching skill:
  - IDA Pro → `smart-patch-ida`
  - Binary Ninja → `smart-patch-binja`
- After each patch, the agent calls `exploration_report(category="patch_result")` with `original_hex`/`new_hex`
- This creates a `PatchRecord` in `state.patches_applied`
- Turn limit: 20 turns (`max_execute_turns`)

### Phase 4: SAVE

User approval gate before persisting changes.

- Emits `SAVE_APPROVAL_REQUEST` with patch count, total bytes, verification status, per-patch details
- User responds "Save All" or "Discard All"
- **Save**: emits `SAVE_COMPLETED`
- **Discard**: rolls back patches by writing `PatchRecord.original_bytes` back via `execute_python`, emits `SAVE_DISCARDED`

### ExplorationState

```python
@dataclass
class ExplorationState:
    phase: ExplorationPhase
    knowledge_base: KnowledgeBase
    modification_plan: Optional[ModificationPlan]
    patches_applied: List[PatchRecord]
    explore_turns: int
    execute_turns: int
    total_turns: int        # monotonic counter for UI
    max_explore_turns: int  # default 30
    max_execute_turns: int  # default 20
    explore_only: bool      # True for /explore (no patching)
```

### `/explore` vs `/modify`

| Aspect | `/explore` | `/modify` |
|--------|-----------|-----------|
| Phases | EXPLORE only | EXPLORE → PLAN → EXECUTE → SAVE |
| Subagent | No (inline) | Yes (Phase 1 in subagent) |
| Patching | No | Yes |
| Knowledge base | Accumulated, returned to caller | Accumulated, passed to Phase 2 |

---

## Plan Mode

**Files**: `rikugan/agent/plan_mode.py`, `rikugan/agent/loop.py`

Plan mode is a simpler two-step workflow: **plan first, then execute**.

Triggered by `/plan <message>`.

### Flow

1. **Plan Generation** — LLM receives `_PLAN_GENERATION_PROMPT` and outputs a numbered list
2. **Plan Parsing** — `parse_plan()` extracts numbered steps from the text
3. **User Approval** — `PLAN_GENERATED` event; user approves or rejects
4. **Step Execution** — For each step:
   - Emit `PLAN_STEP_START`
   - Run a full turn cycle with `_STEP_EXECUTION_PROMPT`
   - Emit `PLAN_STEP_DONE`

Plan mode is orthogonal to exploration mode. `/plan` does not enter exploration phases.

---

## Subagents

**File**: `rikugan/agent/subagent.py`

Subagents are isolated `AgentLoop` instances with their own `SessionState`. They keep the parent's context window clean from verbose tool output.

### `SubagentRunner`

```python
class SubagentRunner:
    def run_task(self, task, max_turns=20) -> Generator[TurnEvent, None, str]:
        # General-purpose: returns final text
        loop = AgentLoop(provider, tools, config, fresh_session)
        for event in loop.run(augmented_task):
            yield event
        return final_text

    def run_exploration(self, user_goal, max_turns=30) -> Generator[TurnEvent, None, KnowledgeBase]:
        # Phase 1 specific: returns KnowledgeBase
        loop = AgentLoop(provider, tools, config, fresh_session)
        for event in loop.run(f"/explore {user_goal}"):
            yield event
        return loop.last_knowledge_base
```

### `spawn_subagent` Pseudo-Tool

The LLM can also spawn subagents via the `spawn_subagent` pseudo-tool:

```json
{"task": "Analyze the main function", "max_turns": 5}
```

The parent loop creates a `SubagentRunner`, delegates all events to the UI, and receives only the compact text summary.

### Knowledge Base Transfer

When a subagent running in explore mode finishes:
1. `_clear_exploration_state()` saves the `KnowledgeBase` to `_last_knowledge_base`
2. The parent accesses it via the `last_knowledge_base` property
3. The parent populates its own `ExplorationState.knowledge_base` from the subagent's results
4. Phases 2-4 proceed in the parent with a clean context window

---

## Mutation Tracking and Undo

**File**: `rikugan/agent/mutation.py`

Every mutating tool call (`defn.mutating=True`) is recorded on `SessionState.mutation_log` for undo support. The log lives on the session because an `AgentLoop` is built per user message, and `/undo` is itself a user message, so a log owned by the loop would always be empty by the time it was read. `AgentLoop._mutation_log` is a property delegating to it, and subagents share the same list. It is not persisted: a reverse call only means something against a live database.

### `MutationRecord`

```python
@dataclass
class MutationRecord:                    # rikugan/core/types.py
    tool_name: str                       # e.g., "rename_function"
    arguments: dict[str, Any]            # original arguments
    reverse_tool: str                    # tool to call for undo
    reverse_arguments: dict[str, Any]    # arguments for undo
    timestamp: float
    description: str                     # human-readable
    reversible: bool = True
    irreversible_reason: str = ""        # shown in /undo's reply and the panel tooltip
```

### `build_reverse_record()`

`_REVERSE_BUILDERS` generates reverse operations for tools where that is possible, and `_KNOWN_IRREVERSIBLE` lists the rest. `tests/agent/test_mutation_contract.py` fails for any mutating tool in neither.

| Tool | Reverse Strategy |
|------|-----------------|
| `rename_function` | Restore the name read through `read_function_name` |
| `rename_variable` | Swap `old_name` ↔ `new_name` |
| `rename_address` | Restore the name read through `get_name_at`. An address that had none is recorded as non-reversible |
| `set_comment`, `set_function_comment` | Write back the comment read through `read_comment` / `read_function_comment`, in the same slot. Writing an empty one is how both hosts remove it |
| `set_pseudocode_comment` | The same, through `get_pseudocode_comment` (IDA only) |
| `set_function_prototype` | Write back the type read through `read_function_prototype`. A function with none is recorded as non-reversible |
| Everything in `_KNOWN_IRREVERSIBLE` | **Not reversible** (`reversible=False`) |

`tests/agent/test_mutation_contract.py` also derives arguments from each tool's real parameters, and checks that every getter and reverse call is one that tool accepts, in both hosts. `tests/agent/test_undo_against_hosts.py` runs each round trip through the real tools over a fake database.

### `capture_pre_state()`

Calls getter tools before the mutation. The `read_*` readers are `internal`, so they add nothing to the tool list sent to the model, and return raw values, since `/undo` passes them straight back to the setter. If a read fails, it records `pre["_capture_failed"]`, and the mutation is logged as non-reversible with that reason rather than failing at undo time. A reader's "there is nothing" message must not be taken for a value: `get_name_at` says "No name at 0x...", which would otherwise become the name.

`set_type`, `propagate_type` and `apply_type_to_variable` need a type-at-address reader that neither host has, so they stay non-reversible.

### `/undo [N]`

The `/undo` command runs on a new loop over the same session:
1. Parses the count (default 1)
2. Takes records from the end of the log, newest first
3. A non-reversible record is dropped with its reason
4. Otherwise calls the reverse tool. On success the record is popped. On `ToolError` it **stays and undo stops**: the mutation is still applied, and carrying on would reverse older ones out of order
5. `UndoNoLongerPossible` means a retry cannot help (a file changed since, a discarded backup), so the record is dropped and older undos continue
6. Yields `MUTATION_UNDONE` for each record that left, then a summary

Analysis tools report failure by returning text rather than raising, so `mutation_succeeded()` recognises how each reversible tool opens its message when it worked, and anything else counts as failed. It applies both ways: a reversal that failed keeps its record, and a mutation that failed adds none. A new reversible tool has to open with one of `_SUCCESS_PREFIXES`.

### UI Integration

- `MutationLogPanel` (in a horizontal `QSplitter` alongside the chat) lists the **active chat's** log. There is one panel per window and one log per chat, so `RikuganPanelCore._sync_mutation_panel()` reads the log rather than accumulating events, on `MUTATION_RECORDED`, `MUTATION_UNDONE`, a tab switch, a database change and the end of a turn. The last catches silent subagents, whose events do not arrive
- "Undo Last" button submits `/undo 1` through the agent loop

---

## Files and Shell

**Files**: `rikugan/core/workspace.py`, `rikugan/core/artifacts.py`, `rikugan/core/chat_context.py`, `rikugan/tools/filesystem.py`, `rikugan/tools/file_search.py`, `rikugan/tools/shell.py`

Registered in both hosts, category `filesystem`. Neither host marshals this category onto its main thread. Per-chat state is `SessionState.files` (`FileAccess`), shared with subagents: the workspace, artifact store, read ledger and consent answer. Handlers find their chat through a context variable, which `AgentLoop._run_tool` sets and the registry carries onto the worker thread.

### Workspace

`SessionControllerBase.start_agent` builds a `Workspace` on the UI thread each turn, since IDA's path queries are main-thread only and a Binary Ninja view can change path. Roots are the binary's directory, `<config>/scratch/<db_instance_id>/` (addressed as `scratch/`) and `filesystem_extra_roots`. A binary directory that is the user's home or an ancestor of it is not used. See the [safety rules in AGENTS.md](AGENTS.md#filesystem-and-shell-safety).

### Editing

| Rule | Why |
|------|-----|
| Read before write | A session ledger of `path -> st_mtime_ns`. Refuses a file not read, or changed since. Prevents clobbering content the model never saw |
| Exact match only | No regex, normalization or fuzzy application. A tool that sometimes guesses right is worse than one that fails loudly |
| Unique by default | More than one match fails and lists every line number, or `replace_all` |
| One diagnosis on failure | Names copied line numbers or differing whitespace, without applying anything |
| Line endings kept | A CRLF file's strings are matched as CRLF, since `read_file` hides the `\r` |
| Atomic | Temp file plus `os.replace()`, keeping the mode |

### Undo for files

Each change copies the old file to `scratch/.backups/` (capped at 200 files / 100 MB, oldest dropped) and appends a `MutationRecord` whose reverse is `restore_file_backup`, an `internal` tool: an agent able to call it could silently revert the user's edits. Restoring also restores the backup's mtime, which is what the previous edit recorded, so a chain of edits undoes one at a time. A file changed since is left alone and reported. What `run_shell` does is not undoable.

### Oversized output

`format_large_result` renders anything over budget (20,000 characters) as numbered head and tail, 60/40, with a notice directly under the title:

```
=== build.log ===
⟦Showing lines 1-246 and 9936-10181 of 10181; 9,689 middle lines (1.8MB) elided. Continue with read_file("artifact://64", offset=247)⟧
```

The rest goes to an in-memory `ArtifactStore` (2 MB each, 8 MB per chat, 32 artifacts, least recently used dropped, ids never reused). Past 2 MB only both ends are saved, the notice says so, and a read landing in that gap says so too. The notice comes first because old tool results are later cut to their first 500 characters. These tools' results also get more room in `SessionState._truncate_results`: without it the model could not hold more than about 600 lines of a file.

### Search and shell

`search_files` uses ripgrep when `shutil.which("rg")` finds it and a Python scan otherwise, with the same rows from both. It names the engine in its result, because their regex dialects differ. Neither follows symlinks, and a search that stops early says why.

`scratch/` means the agent's scratch directory only to the file tools. A shell would read it as a folder inside its working directory, so a file just written looks missing, and `mkdir scratch` leaves a stray folder beside the sample. `run_shell` therefore refuses that use before anything runs, unless such a folder exists, and exports `$SCRATCH`. `write_file` and the system prompt state the real paths.

`run_shell` runs with `shell=True`, no stdin, its own process group and a timeout clamped to 240 s. Timeout and Stop kill the group. Its tool timeout is longer than that, since the registry's cannot stop a thread and would orphan the process. Capture keeps the head and tail of each stream and keeps draining past the limit, so a chatty command cannot block on a full pipe.

---

## Context Window Management

**File**: `rikugan/agent/context_window.py`

### `ContextWindowManager`

Tracks token usage and compacts the conversation when approaching limits.

```python
class ContextWindowManager:
    max_tokens: int           # from config (default 128000)
    compaction_threshold: 0.8 # compact when usage > 80%

    def should_compact() -> bool
    def compact_messages(messages) -> List[Message]
    def estimate_tokens(text) -> int  # ~3.5 chars/token heuristic
```

### Compaction Strategy

When `should_compact()` returns True:
1. Keep the first message (system/initial)
2. Keep the last 4 messages (recent context)
3. Summarize all middle messages into one `[Context summary]` message

### Per-Message Truncation

`SessionState._truncate_results()` caps tool results to prevent individual messages from consuming too much context. Truncated results include `[...N chars omitted]` markers.

### Integration with AgentLoop

At the top of each turn in `run()`:
```python
if self._context_manager.should_compact():
    messages = self._context_manager.compact_messages(messages)
```

---

## Persistent Memory

**Files**: `rikugan/agent/system_prompt.py`, `rikugan/agent/loop.py`

### RIKUGAN.md

A per-binary Markdown file stored alongside the IDB/BNDB. It acts as cross-session memory.

- **Location**: `<idb_directory>/RIKUGAN.md`
- **Loading**: First 200 lines loaded into the system prompt at the start of every session
- **Writing**: Via the `save_memory` pseudo-tool or plan persistence

### `save_memory` Pseudo-Tool

The LLM can persist facts:
```json
{"fact": "sub_401230 is the snake initializer, length at +0x1A", "category": "function_purpose"}
```

Categories: `function_purpose`, `architecture`, `naming_convention`, `prior_analysis`, `general`.

### Plan Persistence

Approved plans from exploration mode are saved to RIKUGAN.md with a timestamp, preserving analysis context across sessions.

### `/memory` Command

Shows the current contents of RIKUGAN.md in the chat.

---

## Session Management

**Files**: `rikugan/state/session.py`, `rikugan/state/history.py`, `rikugan/ui/session_controller_base.py`

### `SessionState`

```python
@dataclass
class SessionState:
    id: str                        # unique hex ID
    created_at: float
    messages: List[Message]        # full conversation history
    total_usage: TokenUsage        # cumulative token usage
    last_prompt_tokens: int        # most recent prompt size
    current_turn: int
    is_running: bool
    provider_name: str
    model_name: str
    idb_path: str
    metadata: Dict[str, str]
```

Key methods:
- `add_message(msg)` — Appends and updates token tracking
- `get_messages_for_provider(context_window)` — Returns sanitized, trimmed messages
- `_sanitize()` — Patches orphaned `tool_use` blocks with synthetic error results
- `_truncate_results()` — Caps tool result sizes
- `_trim_to_budget()` — Drops oldest messages if over budget

### Multi-Tab Sessions

`SessionControllerBase` manages multiple sessions:

```python
class SessionControllerBase:
    _sessions: Dict[str, SessionState]  # tab_id → session
    _active_tab_id: str

    def create_tab() -> str
    def close_tab(tab_id)
    def switch_tab(tab_id)
    def fork_session(source_tab_id) -> Optional[str]  # deep copy
```

### Session Fork

`fork_session()` creates a deep copy of a session's messages and state into a new tab. The forked session gets `metadata["forked_from"]` set to the source session ID. Useful for branching analysis from a checkpoint.

### Persistence

`SessionHistory` handles save/restore:
- Sessions are JSON-serialized to `<config_dir>/rikugan/sessions/`
- Auto-saved after each agent turn (if `checkpoint_auto_save` is enabled)
- Restored per-file when the same IDB/BNDB is reopened
- Full round-trip: messages, token usage, tool calls, tool results all preserved

---

## MCP Integration

**Files**: `rikugan/mcp/client.py`, `rikugan/mcp/bridge.py`, `rikugan/mcp/manager.py`

### Architecture

```
mcp.json config → MCPManager → MCPClient (per server) → subprocess (stdio)
                      ↓
                 MCPBridge → ToolRegistry
```

### MCPClient

Communicates with an MCP server subprocess via JSON-RPC 2.0 + Content-Length framing.

Key features:
- **Heartbeat**: Background thread pings the server every 30s. Marks `_healthy=False` on failure
- **`is_healthy` property**: Returns False if heartbeat failed or process died
- **Per-request timeout**: Configurable default (from `MCP_DEFAULT_TIMEOUT`)
- **Tool discovery**: `tools/list` RPC call at startup populates `_tools`

### MCPBridge

Converts MCP tool schemas to `ToolDefinition` objects and registers them in the `ToolRegistry` with the prefix `mcp_<server>_<tool>`.

### `/mcp` Command

Shows the health status of all configured MCP servers (running, healthy, tool count).

---

## Provider Layer

**Files**: `rikugan/providers/base.py`, `rikugan/providers/registry.py`, `rikugan/providers/*.py`

### `LLMProvider` ABC

```python
class LLMProvider(ABC):
    def chat(self, messages, tools, temperature, max_tokens, system) -> Message
    def chat_stream(self, messages, tools, ...) -> Generator[StreamChunk]
    def list_models() -> List[ModelInfo]
```

### Providers

| Provider | File | Notes |
|----------|------|-------|
| Anthropic (Claude) | `anthropic_provider.py` | OAuth auto-detection, prompt caching |
| OpenAI | `openai_provider.py` | Standard OpenAI SDK |
| Gemini | `gemini_provider.py` | google-genai SDK |
| Ollama | `ollama_provider.py` | Local inference |
| OpenAI-Compatible | `openai_compat.py` | Custom API base |

### Prompt Caching (Anthropic)

`cache_control: {"type": "ephemeral"}` is set on:
1. The system prompt (stable across turns)
2. The last tool result message
3. The last user message

This enables Anthropic's server-side prompt caching for 2-10x cost reduction on long conversations.

### Retry Logic

In `_stream_llm_turn()`:
- `RateLimitError` triggers exponential backoff (1s, 2s, 4s) up to 3 retries
- `ProviderError` with `retryable=True` follows the same pattern
- User sees "Rate limited, retrying in Ns..." via `TEXT_DELTA` events

---

## System Prompt Architecture

**Files**: `rikugan/agent/system_prompt.py`, `rikugan/agent/prompts/`

### Prompt Structure

```
┌─────────────────────────────┐
│  Host-specific base prompt  │ ← ida.py or binja.py
│  (tool usage guidelines,    │
│   discipline, safety)       │
├─────────────────────────────┤
│  Persistent Memory          │ ← RIKUGAN.md (first 200 lines)
├─────────────────────────────┤
│  Current Binary info        │ ← binary name, arch, entry point
├─────────────────────────────┤
│  Current Position           │ ← address + function name
├─────────────────────────────┤
│  Available Tools            │ ← comma-separated tool names
├─────────────────────────────┤
│  Active Skill               │ ← skill body (if any)
├─────────────────────────────┤
│  Exploration Addendum       │ ← only during exploration mode
└─────────────────────────────┘
```

### Shared Prompt Sections (`prompts/base.py`)

- `DISCIPLINE_SECTION` — "Do exactly what was asked"
- `RENAMING_SECTION` — Renaming/retyping guidelines
- `ANALYSIS_SECTION` — Analysis approach
- `SAFETY_SECTION` — Safety guidelines
- `TOKEN_EFFICIENCY_SECTION` — Prefer search over listing
- `CLOSING_SECTION` — Final reminders

---

## UI Layer

**Files**: `rikugan/ui/panel_core.py`, `rikugan/ui/chat_view.py`, `rikugan/ui/message_widgets.py`

### `RikuganPanelCore`

The main Qt widget. Layout:

```
┌─────────────────────────────────────────────┐
│ Tab Bar (+ button, close buttons)           │
├────────────────────────┬────────────────────┤
│                        │                    │
│   QTabWidget           │  MutationLogPanel  │
│   (ChatView per tab)   │  (toggle-able)     │
│                        │                    │
├────────────────────────┴────────────────────┤
│ [InputArea] [Send] [Stop] [New] [Export]    │
│             [Settings] [Mutations]          │
├─────────────────────────────────────────────┤
│ ContextBar (model name, token count)        │
└─────────────────────────────────────────────┘
```

### Event Polling

A `QTimer` fires every 50ms, calling `_poll_events()`:
1. Dequeues up to 20 events from `BackgroundAgentRunner`
2. Routes each to `ChatView.handle_event()`
3. Checks for `USER_QUESTION` / `SAVE_APPROVAL_REQUEST` to enable input
4. Updates token display on `USAGE_UPDATE`
5. Refreshes `MutationLogPanel` on `MUTATION_RECORDED` and `MUTATION_UNDONE`

### Tool review

`ToolCallWidget` builds its expanded view on first open, since most cards are never opened. `tool_review.build_review()` turns a call's JSON into what it means, and `ToolReviewView` renders it, also on `ToolApprovalWidget`:

| Tool | Shown as |
|------|----------|
| `write_file` | Highlighted file contents, by extension |
| `edit_file` | Line diff, with `-`/`+` in the gutter and tinted lines |
| `execute_python`, `run_shell` | Highlighted code |
| `read_file`, `search_files` | What it covers: range, glob, mode |
| Anything else | The raw arguments |

Results lose the sanitizer's wrapper (`unwrap_tool_result`), which is for the model. `code_view.scan_line()` is free of Qt, so its behaviour is tested directly. It reads each line with one pattern, so the token that starts first wins, and carries state across lines for docstrings and block comments. Layout needs real Qt, which `tests/tools/test_tool_review.py::TestUnderRealQt` runs in a subprocess, because the rest of the suite has stubs where Qt should be.

### `ChatView`

Scrollable chat area. Renders `TurnEvent` → widget:

| Event | Widget |
|-------|--------|
| `TEXT_DELTA` / `TEXT_DONE` | `AssistantMessageWidget` (Markdown rendered) |
| `TOOL_CALL_*` | `ToolCallWidget` (collapsible, syntax-highlighted) |
| `TOOL_RESULT` | Updates `ToolCallWidget` result section |
| `TURN_START` | `ThinkingWidget` (animated dots) |
| `ERROR` | `ErrorMessageWidget` |
| `PLAN_GENERATED` | `PlanView` (step list with status indicators) |
| `TOOL_APPROVAL_REQUEST` | `ToolApprovalWidget` (Allow/Deny buttons) |
| `EXPLORATION_PHASE_CHANGE` | `ExplorationPhaseWidget` |
| `EXPLORATION_FINDING` | `ExplorationFindingWidget` |

Tool call batching: consecutive calls to the same tool are merged into a `ToolBatchWidget` to reduce visual noise. A preview budget of 3 tool previews per turn prevents UI clutter.

### `MutationLogPanel`

Side panel showing mutation history:
- `MutationEntryWidget` per mutation — shows timestamp, description, reversibility indicator, tool badge
- "Undo Last" button emits `undo_requested` signal → submits `/undo 1`
- Count label updates dynamically

---

## Thread Safety Model

### Background Thread

`BackgroundAgentRunner` runs the agent loop in a daemon thread. All `TurnEvent` objects are passed to the UI via `queue.Queue`.

### IDA API Marshalling

IDA Pro requires all API calls on the main thread. The `@idasync` decorator in `core/thread_safety.py` marshalls calls:
- If already on main thread: execute directly
- If on background thread: schedule via `ida_kernwin.execute_sync()` and wait

Binary Ninja tools run directly — BN's API is thread-safe.

### User Answer/Approval Queues

Two `queue.Queue(maxsize=1)` instances replace the old `threading.Event` + mutable field pattern:
- `_user_answer_queue` — For `USER_QUESTION` responses (plan approval, save gate, etc.)
- `_tool_approval_queue` — For `execute_python` approval

The agent waits with `queue.get(timeout=0.5)` in a loop, checking for cancellation between attempts. The UI thread calls `put()`. No race condition possible.

---

## Error Handling and Retry

### Exception Hierarchy (`core/errors.py`)

```
RikuganError
├── AgentError          — loop-level errors
├── CancellationError   — user cancelled
├── ProviderError       — LLM API errors
│   └── RateLimitError  — HTTP 429
├── ToolError           — tool execution errors
├── ToolValidationError — argument validation
├── MCPError            — MCP protocol errors
│   ├── MCPConnectionError
│   └── MCPTimeoutError
└── SkillError          — skill loading errors
```

### Retry Logic

In `_stream_llm_turn()`:
```python
for attempt in range(max_retries):
    try:
        yield from stream
        break
    except RateLimitError as e:
        wait = e.retry_after or (2 ** attempt)
        yield TEXT_DELTA(f"Rate limited, retrying in {wait}s...")
        time.sleep(wait)
```

### Consecutive Error Tracking

`_consecutive_errors` counts sequential tool failures. After 3 consecutive errors, `_tools_disabled_for_turn` is set — the LLM receives tools as unavailable for the current turn, forcing it to respond with text instead of looping on broken calls.

---

## Logging

**File**: `rikugan/core/logging.py`

### Log Outputs

1. **IDA Output Window** — `IDAHandler`, INFO level, `[Rikugan] LEVEL: message`
2. **Debug File** — `_FlushFileHandler`, DEBUG level, flushed + fsynced after every write
   - Location: `<config_dir>/rikugan/rikugan_debug.log`
   - Survives crashes (fsync)
3. **Structured JSON** — `_JSONFormatter`, INFO level, JSONL format
   - Location: `<config_dir>/rikugan/rikugan_structured.jsonl`
   - Append mode, machine-parseable

### JSON Log Format

```json
{"ts": 1709500000.123, "level": "INFO", "thread": "Thread-1", "msg": "Subagent started"}
```

---

## Commands Reference

| Command | Description |
|---------|-------------|
| `/plan <msg>` | Enter plan mode: generate plan, then execute step-by-step |
| `/modify <msg>` | Enter exploration mode: EXPLORE → PLAN → EXECUTE → SAVE |
| `/explore <msg>` | Enter explore-only mode: autonomous read-only analysis |
| `/memory` | Show current RIKUGAN.md contents |
| `/undo [N]` | Undo last N mutations (default 1) |
| `/mcp` | Show MCP server health status |
| `/doctor` | Diagnose provider, API key, tools, skills, config issues |
| `/<skill-slug>` | Activate a skill (e.g., `/malware-analysis`, `/ctf`) |

---

## Data Flow Diagrams

### Normal Turn

```
User "Explain main()"
  │
  ├─→ SessionState.add_message(USER)
  ├─→ build_system_prompt()
  ├─→ provider.chat_stream(messages, tools, system)
  │     ├─→ TEXT_DELTA "The main function..."
  │     ├─→ TOOL_CALL_START "decompile_function"
  │     ├─→ TOOL_CALL_DONE
  │     └─→ USAGE_UPDATE
  ├─→ ToolRegistry.execute("decompile_function", {"name": "main"})
  │     └─→ TOOL_RESULT "int main() { ... }"
  ├─→ SessionState.add_message(TOOL)
  ├─→ provider.chat_stream(messages + tool_result)
  │     ├─→ TEXT_DELTA "This function initializes..."
  │     └─→ TEXT_DONE
  └─→ TURN_END
```

### Exploration Mode (`/modify`)

```
User "/modify Change score from 100 to 999"
  │
  ├─→ Phase 1: EXPLORE (subagent)
  │     ├─→ SubagentRunner.run_exploration()
  │     │     ├─→ [subagent uses tools, logs findings]
  │     │     ├─→ exploration_report → KnowledgeBase
  │     │     └─→ phase_transition("plan") → KnowledgeBase returned
  │     └─→ Parent receives KnowledgeBase summary (~1-2KB)
  │
  ├─→ Phase 2: PLAN
  │     ├─→ PLAN_SYNTHESIS_PROMPT + KB summary → LLM
  │     ├─→ Parse plan → ModificationPlan
  │     └─→ User approves plan
  │
  ├─→ Phase 3: EXECUTE
  │     ├─→ For each PlannedChange:
  │     │     ├─→ EXECUTE_STEP_PROMPT → LLM
  │     │     ├─→ Smart patch skill activated
  │     │     ├─→ execute_python (with approval) → patch bytes
  │     │     ├─→ redecompile_function → verify
  │     │     └─→ exploration_report(category="patch_result") → PatchRecord
  │     └─→ All patches applied
  │
  └─→ Phase 4: SAVE
        ├─→ SAVE_APPROVAL_REQUEST → User
        ├─→ "Save All" → write to file → SAVE_COMPLETED
        └─→ "Discard All" → restore original bytes → SAVE_DISCARDED
```

### Mutation Tracking

```
LLM calls rename_function(old="sub_401000", new="main")
  │
  ├─→ capture_pre_state() → {} (no pre-state needed for renames)
  ├─→ ToolRegistry.execute("rename_function", {...})
  ├─→ build_reverse_record() → MutationRecord(
  │     reverse_tool="rename_function",
  │     reverse_args={"old_name": "main", "new_name": "sub_401000"})
  ├─→ session.mutation_log.append(record)
  └─→ MUTATION_RECORDED event → UI (MutationLogPanel)

User "/undo" (a new AgentLoop over the same session)
  │
  ├─→ ToolRegistry.execute("rename_function", {"old_name": "main", "new_name": "sub_401000"})
  ├─→ Pop the record from session.mutation_log
  ├─→ MUTATION_UNDONE event → UI (MutationLogPanel)
  └─→ TEXT_DONE "Undid 1 mutation(s)."
```
