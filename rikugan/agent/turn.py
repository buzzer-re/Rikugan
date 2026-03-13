"""Turn event types emitted by the agent loop."""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from typing import Any

from ..core.types import TokenUsage


class TurnEventType(str, Enum):
    TEXT_DELTA = "text_delta"
    TEXT_DONE = "text_done"
    TOOL_CALL_START = "tool_call_start"
    TOOL_CALL_ARGS_DELTA = "tool_call_args_delta"
    TOOL_CALL_DONE = "tool_call_done"
    TOOL_RESULT = "tool_result"
    TURN_START = "turn_start"
    TURN_END = "turn_end"
    ERROR = "error"
    CANCELLED = "cancelled"
    USAGE_UPDATE = "usage_update"
    USER_QUESTION = "user_question"
    PLAN_GENERATED = "plan_generated"
    PLAN_STEP_START = "plan_step_start"
    PLAN_STEP_DONE = "plan_step_done"
    TOOL_APPROVAL_REQUEST = "tool_approval_request"
    EXPLORATION_PHASE_CHANGE = "exploration_phase_change"
    EXPLORATION_FINDING = "exploration_finding"
    PATCH_APPLIED = "patch_applied"
    PATCH_VERIFIED = "patch_verified"
    SAVE_APPROVAL_REQUEST = "save_approval_request"
    SAVE_COMPLETED = "save_completed"
    SAVE_DISCARDED = "save_discarded"
    MUTATION_RECORDED = "mutation_recorded"
    SUBAGENT_SPAWNED = "subagent_spawned"
    SUBAGENT_PROGRESS = "subagent_progress"
    SUBAGENT_COMPLETED = "subagent_completed"
    SUBAGENT_FAILED = "subagent_failed"


@dataclass
class TurnEvent:
    type: TurnEventType
    text: str = ""
    tool_call_id: str = ""
    tool_name: str = ""
    tool_args: str = ""
    tool_result: str = ""
    tool_is_error: bool = False
    error: str | None = None
    usage: TokenUsage | None = None
    turn_number: int = 0
    plan_steps: list[str] | None = None
    plan_step_index: int = 0
    metadata: dict[str, Any] = field(default_factory=dict)

    @staticmethod
    def text_delta(text: str) -> TurnEvent:
        return TurnEvent(type=TurnEventType.TEXT_DELTA, text=text)

    @staticmethod
    def text_done(full_text: str) -> TurnEvent:
        return TurnEvent(type=TurnEventType.TEXT_DONE, text=full_text)

    @staticmethod
    def tool_call_start(tool_call_id: str, tool_name: str) -> TurnEvent:
        return TurnEvent(
            type=TurnEventType.TOOL_CALL_START,
            tool_call_id=tool_call_id,
            tool_name=tool_name,
        )

    @staticmethod
    def tool_call_args_delta(tool_call_id: str, delta: str) -> TurnEvent:
        return TurnEvent(
            type=TurnEventType.TOOL_CALL_ARGS_DELTA,
            tool_call_id=tool_call_id,
            tool_args=delta,
        )

    @staticmethod
    def tool_call_done(tool_call_id: str, tool_name: str, args: str) -> TurnEvent:
        return TurnEvent(
            type=TurnEventType.TOOL_CALL_DONE,
            tool_call_id=tool_call_id,
            tool_name=tool_name,
            tool_args=args,
        )

    @staticmethod
    def tool_result_event(
        tool_call_id: str,
        tool_name: str,
        result: str,
        is_error: bool = False,
    ) -> TurnEvent:
        return TurnEvent(
            type=TurnEventType.TOOL_RESULT,
            tool_call_id=tool_call_id,
            tool_name=tool_name,
            tool_result=result,
            tool_is_error=is_error,
        )

    @staticmethod
    def turn_start(turn_number: int) -> TurnEvent:
        return TurnEvent(type=TurnEventType.TURN_START, turn_number=turn_number)

    @staticmethod
    def turn_end(turn_number: int) -> TurnEvent:
        return TurnEvent(type=TurnEventType.TURN_END, turn_number=turn_number)

    @staticmethod
    def error_event(error: str) -> TurnEvent:
        return TurnEvent(type=TurnEventType.ERROR, error=error)

    @staticmethod
    def cancelled_event() -> TurnEvent:
        return TurnEvent(type=TurnEventType.CANCELLED)

    @staticmethod
    def usage_update(usage: TokenUsage) -> TurnEvent:
        return TurnEvent(type=TurnEventType.USAGE_UPDATE, usage=usage)

    @staticmethod
    def user_question(
        question: str,
        options: list[str] | None,
        tool_call_id: str,
        allow_text: bool = False,
    ) -> TurnEvent:
        return TurnEvent(
            type=TurnEventType.USER_QUESTION,
            text=question,
            tool_call_id=tool_call_id,
            metadata={"options": options or [], "allow_text": allow_text},
        )

    @staticmethod
    def plan_generated(steps: list[str]) -> TurnEvent:
        return TurnEvent(type=TurnEventType.PLAN_GENERATED, plan_steps=steps)

    @staticmethod
    def plan_step_start(index: int, description: str) -> TurnEvent:
        return TurnEvent(
            type=TurnEventType.PLAN_STEP_START,
            plan_step_index=index,
            text=description,
        )

    @staticmethod
    def plan_step_done(index: int, result: str) -> TurnEvent:
        return TurnEvent(
            type=TurnEventType.PLAN_STEP_DONE,
            plan_step_index=index,
            text=result,
        )

    @staticmethod
    def tool_approval_request(
        tool_call_id: str,
        tool_name: str,
        args: str,
        description: str,
    ) -> TurnEvent:
        return TurnEvent(
            type=TurnEventType.TOOL_APPROVAL_REQUEST,
            tool_call_id=tool_call_id,
            tool_name=tool_name,
            tool_args=args,
            text=description,
        )

    @staticmethod
    def exploration_phase_change(
        from_phase: str,
        to_phase: str,
        reason: str = "",
    ) -> TurnEvent:
        return TurnEvent(
            type=TurnEventType.EXPLORATION_PHASE_CHANGE,
            text=reason,
            metadata={"from_phase": from_phase, "to_phase": to_phase},
        )

    @staticmethod
    def exploration_finding(
        category: str,
        summary: str,
        address: int | None = None,
        relevance: str = "medium",
    ) -> TurnEvent:
        return TurnEvent(
            type=TurnEventType.EXPLORATION_FINDING,
            text=summary,
            metadata={
                "category": category,
                "address": f"0x{address:x}" if address is not None else None,
                "relevance": relevance,
            },
        )

    @staticmethod
    def patch_applied(
        address: int,
        description: str,
        original_hex: str,
        new_hex: str,
    ) -> TurnEvent:
        return TurnEvent(
            type=TurnEventType.PATCH_APPLIED,
            text=description,
            metadata={
                "address": f"0x{address:x}",
                "original": original_hex,
                "new": new_hex,
            },
        )

    @staticmethod
    def patch_verified(address: int, success: bool, result: str) -> TurnEvent:
        return TurnEvent(
            type=TurnEventType.PATCH_VERIFIED,
            text=result,
            metadata={
                "address": f"0x{address:x}",
                "success": success,
            },
        )

    @staticmethod
    def save_approval_request(
        patch_count: int,
        total_bytes: int,
        all_verified: bool,
        patches_detail: list[dict[str, Any]] | None = None,
    ) -> TurnEvent:
        return TurnEvent(
            type=TurnEventType.SAVE_APPROVAL_REQUEST,
            text=f"{patch_count} patches ready ({total_bytes} bytes modified)",
            metadata={
                "patch_count": patch_count,
                "total_bytes": total_bytes,
                "all_verified": all_verified,
                "patches": patches_detail or [],
            },
        )

    @staticmethod
    def save_completed(patch_count: int, total_bytes: int) -> TurnEvent:
        return TurnEvent(
            type=TurnEventType.SAVE_COMPLETED,
            text=f"Saved {patch_count} patches ({total_bytes} bytes)",
            metadata={"patch_count": patch_count, "total_bytes": total_bytes},
        )

    @staticmethod
    def save_discarded(patch_count: int, rolled_back: bool) -> TurnEvent:
        return TurnEvent(
            type=TurnEventType.SAVE_DISCARDED,
            text=f"Discarded {patch_count} patches"
            + (" (original bytes restored)" if rolled_back else " (in-memory changes persist)"),
            metadata={"patch_count": patch_count, "rolled_back": rolled_back},
        )

    @staticmethod
    def mutation_recorded(
        tool_name: str,
        description: str,
        reversible: bool,
        reverse_tool: str = "",
        reverse_args: dict[str, Any] | None = None,
    ) -> TurnEvent:
        return TurnEvent(
            type=TurnEventType.MUTATION_RECORDED,
            tool_name=tool_name,
            text=description,
            metadata={
                "reversible": reversible,
                "reverse_tool": reverse_tool,
                "reverse_args": reverse_args or {},
            },
        )

    @staticmethod
    def subagent_spawned(
        agent_id: str,
        name: str,
        agent_type: str,
        task: str,
    ) -> TurnEvent:
        return TurnEvent(
            type=TurnEventType.SUBAGENT_SPAWNED,
            text=name,
            metadata={
                "agent_id": agent_id,
                "agent_type": agent_type,
                "task": task,
            },
        )

    @staticmethod
    def subagent_progress(
        agent_id: str,
        turn_count: int,
        text: str = "",
    ) -> TurnEvent:
        return TurnEvent(
            type=TurnEventType.SUBAGENT_PROGRESS,
            text=text,
            metadata={
                "agent_id": agent_id,
                "turn_count": turn_count,
            },
        )

    @staticmethod
    def subagent_completed(
        agent_id: str,
        name: str,
        summary: str,
        turn_count: int = 0,
        elapsed: float = 0.0,
    ) -> TurnEvent:
        return TurnEvent(
            type=TurnEventType.SUBAGENT_COMPLETED,
            text=summary,
            metadata={
                "agent_id": agent_id,
                "name": name,
                "turn_count": turn_count,
                "elapsed": elapsed,
            },
        )

    @staticmethod
    def subagent_failed(
        agent_id: str,
        name: str,
        error: str,
    ) -> TurnEvent:
        return TurnEvent(
            type=TurnEventType.SUBAGENT_FAILED,
            error=error,
            metadata={
                "agent_id": agent_id,
                "name": name,
            },
        )
