"""Session state management."""

from __future__ import annotations

import json
import threading
import time
import uuid
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set

from ..core.logging import log_debug
from ..core.types import Message, Role, ToolResult, TokenUsage

# ---------- Token estimation ----------

_CHARS_PER_TOKEN = 3.5

# Tool results older than this many messages from the end get truncated
_OLD_RESULT_THRESHOLD = 8
_OLD_RESULT_MAX_CHARS = 500
_RECENT_RESULT_MAX_CHARS = 8000


def _estimate_tokens(msg: Message) -> int:
    """Rough token count estimate from message text content."""
    chars = len(msg.content or "")
    for tc in msg.tool_calls:
        chars += len(tc.name) + 50
        if tc.arguments:
            try:
                chars += len(json.dumps(tc.arguments))
            except (TypeError, ValueError):
                chars += 100
    for tr in msg.tool_results:
        chars += len(tr.content or "") + len(tr.name or "") + 20
    return max(1, int(chars / _CHARS_PER_TOKEN))


def _truncate_tool_result(tr: ToolResult, max_chars: int) -> ToolResult:
    """Return a truncated copy of a tool result if it exceeds max_chars."""
    if not tr.content or len(tr.content) <= max_chars:
        return tr
    omitted = len(tr.content) - max_chars
    return ToolResult(
        tool_call_id=tr.tool_call_id,
        name=tr.name,
        content=tr.content[:max_chars] + f"\n... [{omitted} chars omitted]",
        is_error=tr.is_error,
    )


@dataclass
class SessionState:
    """Holds the state of one Rikugan conversation session.

    Thread-safety: all mutations to ``messages`` are guarded by ``_lock``.
    Readers that need a consistent snapshot should also hold the lock.
    """

    id: str = field(default_factory=lambda: uuid.uuid4().hex[:12])
    created_at: float = field(default_factory=time.time)
    messages: List[Message] = field(default_factory=list)
    total_usage: TokenUsage = field(default_factory=TokenUsage)
    last_prompt_tokens: int = 0
    current_turn: int = 0
    is_running: bool = False
    provider_name: str = ""
    model_name: str = ""
    idb_path: str = ""
    db_instance_id: str = ""
    metadata: Dict[str, str] = field(default_factory=dict)
    # Subagent message logs, keyed by the spawn_subagent tool_call_id.
    # Stored separately from main messages to avoid burning context tokens.
    subagent_logs: Dict[str, List[Message]] = field(default_factory=dict)

    def __post_init__(self) -> None:
        self._lock = threading.Lock()

    def add_message(self, msg: Message) -> None:
        with self._lock:
            self.messages.append(msg)
            if msg.token_usage:
                self.total_usage.prompt_tokens += msg.token_usage.prompt_tokens
                self.total_usage.completion_tokens += msg.token_usage.completion_tokens
                self.total_usage.total_tokens += msg.token_usage.total_tokens
                # Track the last prompt size as current context usage
                if msg.token_usage.prompt_tokens > 0:
                    self.last_prompt_tokens = msg.token_usage.context_tokens

    def clear(self) -> None:
        with self._lock:
            self.messages.clear()
            self.total_usage = TokenUsage()
            self.last_prompt_tokens = 0
            self.current_turn = 0
            self.is_running = False

    def prune_messages(self, keep_last_n: int = 50) -> int:
        """Drop old messages in place, preserving the system prompt + last N.

        Returns the number of messages removed.
        """
        with self._lock:
            if len(self.messages) <= keep_last_n + 1:
                return 0

            # Keep messages[0] (system prompt / first user message) + tail
            head = self.messages[:1]
            tail = self.messages[-keep_last_n:]
            removed = len(self.messages) - len(head) - len(tail)
            self.messages[:] = head + tail
            return removed

    def get_messages_for_provider(self, context_window: int = 0) -> List[Message]:
        """Return messages sanitized and trimmed for the provider API.

        1. Ensures every tool_use has a matching tool_result.
        2. Truncates old / large tool results.
        3. Drops oldest messages if the estimated token count exceeds
           the context window budget.
        """
        with self._lock:
            snapshot = list(self.messages)
        sanitized = self._sanitize(snapshot)
        sanitized = self._truncate_results(sanitized)
        if context_window > 0:
            sanitized = self._trim_to_budget(sanitized, context_window)
        return sanitized

    # --- Internal helpers ---

    @staticmethod
    def _sanitize(msgs: List[Message]) -> List[Message]:
        """Patch orphaned tool_use blocks with synthetic error results."""
        sanitized: List[Message] = []
        i = 0
        while i < len(msgs):
            msg = msgs[i]
            if msg.role == Role.ASSISTANT and msg.tool_calls:
                sanitized.append(msg)
                i += 1
                needed_ids: Set[str] = {tc.id for tc in msg.tool_calls}
                if i < len(msgs) and msgs[i].role == Role.TOOL:
                    tool_msg = msgs[i]
                    found_ids = {tr.tool_call_id for tr in tool_msg.tool_results}
                    missing = needed_ids - found_ids
                    if missing:
                        log_debug(
                            f"Sanitize: patching {len(missing)} orphaned tool_use(s): "
                            f"{', '.join(missing)}"
                        )
                        patched_results = list(tool_msg.tool_results)
                        for tc in msg.tool_calls:
                            if tc.id in missing:
                                patched_results.append(ToolResult(
                                    tool_call_id=tc.id, name=tc.name,
                                    content="Cancelled.", is_error=True,
                                ))
                        sanitized.append(Message(
                            role=Role.TOOL, tool_results=patched_results,
                        ))
                    else:
                        sanitized.append(tool_msg)
                    i += 1
                else:
                    log_debug(
                        f"Sanitize: no tool_result message for "
                        f"{len(msg.tool_calls)} tool_use(s), inserting stubs"
                    )
                    stubs = [
                        ToolResult(
                            tool_call_id=tc.id, name=tc.name,
                            content="Cancelled.", is_error=True,
                        )
                        for tc in msg.tool_calls
                    ]
                    sanitized.append(Message(role=Role.TOOL, tool_results=stubs))
            else:
                sanitized.append(msg)
                i += 1
        return sanitized

    @staticmethod
    def _truncate_results(messages: List[Message]) -> List[Message]:
        """Truncate tool results — aggressively for old, moderately for recent."""
        n = len(messages)
        result: List[Message] = []
        for idx, msg in enumerate(messages):
            if msg.role != Role.TOOL or not msg.tool_results:
                result.append(msg)
                continue
            age = n - idx
            max_chars = _OLD_RESULT_MAX_CHARS if age > _OLD_RESULT_THRESHOLD else _RECENT_RESULT_MAX_CHARS
            new_results = [_truncate_tool_result(tr, max_chars) for tr in msg.tool_results]
            result.append(Message(role=Role.TOOL, tool_results=new_results))
        return result

    @staticmethod
    def _trim_to_budget(messages: List[Message], context_window: int) -> List[Message]:
        """Drop oldest messages if estimated tokens exceed context budget."""
        # Reserve 25% for system prompt + new completion
        budget = int(context_window * 0.75)
        total = sum(_estimate_tokens(m) for m in messages)

        if total <= budget:
            return messages

        # Drop messages from the front, keeping at least the last 4
        result = list(messages)
        while total > budget and len(result) > 4:
            removed = result.pop(0)
            total -= _estimate_tokens(removed)
            # If we removed a USER msg, also drop the following assistant+tool
            # to keep message pairs coherent
            while result and result[0].role != Role.USER and len(result) > 4:
                removed = result.pop(0)
                total -= _estimate_tokens(removed)

        return result

    def message_count(self) -> int:
        with self._lock:
            return len(self.messages)
