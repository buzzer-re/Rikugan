"""Agent loop: generator-based turn cycle with tool orchestration."""

from __future__ import annotations

import dataclasses
import json
import os
import threading
import time
import queue
import traceback
from typing import Any, Callable, Dict, Generator, List, Optional, Tuple

from ..core.config import RikuganConfig
from ..core.errors import AgentError, CancellationError, ProviderError, RateLimitError, ToolError
from ..core.logging import log_debug, log_error, log_info
from ..core.types import Message, Role, StreamChunk, TokenUsage, ToolCall, ToolResult
from ..providers.base import LLMProvider
from ..tools.registry import ToolRegistry
from ..skills.registry import SkillRegistry
from .context_window import ContextWindowManager
from .modes.normal import run_normal_loop
from .modes.plan import run_plan_mode
from .modes.exploration import run_exploration_mode
from .plan_mode import parse_plan as _parse_plan_impl
from .system_prompt import build_system_prompt
from .turn import TurnEvent, TurnEventType
from .mutation import MutationRecord, build_reverse_record, capture_pre_state
from .subagent import SubagentRunner
from .exploration_mode import (
    ExplorationPhase, ExplorationState, Finding, FunctionInfo,
    KnowledgeBase, PatchRecord,
)
from .minify import minify_text, minify_messages
from ..core.sanitize import sanitize_tool_result, sanitize_skill_body, strip_injection_markers
from ..state.session import SessionState

# Minimum acceptable context window; smaller values get flagged by /doctor.
_MIN_CONTEXT_WINDOW_TOKENS = 8_000

_MEMORY_HEADER = (
    "# Rikugan Persistent Memory\n\n"
    "This file persists across sessions. "
    "The agent reads the first 200 lines into its system prompt.\n\n"
)


@dataclasses.dataclass
class _ParsedCommand:
    """Result of parsing a user message for slash-command prefixes."""
    message: str
    use_plan_mode: bool = False
    use_exploration_mode: bool = False
    explore_only: bool = False
    direct_command: str = ""  # e.g. "/memory", "/undo", "/mcp", "/doctor"
    direct_arg: str = ""      # remainder after the direct command token


def _parse_user_command(user_message: str) -> _ParsedCommand:
    """Strip slash-command prefixes and return a _ParsedCommand descriptor.

    Direct commands (/memory, /undo, /mcp, /doctor) set `direct_command`.
    Mode prefixes (/plan, /modify, /explore) set the corresponding flag and
    strip the prefix from `message`.  Plain messages are returned unchanged.
    """
    stripped = user_message.strip()
    lower = stripped.lower()
    if lower.startswith("/plan "):
        return _ParsedCommand(message=stripped[6:].strip(), use_plan_mode=True)
    if lower.startswith("/modify "):
        return _ParsedCommand(message=stripped[8:].strip(), use_exploration_mode=True)
    if lower.startswith("/explore "):
        return _ParsedCommand(
            message=stripped[9:].strip(),
            use_exploration_mode=True,
            explore_only=True,
        )
    if lower == "/memory":
        return _ParsedCommand(message=stripped, direct_command="/memory")
    if lower.startswith("/undo"):
        return _ParsedCommand(
            message=stripped, direct_command="/undo", direct_arg=stripped,
        )
    if lower == "/mcp":
        return _ParsedCommand(message=stripped, direct_command="/mcp")
    if lower == "/doctor":
        return _ParsedCommand(message=stripped, direct_command="/doctor")
    return _ParsedCommand(message=stripped)


def _append_to_memory_file(md_path: str, content: str) -> None:
    """Create RIKUGAN.md with header if missing, then append *content*."""
    if not os.path.exists(md_path):
        with open(md_path, "w", encoding="utf-8") as f:
            f.write(_MEMORY_HEADER)
    with open(md_path, "a", encoding="utf-8") as f:
        f.write(content)

# Static pseudo-tool schemas — don't depend on runtime state
_EXPLORATION_REPORT_SCHEMA = {
    "type": "function",
    "function": {
        "name": "exploration_report",
        "description": (
            "Log a structured finding during binary exploration. "
            "Call this whenever you discover something relevant to "
            "the user's goal: a function's purpose, a key constant, "
            "a data structure, or a hypothesis about what to change."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "category": {
                    "type": "string",
                    "description": "Type of finding.",
                    "enum": ["function_purpose", "data_structure", "constant", "hypothesis",
                             "string_ref", "import_usage", "patch_result", "general"],
                },
                "address": {"type": "integer", "description": "Address related to this finding (hex or decimal)."},
                "function_name": {"type": "string", "description": "Name of the function (for function_purpose findings)."},
                "summary": {"type": "string", "description": "Brief summary of the finding."},
                "evidence": {"type": "string", "description": "Supporting evidence (e.g. decompiled code snippet)."},
                "relevance": {"type": "string", "description": "How relevant to the user's goal.", "enum": ["low", "medium", "high"]},
                "original_hex": {"type": "string", "description": "Original bytes as hex string (for patch_result category). E.g. '74 05'."},
                "new_hex": {"type": "string", "description": "New patched bytes as hex string (for patch_result category). E.g. '75 05'."},
            },
            "required": ["category", "summary"],
        },
    },
}
_PHASE_TRANSITION_SCHEMA = {
    "type": "function",
    "function": {
        "name": "phase_transition",
        "description": (
            "Request to move to the next exploration phase. "
            "Call with to_phase='plan' when you have identified "
            "all locations that need to change and have formed "
            "concrete hypotheses. Requires at least 1 relevant "
            "function and 1 hypothesis logged via exploration_report."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "to_phase": {"type": "string", "description": "Target phase to transition to.", "enum": ["plan"]},
                "reason": {"type": "string", "description": "Why you're ready to transition."},
            },
            "required": ["to_phase", "reason"],
        },
    },
}
_SAVE_MEMORY_SCHEMA = {
    "type": "function",
    "function": {
        "name": "save_memory",
        "description": (
            "Save a fact to persistent memory (RIKUGAN.md). "
            "Use this to remember important findings across sessions: "
            "function purposes, naming conventions, architecture notes, "
            "or analysis results that would be useful in future sessions."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "fact": {"type": "string", "description": "The fact or finding to remember."},
                "category": {
                    "type": "string",
                    "description": "Category of the memory.",
                    "enum": ["function_purpose", "architecture", "naming_convention",
                             "prior_analysis", "data_structure", "general"],
                },
            },
            "required": ["fact", "category"],
        },
    },
}
_SPAWN_SUBAGENT_SCHEMA = {
    "type": "function",
    "function": {
        "name": "spawn_subagent",
        "description": (
            "Spawn an isolated subagent to handle a complex subtask. "
            "The subagent has its own context window and can use all "
            "available tools. It returns a concise summary of its "
            "findings. Use this to delegate research-heavy tasks "
            "(e.g. 'analyze all functions referencing the score string') "
            "without filling your own context with raw tool output."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "task": {"type": "string", "description": "The task for the subagent to perform."},
                "max_turns": {"type": "integer", "description": "Maximum turns for the subagent (default: 20)."},
            },
            "required": ["task"],
        },
    },
}
_ASK_USER_SCHEMA = {
    "type": "function",
    "function": {
        "name": "ask_user",
        "description": (
            "Ask the user a question and wait for their answer. "
            "Use this when you need clarification, confirmation, "
            "or a choice from the user before proceeding."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "question": {"type": "string", "description": "The question to ask the user."},
                "options": {"type": "array", "items": {"type": "string"}, "description": "Optional list of choices for the user."},
            },
            "required": ["question"],
        },
    },
}


class AgentLoop:
    """The core agentic loop: stream LLM -> execute tools -> repeat.

    Uses a generator pattern to yield TurnEvents to the UI layer.
    Runs in a background thread; IDA API calls are marshalled via @idasync.
    """

    def __init__(
        self,
        provider: LLMProvider,
        tool_registry: ToolRegistry,
        config: RikuganConfig,
        session: SessionState,
        skill_registry: Optional[SkillRegistry] = None,
        host_name: str = "IDA Pro",
        parent_loop: Optional["AgentLoop"] = None,
    ):
        self.provider = provider
        self.tools = tool_registry
        self.config = config
        self.session = session
        self.skills = skill_registry
        self.host_name = host_name
        self._cancelled = parent_loop._cancelled if parent_loop else threading.Event()
        self._running = False
        self._consecutive_errors = 0
        self._tools_disabled_for_turn = False
        # Thread-safe queues for user answers and tool approvals (no race condition)
        # Subagents share the parent's queues so UI signals reach them.
        self._user_answer_queue: queue.Queue[str] = (
            parent_loop._user_answer_queue if parent_loop
            else queue.Queue(maxsize=1)
        )
        self._tool_approval_queue: queue.Queue[str] = (
            parent_loop._tool_approval_queue if parent_loop
            else queue.Queue(maxsize=1)
        )
        self._always_allow_scripts = (
            parent_loop._always_allow_scripts if parent_loop else False
        )
        self.plan_mode = False

        # Context window manager — compacts history when approaching limits
        ctx_window = getattr(config.provider, "context_window", 0) or 128000
        self._context_manager = ContextWindowManager(
            max_tokens=ctx_window,
            compaction_threshold=0.8,
        )

        # Mutation log for /undo support
        self._mutation_log: List[MutationRecord] = []

        # Exploration mode state (populated when /modify or /explore is used)
        self._exploration_state: Optional[ExplorationState] = None
        self._last_knowledge_base: Optional[KnowledgeBase] = None

    @property
    def is_running(self) -> bool:
        return self._running

    @property
    def last_knowledge_base(self) -> Optional[KnowledgeBase]:
        """The knowledge base from the most recent exploration run."""
        if self._exploration_state is not None:
            return self._exploration_state.knowledge_base
        return self._last_knowledge_base

    def _clear_exploration_state(self) -> None:
        """Save knowledge base and reset exploration state."""
        if self._exploration_state is not None:
            self._last_knowledge_base = self._exploration_state.knowledge_base
            self._exploration_state = None

    def cancel(self) -> None:
        """Cancel the current run."""
        self._cancelled.set()

    def _drain_queue(self, q: "queue.Queue[str]") -> None:
        """Remove any stale item from a maxsize=1 queue (non-blocking)."""
        while True:
            try:
                q.get_nowait()
            except queue.Empty:
                break

    def submit_user_answer(self, answer: str) -> None:
        """Submit an answer to an ask_user question (called from UI thread)."""
        self._drain_queue(self._user_answer_queue)
        self._user_answer_queue.put(answer)

    def submit_tool_approval(self, decision: str) -> None:
        """Submit tool approval decision: 'allow', 'allow_all', or 'deny'."""
        self._drain_queue(self._tool_approval_queue)
        self._tool_approval_queue.put(decision)

    def _check_cancelled(self) -> None:
        if self._cancelled.is_set():
            raise CancellationError("Agent run cancelled")

    def _wait_for_queue(self, q: "queue.Queue[str]") -> str:
        """Block until a value arrives on `q`, checking for cancellation."""
        while True:
            self._check_cancelled()
            try:
                return q.get(timeout=0.5)
            except queue.Empty:
                continue  # poll timeout — retry until item arrives or cancelled

    def _build_system_prompt(self) -> str:
        binary_info = None
        current_address = None
        current_function = None

        if self.config.auto_context:
            try:
                binary_info = self.tools.execute("get_binary_info", {})
            except Exception as e:
                log_debug(f"get_binary_info failed: {e}")
            try:
                current_address = self.tools.execute("get_cursor_position", {})
                current_function = self.tools.execute("get_current_function", {})
            except Exception as e:
                log_debug(f"cursor/function context failed: {e}")

        skill_summary = None
        if self.skills:
            skill_summary = self.skills.get_summary_for_prompt()

        # Derive IDB directory for persistent memory loading
        idb_dir = ""
        if self.session.idb_path:
            idb_dir = os.path.dirname(self.session.idb_path)

        return build_system_prompt(
            host_name=self.host_name,
            binary_info=binary_info,
            current_function=current_function,
            current_address=current_address,
            tool_names=self.tools.list_names(),
            skill_summary=skill_summary,
            idb_dir=idb_dir,
        )

    def _resolve_skill(self, user_message: str) -> tuple:
        """Rewrite user message if it matches a skill.

        Checks explicit /slug invocation first, then falls back to
        trigger pattern matching on the user's natural language.

        Returns (rewritten_message, skill_or_None).
        """
        if not self.skills:
            return (user_message, None)

        # 1. Explicit /slug invocation
        skill, remaining = self.skills.resolve_skill_invocation(user_message)
        if skill is not None:
            log_debug(f"AgentLoop: skill invocation /{skill.slug}")
            rewritten = (
                f"[Skill: {skill.name}]\n"
                f"{sanitize_skill_body(skill.body, skill.name)}\n\n"
                f"User request: {remaining}"
            )
            return (rewritten, skill)

        # 2. Trigger pattern matching on natural language
        skill = self.skills.match_triggers(user_message)
        if skill is not None:
            log_debug(f"AgentLoop: trigger-matched skill /{skill.slug}")
            rewritten = (
                f"[Skill: {skill.name}]\n"
                f"{sanitize_skill_body(skill.body, skill.name)}\n\n"
                f"User request: {user_message}"
            )
            return (rewritten, skill)

        return (user_message, None)

    @staticmethod
    def _parse_plan(text: str) -> List[str]:
        """Parse a numbered plan from LLM text into step strings."""
        return _parse_plan_impl(text)

    def _handle_memory_command(self) -> Generator[TurnEvent, None, None]:
        """Show current RIKUGAN.md contents in chat."""
        idb_dir = ""
        if self.session.idb_path:
            idb_dir = os.path.dirname(self.session.idb_path)
        if not idb_dir:
            yield TurnEvent.text_done("No IDB/BNDB path set — persistent memory is not available.")
            return

        md_path = os.path.join(idb_dir, "RIKUGAN.md")
        if not os.path.isfile(md_path):
            yield TurnEvent.text_done(
                f"No persistent memory file found.\n\n"
                f"A `RIKUGAN.md` file will be created in `{idb_dir}` "
                f"when the agent first uses `save_memory`."
            )
            return

        try:
            with open(md_path, "r", encoding="utf-8") as f:
                content = f.read()
            if not content.strip():
                yield TurnEvent.text_done("RIKUGAN.md exists but is empty.")
            else:
                yield TurnEvent.text_done(f"**Persistent Memory** (`{md_path}`):\n\n{content}")
        except OSError as e:
            yield TurnEvent.error_event(f"Failed to read RIKUGAN.md: {e}")

    def _handle_undo_command(self, raw_cmd: str) -> Generator[TurnEvent, None, None]:
        """Undo the last N mutations."""
        # Parse count from "/undo" or "/undo N"
        parts = raw_cmd.strip().split()
        count = 1
        if len(parts) > 1:
            try:
                count = int(parts[1])
            except ValueError:
                yield TurnEvent.error_event(f"Invalid undo count: {parts[1]}. Usage: /undo [N]")
                return

        if not self._mutation_log:
            yield TurnEvent.text_done("Nothing to undo — mutation log is empty.")
            return

        count = min(count, len(self._mutation_log))
        undone = 0
        errors = []
        for _ in range(count):
            record = self._mutation_log.pop()
            if not record.reversible:
                errors.append(f"Cannot undo: {record.description} (not reversible)")
                continue
            try:
                self.tools.execute(record.reverse_tool, record.reverse_arguments)
                undone += 1
                log_info(f"Undo: {record.description}")
            except ToolError as e:
                errors.append(f"Failed to undo {record.description}: {e}")
                log_error(f"Undo failed: {record.description}: {e}")

        parts_out = []
        if undone:
            parts_out.append(f"Undid {undone} mutation(s).")
        if errors:
            parts_out.append("\n".join(errors))
        yield TurnEvent.text_done("\n".join(parts_out) if parts_out else "Nothing undone.")

    def _handle_mcp_command(self) -> Generator[TurnEvent, None, None]:
        """Show MCP server health and status."""
        # Access the MCP manager via the tool registry's registered tools
        # We check for MCP-prefixed tools and try to reach the manager
        mcp_tools = [n for n in self.tools.list_names() if n.startswith("mcp_")]
        if not mcp_tools:
            yield TurnEvent.text_done("No MCP servers configured or connected.")
            return

        lines = ["**MCP Server Status**\n"]
        # Group tools by server prefix
        servers: Dict[str, List[str]] = {}
        for name in mcp_tools:
            # MCP tools are named mcp_<server>_<tool>
            parts = name.split("_", 2)
            server = parts[1] if len(parts) >= 3 else "unknown"
            servers.setdefault(server, []).append(name)

        for server, tools in sorted(servers.items()):
            lines.append(f"- **{server}**: {len(tools)} tools registered")

        lines.append(f"\n**Total**: {len(mcp_tools)} MCP tools available")
        yield TurnEvent.text_done("\n".join(lines))

    def _handle_doctor_command(self) -> Generator[TurnEvent, None, None]:
        """Diagnose common setup issues."""
        issues: List[str] = []
        ok: List[str] = []

        # Check provider
        if self.provider:
            ok.append(f"Provider: {self.config.provider.name} ({self.config.provider.model})")
        else:
            issues.append("No LLM provider configured")

        # Check API key
        if self.config.provider.api_key:
            ok.append("API key: configured")
        else:
            env_key = os.environ.get("ANTHROPIC_API_KEY") or os.environ.get("OPENAI_API_KEY")
            if env_key:
                ok.append("API key: from environment variable")
            else:
                issues.append("No API key configured (set in config or environment)")

        # Check tools
        tool_count = len(self.tools.list_names())
        if tool_count > 0:
            ok.append(f"Tools: {tool_count} registered")
        else:
            issues.append("No tools registered — check plugin initialization")

        # Check skills
        if self.skills:
            slugs = self.skills.list_slugs()
            ok.append(f"Skills: {len(slugs)} loaded")
        else:
            issues.append("No skill registry — skills won't be available")

        # Check context window
        ctx = self.config.provider.context_window
        if ctx >= _MIN_CONTEXT_WINDOW_TOKENS:
            ok.append(f"Context window: {ctx:,} tokens")
        else:
            issues.append(f"Context window very small: {ctx} tokens")

        # Check config validation
        config_errors = self.config.validate()
        if config_errors:
            issues.extend(f"Config: {e}" for e in config_errors)
        else:
            ok.append("Config: valid")

        # Check IDB path for persistent memory
        if self.session.idb_path:
            ok.append(f"IDB/BNDB: {self.session.idb_path}")
        else:
            issues.append("No IDB/BNDB path — persistent memory disabled")

        # Format output
        lines = ["**Rikugan Doctor**\n"]
        if ok:
            lines.append("**OK:**")
            for item in ok:
                lines.append(f"  - {item}")
        if issues:
            lines.append("\n**Issues:**")
            for item in issues:
                lines.append(f"  - {item}")
        else:
            lines.append("\nNo issues found.")
        yield TurnEvent.text_done("\n".join(lines))

    def _format_provider_error_for_user(self, error: ProviderError) -> str:
        """Return a user-facing provider error message for chat display."""
        provider = error.provider or self.config.provider.name or "provider"
        detail = str(error).strip() or "Request failed."

        if isinstance(error, RateLimitError):
            return f"{provider}: rate limit exceeded. {detail}"
        return f"{provider}: {detail}"

    def _stream_llm_turn(
        self, system_prompt: str, tools_schema: Optional[List],
        max_retries: int = 0,
    ) -> Generator[TurnEvent, None, Tuple[str, List[ToolCall], Optional[TokenUsage], Any]]:
        """Stream one LLM call, yielding events. Retries on transient errors.

        Returns ``(text, tool_calls, usage, raw_parts)`` where *raw_parts* is
        provider-specific opaque data (e.g. Gemini parts with thought_signatures)
        that should be stored on the :class:`Message` for faithful history replay.

        *max_retries* of 0 (default) reads from ``config.max_retries``.
        """
        if max_retries <= 0:
            max_retries = self.config.max_retries or 3
        silent_mode = self.config.silent_retry_mode

        last_error: Optional[Exception] = None
        for attempt in range(max_retries):
            self._check_cancelled()
            try:
                result = yield from self._stream_llm_turn_inner(system_prompt, tools_schema)
                return result
            except (RateLimitError, ProviderError) as e:
                is_rate_limit = isinstance(e, RateLimitError)
                if not is_rate_limit and not (e.retryable and attempt < max_retries - 1):
                    raise
                last_error = e
                log_error(f"Retryable error (attempt {attempt + 1}/{max_retries}): {e}")
                if attempt < max_retries - 1:
                    if is_rate_limit:
                        backoff = e.retry_after if e.retry_after > 0 else min(2 ** attempt, 10)
                    else:
                        backoff = min(2 ** attempt, 10)
                    if silent_mode:
                        yield TurnEvent.error_event(
                            f"\u23f3 Retrying in {backoff:.0f}s "
                            f"(attempt {attempt + 2}/{max_retries})..."
                        )
                    else:
                        yield TurnEvent.error_event(
                            f"{self._format_provider_error_for_user(e)} "
                            f"Retrying in {backoff:.0f}s (attempt {attempt + 2}/{max_retries})."
                        )
                    deadline = time.monotonic() + backoff
                    while time.monotonic() < deadline:
                        self._check_cancelled()
                        remaining = deadline - time.monotonic()
                        if remaining <= 0:
                            break
                        time.sleep(min(0.5, remaining))
                continue

        # All retries exhausted
        raise last_error  # type: ignore[misc]

    def _maybe_inject_error_hint(self) -> None:
        """Inject a system hint when consecutive tool errors exceed thresholds."""
        if self._consecutive_errors >= 5:
            self._tools_disabled_for_turn = True
            self._consecutive_errors = 0
            self.session.add_message(Message(
                role=Role.USER,
                content=(
                    "[SYSTEM] You have failed 5 consecutive tool calls. "
                    "Tools are temporarily disabled. Explain what went wrong "
                    "and what you were trying to do. The user may help you. "
                    "Tools will be re-enabled on your next turn."
                ),
            ))
        elif self._consecutive_errors >= 3:
            self.session.add_message(Message(
                role=Role.USER,
                content=(
                    "[SYSTEM] You have failed 3 consecutive tool calls. "
                    "Stop retrying the same approach. Try a different strategy "
                    "or explain what is failing."
                ),
            ))

    def _prepare_provider_messages(
        self, system_prompt: str
    ) -> Tuple[List, int, Optional[TokenUsage]]:
        """Estimate tokens, compact context if needed, return (provider_messages, estimated_tokens, estimated_usage)."""
        # Fast path: use running token counter to skip expensive O(n)
        # estimation when we're clearly below the compaction threshold.
        fast_estimate = self.session.token_estimate
        if fast_estimate > 0 and fast_estimate < int(self._context_manager.max_tokens * 0.5):
            # Well below threshold — skip full estimation for compaction
            pass
        else:
            # Estimate full in-memory context so compaction decisions work
            # even when provider streaming usage is missing.
            full_messages = minify_messages(
                self.session.get_messages_for_provider(context_window=0)
            )
            full_prompt_tokens = self._estimate_prompt_tokens(full_messages, system_prompt)
            if full_prompt_tokens > 0:
                self._context_manager.update_usage(
                    TokenUsage(prompt_tokens=full_prompt_tokens, total_tokens=full_prompt_tokens)
                )

        if self._context_manager.should_compact():
            log_info(
                f"Context compaction triggered (usage ratio: "
                f"{self._context_manager.usage_ratio:.1%})"
            )
            with self.session._lock:
                self.session.messages[:] = self._context_manager.compact_messages(
                    self.session.messages,
                )

        ctx_window = self.config.provider.context_window
        provider_messages = minify_messages(
            self.session.get_messages_for_provider(context_window=ctx_window)
        )
        estimated_prompt_tokens = self._estimate_prompt_tokens(provider_messages, system_prompt)
        estimated_usage: Optional[TokenUsage] = None
        if estimated_prompt_tokens > 0:
            estimated_usage = TokenUsage(
                prompt_tokens=estimated_prompt_tokens,
                total_tokens=estimated_prompt_tokens,
            )
            self._context_manager.update_usage(estimated_usage)
        return provider_messages, estimated_prompt_tokens, estimated_usage

    def _accumulate_chunk_usage(
        self, last: Optional[TokenUsage], chunk: TokenUsage
    ) -> TokenUsage:
        """Merge a streaming chunk's usage into the accumulated total."""
        if last is None:
            return TokenUsage(
                prompt_tokens=chunk.prompt_tokens,
                completion_tokens=chunk.completion_tokens,
                total_tokens=(chunk.total_tokens or chunk.prompt_tokens + chunk.completion_tokens),
                cache_read_tokens=chunk.cache_read_tokens,
                cache_creation_tokens=chunk.cache_creation_tokens,
            )
        # Accumulate: message_start sends prompt_tokens, message_delta sends completion_tokens.
        return TokenUsage(
            prompt_tokens=last.prompt_tokens + chunk.prompt_tokens,
            completion_tokens=last.completion_tokens + chunk.completion_tokens,
            total_tokens=(
                last.prompt_tokens + chunk.prompt_tokens
                + last.completion_tokens + chunk.completion_tokens
            ),
            cache_read_tokens=last.cache_read_tokens + chunk.cache_read_tokens,
            cache_creation_tokens=last.cache_creation_tokens + chunk.cache_creation_tokens,
        )

    def _finalize_stream_usage(
        self,
        last_usage: Optional[TokenUsage],
        estimated_usage: Optional[TokenUsage],
        estimated_prompt_tokens: int,
    ) -> Tuple[Optional[TokenUsage], bool]:
        """Return (finalized_usage, should_emit_update).

        Falls back to the local estimate when the provider omitted usage entirely,
        or patches in prompt_tokens when the provider only emitted completion tokens.
        """
        if last_usage is None:
            return estimated_usage, False
        if estimated_prompt_tokens > 0 and last_usage.prompt_tokens <= 0:
            merged_total = (
                last_usage.total_tokens
                if last_usage.total_tokens > 0
                else estimated_prompt_tokens + last_usage.completion_tokens
            )
            patched = TokenUsage(
                prompt_tokens=estimated_prompt_tokens,
                completion_tokens=last_usage.completion_tokens,
                total_tokens=merged_total,
                cache_read_tokens=last_usage.cache_read_tokens,
                cache_creation_tokens=last_usage.cache_creation_tokens,
            )
            return patched, True
        return last_usage, False

    def _stream_llm_turn_inner(
        self, system_prompt: str, tools_schema: Optional[List],
    ) -> Generator[TurnEvent, None, Tuple[str, List[ToolCall], Optional[TokenUsage], Any]]:
        """Stream one LLM call, yielding events (no retry logic)."""
        assistant_text_parts: List[str] = []
        tool_calls: List[ToolCall] = []
        current_tool_arg_parts: Dict[str, List[str]] = {}
        current_tool_names: Dict[str, str] = {}
        last_usage: Optional[TokenUsage] = None
        raw_parts: Any = None

        provider_messages, estimated_prompt_tokens, estimated_usage = \
            self._prepare_provider_messages(system_prompt)
        # Do not emit a pre-stream estimate — it causes the display to jump
        # to an estimated value only to be overwritten by real data moments later.

        stream = self.provider.chat_stream(
            messages=provider_messages,
            tools=tools_schema if tools_schema else None,
            temperature=self.config.provider.temperature,
            max_tokens=self.config.provider.max_tokens,
            system=system_prompt,
        )

        chunk_count = 0
        for chunk in stream:
            self._check_cancelled()
            chunk_count += 1

            if chunk.text:
                assistant_text_parts.append(chunk.text)
                yield TurnEvent.text_delta(chunk.text)

            if chunk.is_tool_call_start and chunk.tool_call_id:
                current_tool_arg_parts[chunk.tool_call_id] = []
                current_tool_names[chunk.tool_call_id] = chunk.tool_name or ""
                yield TurnEvent.tool_call_start(chunk.tool_call_id, chunk.tool_name or "")

            if chunk.tool_args_delta and chunk.tool_call_id:
                if not chunk.is_tool_call_end:
                    current_tool_arg_parts.setdefault(chunk.tool_call_id, []).append(chunk.tool_args_delta)
                    yield TurnEvent.tool_call_args_delta(chunk.tool_call_id, chunk.tool_args_delta)

            if chunk.is_tool_call_end and chunk.tool_call_id:
                tc_id = chunk.tool_call_id
                tc_name = current_tool_names.get(tc_id, chunk.tool_name or "")
                raw_args = "".join(current_tool_arg_parts.get(tc_id, []))
                try:
                    args = json.loads(raw_args) if raw_args else {}
                except json.JSONDecodeError as je:
                    log_error(f"Malformed tool arguments for {tc_name} (id={tc_id}): {je}. Raw: {raw_args[:200]}")
                    args = {}
                    yield TurnEvent.error_event(
                        f"Warning: malformed arguments for tool '{tc_name}'. "
                        "The tool call will proceed with empty arguments."
                    )
                tool_calls.append(ToolCall(id=tc_id, name=tc_name, arguments=args))
                yield TurnEvent.tool_call_done(tc_id, tc_name, raw_args)

            if chunk.usage:
                last_usage = self._accumulate_chunk_usage(last_usage, chunk.usage)
                self._context_manager.update_usage(last_usage)
                # Do not yield per-chunk updates — emit one final update after the stream

            if chunk.raw_parts is not None:
                raw_parts = chunk.raw_parts

        last_usage, need_usage_update = self._finalize_stream_usage(
            last_usage, estimated_usage, estimated_prompt_tokens
        )
        if last_usage is not None:
            if need_usage_update:
                self._context_manager.update_usage(last_usage)
            yield TurnEvent.usage_update(last_usage)

        assistant_text = "".join(assistant_text_parts)
        log_debug(f"Stream done: {chunk_count} chunks, {len(assistant_text)} chars, {len(tool_calls)} tool calls")
        return (assistant_text, tool_calls, last_usage, raw_parts)

    @staticmethod
    def _estimate_prompt_tokens(provider_messages: List[Message], system_prompt: str) -> int:
        """Estimate prompt token usage from message content lengths.

        Uses a lightweight character sum instead of JSON serialization.
        """
        char_count = len(system_prompt)
        for m in provider_messages:
            char_count += len(m.content) if m.content else 0
            if m.tool_calls:
                for tc in m.tool_calls:
                    char_count += len(str(tc.arguments)) if tc.arguments else 0
        return ContextWindowManager.estimate_tokens_from_chars(char_count)

    @staticmethod
    def _describe_tool_call(name: str, args: Dict[str, Any]) -> str:
        """Generate a brief human-readable description of what a tool will do."""
        if name == "execute_python":
            code = args.get("code", args.get("script", ""))
            lines = code.strip().splitlines()
            if len(lines) <= 3:
                return f"Run Python code:\n{code.strip()}"
            preview = "\n".join(lines[:3])
            return f"Run Python code ({len(lines)} lines):\n{preview}\n..."
        if name in ("rename_function",):
            return f"Rename function {args.get('old_name', '?')} → {args.get('new_name', '?')}"
        if name in ("rename_variable", "rename_single_variable"):
            return f"Rename variable {args.get('variable_name', '?')} → {args.get('new_name', '?')}"
        if name in ("set_comment", "set_function_comment"):
            return f"Set comment at {args.get('address', args.get('function_name', '?'))}"
        if name in ("set_type", "set_function_prototype"):
            return f"Set type at {args.get('ea', args.get('name_or_address', '?'))}"
        if name in ("nop_microcode", "nop_instructions"):
            return f"NOP instructions at {args.get('address', args.get('ea', '?'))}"
        if name in ("create_struct", "create_enum"):
            return f"Create {name.split('_')[1]} '{args.get('name', '?')}'"
        if name in ("decompile_function", "fetch_disassembly"):
            return f"Decompile/disassemble {args.get('name', args.get('address', '?'))}"
        # Generic
        summary_parts = []
        for k in ("name", "address", "ea", "target", "query"):
            if k in args:
                summary_parts.append(f"{k}={args[k]}")
                break
        return f"Call {name}({', '.join(summary_parts)})" if summary_parts else f"Call {name}"

    def _wait_for_approval(
        self, tc: ToolCall,
    ) -> Generator[TurnEvent, None, bool]:
        """Yield an approval request and wait for the user decision.

        Returns True if approved, False if denied.
        Handles 'allow_all' to skip future approval prompts for this session.
        """
        # Skip prompt if user previously chose "Always Allow"
        if self._always_allow_scripts:
            return True

        args_str = json.dumps(tc.arguments, indent=2)
        description = self._describe_tool_call(tc.name, tc.arguments)
        yield TurnEvent.tool_approval_request(tc.id, tc.name, args_str, description)

        decision = self._wait_for_queue(self._tool_approval_queue).lower()
        if decision == "allow_all":
            self._always_allow_scripts = True
            return True
        return decision == "allow"

    def _handle_exploration_report_tool(
        self, tc: ToolCall, state: "ExplorationState",
    ) -> Generator[TurnEvent, None, ToolResult]:
        """Handle the exploration_report pseudo-tool."""
        category = tc.arguments.get("category", "general")
        address_raw = tc.arguments.get("address")
        address = None
        if address_raw is not None:
            try:
                address = int(str(address_raw), 0)
            except (ValueError, TypeError) as e:
                log_debug(f"exploration_report: bad address {address_raw!r}: {e}")
        summary = tc.arguments.get("summary", "")
        evidence = tc.arguments.get("evidence", "")
        relevance = tc.arguments.get("relevance", "medium")

        state.knowledge_base.add_finding(Finding(
            category=category, address=address,
            summary=summary, evidence=evidence, relevance=relevance,
        ))
        if category == "function_purpose" and address is not None:
            func_name = tc.arguments.get("function_name", f"sub_{address:x}")
            state.knowledge_base.add_function(FunctionInfo(
                address=address, name=func_name,
                summary=summary, relevance=relevance,
            ))
        if category == "patch_result" and address is not None:
            original_hex = tc.arguments.get("original_hex", "")
            new_hex = tc.arguments.get("new_hex", "")
            try:
                original_bytes = bytes.fromhex(original_hex.replace(" ", "")) if original_hex else b""
            except ValueError:
                original_bytes = b""
            try:
                new_bytes = bytes.fromhex(new_hex.replace(" ", "")) if new_hex else b""
            except ValueError:
                new_bytes = b""
            patch_record = PatchRecord(
                address=address, original_bytes=original_bytes, new_bytes=new_bytes,
                description=summary,
                verified="verif" in evidence.lower() or "confirm" in evidence.lower(),
                verification_result=evidence,
            )
            state.patches_applied.append(patch_record)
            yield TurnEvent.patch_applied(address, summary, original_hex, new_hex)

        content = f"Finding logged: [{category}] {summary}"
        tr = ToolResult(tool_call_id=tc.id, name=tc.name, content=content, is_error=False)
        yield TurnEvent.tool_result_event(tc.id, tc.name, content, False)
        yield TurnEvent.exploration_finding(category, summary, address, relevance)
        return tr

    def _handle_phase_transition_tool(
        self, tc: ToolCall, state: "ExplorationState",
    ) -> Generator[TurnEvent, None, ToolResult]:
        """Handle the phase_transition pseudo-tool."""
        to_phase_str = tc.arguments.get("to_phase", "")
        reason = tc.arguments.get("reason", "")
        try:
            to_phase = ExplorationPhase(to_phase_str)
        except ValueError:
            content = f"Invalid phase: '{to_phase_str}'. Valid: {[p.value for p in ExplorationPhase]}"
            tr = ToolResult(tool_call_id=tc.id, name=tc.name, content=content, is_error=True)
            yield TurnEvent.tool_result_event(tc.id, tc.name, content, True)
            return tr

        allowed, deny_reason = state.can_transition_to(to_phase)
        if not allowed:
            content = f"Cannot transition to {to_phase_str}: {deny_reason}"
            tr = ToolResult(tool_call_id=tc.id, name=tc.name, content=content, is_error=True)
            yield TurnEvent.tool_result_event(tc.id, tc.name, content, True)
            return tr

        old_phase = state.phase.value
        state.transition_to(to_phase)
        content = f"Phase transition: {old_phase} → {to_phase_str}. {reason}"
        tr = ToolResult(tool_call_id=tc.id, name=tc.name, content=content, is_error=False)
        yield TurnEvent.tool_result_event(tc.id, tc.name, content, False)
        yield TurnEvent.exploration_phase_change(old_phase, to_phase_str, reason)
        return tr

    def _handle_save_memory_tool(self, tc: ToolCall) -> Generator[TurnEvent, None, ToolResult]:
        """Handle the save_memory pseudo-tool."""
        from ..core.sanitize import strip_injection_markers
        fact = strip_injection_markers(tc.arguments.get("fact", ""))
        category = tc.arguments.get("category", "general")
        if not fact:
            content = "Error: 'fact' is required."
            is_err = True
        else:
            idb_dir = os.path.dirname(self.session.idb_path) if self.session.idb_path else ""
            if not idb_dir:
                content = "Error: No IDB/BNDB path set; cannot determine where to save memory."
                is_err = True
            else:
                md_path = os.path.join(idb_dir, "RIKUGAN.md")
                try:
                    _append_to_memory_file(md_path, f"- [{category}] {fact}\n")
                    content = f"Saved to RIKUGAN.md: [{category}] {fact}"
                    is_err = False
                    log_info(f"save_memory: [{category}] {fact[:80]}")
                except OSError as e:
                    content = f"Error writing RIKUGAN.md: {e}"
                    is_err = True
        tr = ToolResult(tool_call_id=tc.id, name=tc.name, content=content, is_error=is_err)
        yield TurnEvent.tool_result_event(tc.id, tc.name, content, is_err)
        return tr

    def _handle_spawn_subagent_tool(self, tc: ToolCall) -> Generator[TurnEvent, None, ToolResult]:
        """Handle the spawn_subagent pseudo-tool."""
        task = tc.arguments.get("task", "")
        max_turns = tc.arguments.get("max_turns", 20)
        if not task:
            content = "Error: 'task' is required."
            is_err = True
        else:
            try:
                runner = SubagentRunner(
                    provider=self.provider, tool_registry=self.tools,
                    config=self.config, host_name=self.host_name,
                    skill_registry=self.skills, parent_loop=self,
                )
                raw = yield from runner.run_task(task, max_turns=max_turns)
                content = sanitize_tool_result(raw or "(Subagent produced no output)", "spawn_subagent")
                is_err = False
                # Store subagent messages separately for export
                if runner.last_session and runner.last_session.messages:
                    self.session.subagent_logs[tc.id] = list(runner.last_session.messages)
            except Exception as e:
                content = f"Subagent error: {e}"
                is_err = True
                log_error(f"spawn_subagent failed: {e}")
        tr = ToolResult(tool_call_id=tc.id, name=tc.name, content=content, is_error=is_err)
        yield TurnEvent.tool_result_event(tc.id, tc.name, content, is_err)
        return tr

    def _handle_activate_skill_tool(self, tc: ToolCall) -> Generator[TurnEvent, None, ToolResult]:
        """Handle the activate_skill pseudo-tool."""
        slug = tc.arguments.get("slug", "")
        skill = self.skills.get(slug) if self.skills else None
        if skill is None:
            content = f"Skill '{slug}' not found."
            is_err = True
        else:
            content = f"[Skill: {skill.name}]\n\n{sanitize_skill_body(skill.body, skill.name)}"
            is_err = False
            log_debug(f"Agent activated skill: /{slug}")
        tr = ToolResult(tool_call_id=tc.id, name=tc.name, content=content, is_error=is_err)
        yield TurnEvent.tool_result_event(tc.id, tc.name, content, is_err)
        return tr

    def _handle_ask_user_tool(self, tc: ToolCall) -> Generator[TurnEvent, None, ToolResult]:
        """Handle the ask_user pseudo-tool."""
        question = tc.arguments.get("question", "")
        options = tc.arguments.get("options", [])
        yield TurnEvent.user_question(question, options, tc.id)
        answer = self._wait_for_queue(self._user_answer_queue)
        content = f"User answered: {answer}"
        tr = ToolResult(tool_call_id=tc.id, name=tc.name, content=content, is_error=False)
        yield TurnEvent.tool_result_event(tc.id, tc.name, content, False)
        return tr

    def _execute_single_tool(self, tc: ToolCall) -> Generator[TurnEvent, None, ToolResult]:
        """Handle approval gating, mutation tracking, and execution of a real tool."""
        # execute_python always requires explicit approval
        if tc.name == "execute_python":
            approved = yield from self._wait_for_approval(tc)
            if not approved:
                content = "Tool execution denied by user."
                tr = ToolResult(tool_call_id=tc.id, name=tc.name, content=content, is_error=True)
                yield TurnEvent.tool_result_event(tc.id, tc.name, content, True)
                return tr

        defn = self.tools.get(tc.name)
        is_mutating = defn is not None and defn.mutating

        if is_mutating and self.config.approve_mutations:
            approved = yield from self._wait_for_approval(tc)
            if not approved:
                content = "Mutation denied by user."
                tr = ToolResult(tool_call_id=tc.id, name=tc.name, content=content, is_error=True)
                yield TurnEvent.tool_result_event(tc.id, tc.name, content, True)
                return tr

        pre_state: Dict[str, Any] = {}
        if is_mutating:
            pre_state = capture_pre_state(
                tc.name, tc.arguments,
                lambda name, args: self.tools.execute(name, args),
            )

        log_debug(f"Executing tool {tc.name}")
        try:
            result = self.tools.execute(tc.name, tc.arguments)
            is_error = False
            # Hysteresis: decrement instead of resetting so a single success
            # after several failures doesn't fully clear the counter.
            self._consecutive_errors = max(0, self._consecutive_errors - 1)
            if is_mutating:
                record = build_reverse_record(tc.name, tc.arguments, pre_state)
                if record is not None:
                    self._mutation_log.append(record)
                    log_debug(f"Mutation recorded: {record.description}")
                    yield TurnEvent.mutation_recorded(
                        tool_name=record.tool_name, description=record.description,
                        reversible=record.reversible, reverse_tool=record.reverse_tool,
                        reverse_args=record.reverse_arguments,
                    )
        except ToolError as e:
            result = f"Error: {e}"
            is_error = True
            self._consecutive_errors += 1
            log_error(f"Tool {tc.name} error: {e}")
        except Exception as e:
            result = f"Unexpected error: {e}"
            is_error = True
            self._consecutive_errors += 1
            log_error(f"Tool {tc.name} unexpected error: {e}\n{traceback.format_exc()}")

        # Sanitize tool output before it enters the conversation.
        # Error messages may contain attacker-controlled content (e.g. function
        # names), so strip injection markers even though we skip full wrapping.
        sanitized = sanitize_tool_result(result, tc.name) if not is_error else strip_injection_markers(result)
        tr = ToolResult(tool_call_id=tc.id, name=tc.name, content=sanitized, is_error=is_error)
        yield TurnEvent.tool_result_event(tc.id, tc.name, result, is_error)
        return tr

    def _execute_tool_calls(
        self, tool_calls: List[ToolCall],
    ) -> Generator[TurnEvent, None, List[ToolResult]]:
        """Execute tool calls, yielding result events. Returns ToolResult list."""
        tool_results: List[ToolResult] = []
        for tc in tool_calls:
            self._check_cancelled()
            state = self._exploration_state
            if tc.name == "exploration_report" and state is not None:
                tr = yield from self._handle_exploration_report_tool(tc, state)
            elif tc.name == "phase_transition" and state is not None:
                tr = yield from self._handle_phase_transition_tool(tc, state)
            elif tc.name == "save_memory":
                tr = yield from self._handle_save_memory_tool(tc)
            elif tc.name == "spawn_subagent":
                tr = yield from self._handle_spawn_subagent_tool(tc)
            elif tc.name == "activate_skill":
                tr = yield from self._handle_activate_skill_tool(tc)
            elif tc.name == "ask_user":
                tr = yield from self._handle_ask_user_tool(tc)
            else:
                tr = yield from self._execute_single_tool(tc)
            tool_results.append(tr)
        return tool_results

    def _build_tools_schema(self, active_skill: Any, use_exploration_mode: bool) -> list:
        """Build the full tool schema list for a run, including pseudo-tools."""
        tools_schema = list(self.tools.to_provider_format())

        # Filter to skill-allowed tools if the skill restricts them
        if active_skill and active_skill.allowed_tools:
            allowed = set(active_skill.allowed_tools)
            tools_schema = [
                t for t in tools_schema
                if t.get("function", {}).get("name") in allowed
            ]

        # activate_skill: dynamic because the slug enum depends on loaded skills
        if self.skills and self.skills.list_slugs():
            tools_schema.append({
                "type": "function",
                "function": {
                    "name": "activate_skill",
                    "description": (
                        "Load a skill's full prompt and reference material into context. "
                        "Call this when the user's request matches a skill's domain "
                        "(e.g., activate 'malware-analysis' for malware tasks, "
                        "'vuln-audit' for security audits, 'ida-scripting' or "
                        "'binja-scripting' when you need to write scripts). "
                        "The skill body will be returned so you can follow its methodology."
                    ),
                    "parameters": {
                        "type": "object",
                        "properties": {
                            "slug": {
                                "type": "string",
                                "description": "The skill slug to activate.",
                                "enum": self.skills.list_slugs(),
                            },
                        },
                        "required": ["slug"],
                    },
                },
            })

        if use_exploration_mode:
            tools_schema.append(_EXPLORATION_REPORT_SCHEMA)
            tools_schema.append(_PHASE_TRANSITION_SCHEMA)

        if self.session.idb_path:
            tools_schema.append(_SAVE_MEMORY_SCHEMA)

        tools_schema.append(_SPAWN_SUBAGENT_SCHEMA)
        tools_schema.append(_ASK_USER_SCHEMA)

        # Deduplicate — Anthropic rejects requests with duplicate tool names
        seen: set = set()
        deduped: list = []
        for t in tools_schema:
            name = t.get("function", t).get("name", "")
            if name and name not in seen:
                seen.add(name)
                deduped.append(t)
        return deduped

    def run(self, user_message: str) -> Generator[TurnEvent, None, None]:
        """Run the agent loop for a user message. Yields TurnEvents.

        This generator should be consumed from a background thread,
        while the UI reads events via the event_queue or directly iterates.
        """
        self._cancelled.clear()
        self._running = True
        self.session.is_running = True

        try:
            cmd = _parse_user_command(user_message)
            if cmd.direct_command == "/memory":
                yield from self._handle_memory_command()
                return
            if cmd.direct_command == "/undo":
                yield from self._handle_undo_command(cmd.direct_arg)
                return
            if cmd.direct_command == "/mcp":
                yield from self._handle_mcp_command()
                return
            if cmd.direct_command == "/doctor":
                yield from self._handle_doctor_command()
                return

            user_message = cmd.message
            use_plan_mode = cmd.use_plan_mode
            use_exploration_mode = cmd.use_exploration_mode
            explore_only = cmd.explore_only

            user_message, active_skill = self._resolve_skill(user_message)
            if active_skill and active_skill.mode == "exploration":
                use_exploration_mode = True
            elif active_skill and active_skill.mode == "plan":
                use_plan_mode = True

            self.session.add_message(Message(role=Role.USER, content=user_message))
            system_prompt = minify_text(self._build_system_prompt())
            tools_schema = self._build_tools_schema(active_skill, use_exploration_mode)
            log_debug(f"Agent run started: {len(tools_schema)} tools, msg={user_message[:80]!r}")

            if use_exploration_mode:
                yield from run_exploration_mode(
                    self, user_message, system_prompt, tools_schema, explore_only=explore_only,
                )
                return

            if use_plan_mode or self.plan_mode:
                yield from run_plan_mode(self, user_message, system_prompt, tools_schema, active_skill=active_skill)
                return

            yield from run_normal_loop(self, system_prompt, tools_schema)

        except CancellationError:
            yield TurnEvent.cancelled_event()
        except Exception as e:
            log_error(f"Agent loop error: {e}\n{traceback.format_exc()}")
            yield TurnEvent.error_event(str(e))
        finally:
            self._running = False
            self.session.is_running = False


_EVENT_QUEUE_MAXSIZE = 500


class BackgroundAgentRunner:
    """Runs the AgentLoop in a background thread, bridging to a bounded queue.

    When the queue is full, consecutive TEXT_DELTA events are coalesced
    into a single event instead of being dropped.
    """

    def __init__(self, agent_loop: AgentLoop):
        self.agent_loop = agent_loop
        self.event_queue: queue.Queue[Optional[TurnEvent]] = queue.Queue(
            maxsize=_EVENT_QUEUE_MAXSIZE,
        )
        self._thread: Optional[threading.Thread] = None

    def start(self, user_message: str) -> None:
        """Start the agent in a background thread."""
        self._thread = threading.Thread(
            target=self._run, args=(user_message,), daemon=True,
        )
        self._thread.start()

    def _run(self, user_message: str) -> None:
        pending_text: List[str] = []
        try:
            for event in self.agent_loop.run(user_message):
                if event.type == TurnEventType.TEXT_DELTA:
                    if self.event_queue.full():
                        # Coalesce: buffer text deltas when queue is full
                        pending_text.append(event.text)
                        continue
                    if pending_text:
                        # Flush buffered text as a single coalesced delta
                        pending_text.append(event.text)
                        event = TurnEvent.text_delta("".join(pending_text))
                        pending_text.clear()
                    self.event_queue.put(event)
                else:
                    # Flush any pending text before non-delta events
                    if pending_text:
                        coalesced = TurnEvent.text_delta("".join(pending_text))
                        pending_text.clear()
                        self.event_queue.put(coalesced)
                    self.event_queue.put(event)
        except Exception as e:
            log_error(f"BackgroundAgentRunner error: {e}\n{traceback.format_exc()}")
            if pending_text:
                try:
                    self.event_queue.put(
                        TurnEvent.text_delta("".join(pending_text)), timeout=1,
                    )
                except queue.Full:
                    pass
                pending_text.clear()
            self.event_queue.put(TurnEvent.error_event(str(e)))
        finally:
            if pending_text:
                try:
                    self.event_queue.put(
                        TurnEvent.text_delta("".join(pending_text)), timeout=1,
                    )
                except queue.Full:
                    pass
            self.event_queue.put(None)  # Sentinel

    def cancel(self) -> None:
        self.agent_loop.cancel()

    def get_event(self, timeout: float = 0.1) -> Optional[TurnEvent]:
        """Get the next event, or None if queue is empty."""
        try:
            return self.event_queue.get(timeout=timeout)
        except queue.Empty:
            return None
