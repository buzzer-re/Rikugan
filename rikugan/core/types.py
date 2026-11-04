"""Core data types for Rikugan."""

from __future__ import annotations

import time
import uuid
from dataclasses import dataclass, field
from enum import Enum
from typing import Any


class Role(str, Enum):
    SYSTEM = "system"
    USER = "user"
    ASSISTANT = "assistant"
    TOOL = "tool"


@dataclass
class ToolCall:
    id: str
    name: str
    arguments: dict[str, Any]

    @staticmethod
    def make_id() -> str:
        return f"call_{uuid.uuid4().hex[:24]}"


@dataclass
class ToolResult:
    tool_call_id: str
    name: str
    content: str
    is_error: bool = False


@dataclass
class TokenUsage:
    prompt_tokens: int = 0
    completion_tokens: int = 0
    total_tokens: int = 0
    cache_read_tokens: int = 0
    cache_creation_tokens: int = 0

    @property
    def context_tokens(self) -> int:
        """Total tokens occupying the context window (including cache hits/writes)."""
        return self.prompt_tokens + self.cache_read_tokens + self.cache_creation_tokens


@dataclass
class Message:
    role: Role
    content: str = ""
    tool_calls: list[ToolCall] = field(default_factory=list)
    tool_results: list[ToolResult] = field(default_factory=list)
    tool_call_id: str | None = None
    name: str | None = None
    timestamp: float = field(default_factory=time.time)
    token_usage: TokenUsage | None = None
    id: str = field(default_factory=lambda: uuid.uuid4().hex[:12])
    metadata: dict[str, Any] = field(default_factory=dict)
    # Provider-specific raw response data (e.g. Gemini parts with thought_signatures).
    # Not serialized to JSON — only kept in-memory for the current session.
    _raw_parts: Any = field(default=None, repr=False)

    def to_dict(self) -> dict[str, Any]:
        d: dict[str, Any] = {
            "role": self.role.value,
            "id": self.id,
            "timestamp": self.timestamp,
        }
        if self.content:
            d["content"] = self.content
        if self.tool_calls:
            d["tool_calls"] = [{"id": tc.id, "name": tc.name, "arguments": tc.arguments} for tc in self.tool_calls]
        if self.tool_call_id:
            d["tool_call_id"] = self.tool_call_id
        if self.name:
            d["name"] = self.name
        if self.tool_results:
            d["tool_results"] = [
                {
                    "tool_call_id": tr.tool_call_id,
                    "name": tr.name,
                    "content": tr.content,
                    "is_error": tr.is_error,
                }
                for tr in self.tool_results
            ]
        if self.token_usage:
            d["token_usage"] = {
                "prompt_tokens": self.token_usage.prompt_tokens,
                "completion_tokens": self.token_usage.completion_tokens,
                "total_tokens": self.token_usage.total_tokens,
                "cache_read_tokens": self.token_usage.cache_read_tokens,
                "cache_creation_tokens": self.token_usage.cache_creation_tokens,
            }
        if self.metadata:
            d["metadata"] = self.metadata
        return d

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> Message:
        tool_calls = [
            ToolCall(id=tc["id"], name=tc["name"], arguments=tc["arguments"]) for tc in d.get("tool_calls", [])
        ]
        tool_results = [
            ToolResult(
                tool_call_id=tr["tool_call_id"],
                name=tr["name"],
                content=tr["content"],
                is_error=tr.get("is_error", False),
            )
            for tr in d.get("tool_results", [])
        ]
        usage = None
        if "token_usage" in d:
            u = d["token_usage"]
            usage = TokenUsage(
                prompt_tokens=u.get("prompt_tokens", 0),
                completion_tokens=u.get("completion_tokens", 0),
                total_tokens=u.get("total_tokens", 0),
                cache_read_tokens=u.get("cache_read_tokens", 0),
                cache_creation_tokens=u.get("cache_creation_tokens", 0),
            )
        return cls(
            role=Role(d["role"]),
            content=d.get("content", ""),
            tool_calls=tool_calls,
            tool_results=tool_results,
            tool_call_id=d.get("tool_call_id"),
            name=d.get("name"),
            timestamp=d.get("timestamp", time.time()),
            token_usage=usage,
            id=d.get("id", uuid.uuid4().hex[:12]),
            metadata=dict(d.get("metadata", {})),
        )


@dataclass
class ProviderCapabilities:
    streaming: bool = True
    tool_use: bool = True
    vision: bool = False
    max_context_window: int = 128000
    max_output_tokens: int = 4096
    supports_system_prompt: bool = True
    supports_cache_control: bool = False


@dataclass
class ModelInfo:
    id: str
    name: str
    provider: str
    context_window: int = 128000
    max_output_tokens: int = 4096
    supports_tools: bool = True
    supports_vision: bool = False


@dataclass
class StreamChunk:
    """A single chunk from a streaming response."""

    text: str = ""
    tool_call_id: str | None = None
    tool_name: str | None = None
    tool_args_delta: str = ""
    finish_reason: str | None = None
    usage: TokenUsage | None = None
    is_tool_call_start: bool = False
    is_tool_call_end: bool = False
    # Provider-specific raw response parts (e.g. Gemini parts with thought_signatures).
    raw_parts: Any = None


# ---------------------------------------------------------------------------
# User approval / decision protocol
# ---------------------------------------------------------------------------


class UserDecision(str, Enum):
    """Typed decisions for approval and save flows."""

    APPROVE = "approve"
    CANCEL = "cancel"
    REGENERATE = "regenerate"
    SAVE = "save"
    DISCARD = "discard"
    FEEDBACK = "feedback"


@dataclass
class UserAnswer:
    """Parsed user answer with optional free-text feedback."""

    decision: UserDecision
    feedback: str = ""


_APPROVE_WORDS = frozenset({"approve", "1", "yes", "y"})
_CANCEL_WORDS = frozenset({"cancel", "no", "n"})
_SAVE_WORDS = frozenset({"save all", "save", "1", "yes", "y"})


def parse_approval(raw: str) -> UserAnswer:
    """Parse a raw user string into an approval decision.

    Used for plan approval and modification-plan approval flows.
    """
    text = raw.strip().lower()
    if text in _APPROVE_WORDS:
        return UserAnswer(UserDecision.APPROVE)
    if text in _CANCEL_WORDS:
        return UserAnswer(UserDecision.CANCEL)
    if text == "regenerate":
        return UserAnswer(UserDecision.REGENERATE)
    return UserAnswer(UserDecision.FEEDBACK, feedback=raw.strip())


def parse_save_decision(raw: str) -> UserAnswer:
    """Parse a raw user string into a save/discard decision.

    Used for the exploration mode save phase.
    """
    text = raw.strip().lower()
    if text in _SAVE_WORDS:
        return UserAnswer(UserDecision.SAVE)
    return UserAnswer(UserDecision.DISCARD)


@dataclass
class MutationRecord:
    """A single mutation, and how to put it back.

    Lives here rather than in ``agent.mutation`` so it can be held by
    ``SessionState`` without the state layer importing the agent, and so it
    falls under the type checker.
    """

    tool_name: str
    arguments: dict[str, Any]
    reverse_tool: str
    reverse_arguments: dict[str, Any]
    timestamp: float = field(default_factory=time.time)
    description: str = ""
    reversible: bool = True
    # Why the reverse could not be worked out, when it could not be. Shown on
    # the mutation row so the user learns at the time, not at undo time.
    irreversible_reason: str = ""
