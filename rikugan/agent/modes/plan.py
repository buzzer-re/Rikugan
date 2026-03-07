"""Plan mode runner: generate plan, get approval, execute steps."""

from __future__ import annotations

import os
import time
from typing import TYPE_CHECKING, Any, Generator, List, Optional

from ...core.errors import CancellationError, ProviderError
from ...core.logging import log_error, log_info
from ...core.sanitize import sanitize_skill_body
from ...core.types import Message, Role, ToolResult
from ..plan_mode import parse_plan as _parse_plan_impl
from ..turn import TurnEvent

if TYPE_CHECKING:
    from ..loop import AgentLoop

_PLAN_GENERATION_PROMPT = (
    "You are in PLAN MODE. Analyze the user's request and create a numbered "
    "step-by-step plan. Output ONLY the plan as a numbered list, one step per "
    "line. Do NOT execute any tools. Do NOT include commentary before or after "
    "the plan. Example format:\n"
    "1. Decompile function at 0x401000\n"
    "2. Identify string references\n"
    "3. Rename variables based on analysis\n"
)

_SKILL_PLAN_GENERATION_PROMPT = (
    "You are in PLAN MODE, triggered by the /{skill_name} skill.\n"
    "The skill's methodology is your framework — follow its phases and "
    "recommended tools as the basis for your plan.\n\n"
    "Skill guidance:\n{skill_body}\n\n"
    "Analyze the user's request within this skill's framework and create a "
    "numbered step-by-step plan. Output ONLY the plan as a numbered list, "
    "one step per line. Do NOT execute any tools. Do NOT include commentary "
    "before or after the plan."
)

_STEP_EXECUTION_PROMPT = (
    "You are executing step {index} of a plan.\n"
    "Step: {description}\n\n"
    "Execute this step using the available tools. When done, provide a brief "
    "summary of what you accomplished."
)


def _parse_plan(text: str) -> List[str]:
    return _parse_plan_impl(text)


def _execute_step(
    loop: "AgentLoop",
    step_index: int,
    step_desc: str,
    system_prompt: str,
    tools_schema: List,
) -> Generator[TurnEvent, None, None]:
    """Execute a single plan step using a mini agent loop."""
    yield TurnEvent.plan_step_start(step_index, step_desc)

    step_prompt = _STEP_EXECUTION_PROMPT.format(
        index=step_index + 1, description=step_desc,
    )
    step_msg = Message(role=Role.USER, content=step_prompt)
    loop.session.add_message(step_msg)

    max_step_turns = 20
    for _st in range(max_step_turns):
        loop._check_cancelled()
        yield TurnEvent.turn_start(_st + 1)

        try:
            assistant_text, tool_calls, last_usage, raw_parts = yield from loop._stream_llm_turn(
                system_prompt, tools_schema,
            )
        except CancellationError:
            yield TurnEvent.cancelled_event()
            return
        except ProviderError as e:
            yield TurnEvent.error_event(loop._format_provider_error_for_user(e))
            return

        if assistant_text:
            yield TurnEvent.text_done(assistant_text)

        assistant_msg = Message(
            role=Role.ASSISTANT, content=assistant_text,
            tool_calls=tool_calls, token_usage=last_usage,
        )
        if raw_parts is not None:
            assistant_msg._raw_parts = raw_parts
        loop.session.add_message(assistant_msg)

        if not tool_calls:
            yield TurnEvent.turn_end(_st + 1)
            break

        tool_results: List[ToolResult] = yield from loop._execute_tool_calls(tool_calls)
        tool_msg = Message(role=Role.TOOL, tool_results=tool_results)
        loop.session.add_message(tool_msg)
        yield TurnEvent.turn_end(_st + 1)

    yield TurnEvent.plan_step_done(step_index, "completed")


def _persist_plan(loop: "AgentLoop", user_goal: str, steps: List[str]) -> None:
    """Save an approved plan to RIKUGAN.md for cross-session reference."""
    from ..loop import _append_to_memory_file

    idb_dir = ""
    if loop.session.idb_path:
        idb_dir = os.path.dirname(loop.session.idb_path)
    if not idb_dir:
        return

    md_path = os.path.join(idb_dir, "RIKUGAN.md")
    try:
        timestamp = time.strftime("%Y-%m-%d %H:%M")
        lines = [f"\n## Plan ({timestamp})\n", f"Goal: {user_goal[:200]}\n"]
        lines += [f"{i}. {step}\n" for i, step in enumerate(steps, 1)]
        lines.append("\n")
        _append_to_memory_file(md_path, "".join(lines))
        log_info(f"Plan persisted to RIKUGAN.md ({len(steps)} steps)")
    except OSError as e:
        log_error(f"Failed to persist plan to RIKUGAN.md: {e}")


def run_plan_mode(
    loop: "AgentLoop",
    user_message: str,
    system_prompt: str,
    tools_schema: List,
    active_skill: Optional[Any] = None,
) -> Generator[TurnEvent, None, None]:
    """Run the agent in plan mode: generate plan, get approval, execute steps."""
    # Phase 1: Generate plan (text-only)
    if active_skill:
        plan_prompt = _SKILL_PLAN_GENERATION_PROMPT.format(
            skill_name=active_skill.slug,
            skill_body=sanitize_skill_body(active_skill.body, active_skill.slug),
        ) + f"\n\nUser request: {user_message}"
    else:
        plan_prompt = _PLAN_GENERATION_PROMPT + f"\n\nUser request: {user_message}"
    plan_msg = Message(role=Role.USER, content=plan_prompt)
    loop.session.add_message(plan_msg)

    yield TurnEvent.turn_start(1)
    try:
        plan_text, _, usage, _ = yield from loop._stream_llm_turn(system_prompt, None)
    except CancellationError:
        yield TurnEvent.cancelled_event()
        return
    except ProviderError as e:
        yield TurnEvent.error_event(loop._format_provider_error_for_user(e))
        return

    if plan_text:
        yield TurnEvent.text_done(plan_text)

    plan_msg_resp = Message(role=Role.ASSISTANT, content=plan_text, token_usage=usage)
    loop.session.add_message(plan_msg_resp)
    yield TurnEvent.turn_end(1)

    steps = _parse_plan(plan_text)
    if not steps:
        yield TurnEvent.error_event("Failed to generate a valid plan.")
        return

    yield TurnEvent.plan_generated(steps)

    # Phase 2: Wait for user approval — PlanView buttons handle the UI.
    # On rejection, ask whether to regenerate or abort.
    answer = loop._wait_for_queue(loop._user_answer_queue).strip().lower()
    while answer not in ("approve", "1", "yes", "y"):
        loop._check_cancelled()
        yield TurnEvent.user_question(
            "Plan rejected. Would you like to regenerate it, or type feedback for a revised plan?",
            ["Regenerate", "Cancel"],
            tool_call_id="plan_reject",
            allow_text=True,
        )
        followup = loop._wait_for_queue(loop._user_answer_queue).strip()
        if followup.lower() in ("cancel", "no", "n"):
            yield TurnEvent.error_event("Plan cancelled by user.")
            return
        # Treat anything else (including "Regenerate" or free-text feedback)
        # as guidance for a new plan attempt.
        feedback = followup if followup.lower() != "regenerate" else ""
        regen_prompt = "The user rejected the previous plan."
        if feedback:
            regen_prompt += f" Their feedback: {feedback}"
        regen_prompt += "\n\nPlease generate a revised plan."
        loop.session.add_message(Message(role=Role.USER, content=regen_prompt))

        yield TurnEvent.turn_start(1)
        try:
            plan_text, _, usage, _ = yield from loop._stream_llm_turn(system_prompt, None)
        except CancellationError:
            yield TurnEvent.cancelled_event()
            return
        except ProviderError as e:
            yield TurnEvent.error_event(loop._format_provider_error_for_user(e))
            return

        if plan_text:
            yield TurnEvent.text_done(plan_text)
        loop.session.add_message(Message(role=Role.ASSISTANT, content=plan_text, token_usage=usage))
        yield TurnEvent.turn_end(1)

        steps = _parse_plan(plan_text)
        if not steps:
            yield TurnEvent.error_event("Failed to generate a valid plan.")
            return

        yield TurnEvent.plan_generated(steps)
        answer = loop._wait_for_queue(loop._user_answer_queue).strip().lower()

    # Phase 3: Execute each step
    for i, step_desc in enumerate(steps):
        loop._check_cancelled()
        yield from _execute_step(loop, i, step_desc, system_prompt, tools_schema)
