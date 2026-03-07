"""Exploration mode runner: explore -> plan -> patch -> save."""

from __future__ import annotations

import re
from typing import TYPE_CHECKING, Generator, List, Optional

from ...core.errors import CancellationError, ProviderError, ToolError
from ...core.logging import log_error, log_info
from ...core.types import Message, Role, ToolResult
from ..exploration_mode import (
    ExplorationPhase, ExplorationState, ModificationPlan, PatchSummary,
    PlannedChange, EXPLORATION_SYSTEM_ADDENDUM, PLAN_SYNTHESIS_PROMPT,
    EXECUTE_STEP_PROMPT,
)
from ..plan_mode import parse_plan as _parse_plan_impl
from ..subagent import SubagentRunner
from ..turn import TurnEvent

if TYPE_CHECKING:
    from ..loop import AgentLoop


def _parse_plan(text: str) -> List[str]:
    return _parse_plan_impl(text)


def _run_phase1_subagent(
    loop: "AgentLoop",
    state: ExplorationState,
    user_message: str,
    exploration_system: str,
) -> Generator[TurnEvent, None, None]:
    """Run exploration Phase 1 as an isolated subagent."""
    runner = SubagentRunner(
        provider=loop.provider,
        tool_registry=loop.tools,
        config=loop.config,
        host_name=loop.host_name,
        skill_registry=loop.skills,
        parent_loop=loop,
    )

    log_info("Phase 1 running as subagent (isolated context)")
    kb = yield from runner.run_exploration(
        user_goal=user_message,
        max_turns=state.max_explore_turns,
        idb_path=loop.session.idb_path or "",
    )

    # Store exploration subagent messages for export
    if runner.last_session and runner.last_session.messages:
        log_id = f"exploration_{state.total_turns}"
        loop.session.subagent_logs[log_id] = list(runner.last_session.messages)

    # Merge subagent knowledge base into parent state
    state.knowledge_base = kb
    state.knowledge_base.user_goal = user_message

    # Inject summary into parent context (compact, not raw output)
    summary = kb.to_summary()
    if summary:
        summary_msg = Message(role=Role.USER, content=(
            "[SYSTEM] Subagent exploration complete. Summary:\n\n" + summary
        ))
        loop.session.add_message(summary_msg)

    # Transition to plan if ready
    if kb.has_minimum_for_planning:
        state.transition_to(ExplorationPhase.PLAN)
        yield TurnEvent.exploration_phase_change(
            "explore", "plan",
            "Subagent exploration complete. Moving to planning.",
        )
    else:
        yield TurnEvent.error_event(
            "Subagent exploration finished without sufficient findings. "
            f"Gap: {kb.planning_gap_description}. "
            "Try a more specific request."
        )
        loop._clear_exploration_state()


def _run_phase1_inline(
    loop: "AgentLoop",
    state: ExplorationState,
    exploration_system: str,
    tools_schema: List,
    explore_only: bool,
) -> Generator[TurnEvent, None, None]:
    """Run exploration Phase 1 inline (in the parent's context)."""
    while state.phase == ExplorationPhase.EXPLORE:
        loop._check_cancelled()
        state.explore_turns += 1
        state.total_turns += 1

        if state.explore_turns > state.max_explore_turns:
            if state.knowledge_base.has_minimum_for_planning and not explore_only:
                state.transition_to(ExplorationPhase.PLAN)
                yield TurnEvent.exploration_phase_change(
                    "explore", "plan",
                    f"Exploration turn limit reached ({state.max_explore_turns}). "
                    "Moving to planning with current findings.",
                )
                break
            else:
                yield TurnEvent.error_event(
                    f"Exploration turn limit reached ({state.max_explore_turns}) "
                    "without sufficient findings for planning. "
                    "Try a more specific request."
                )
                loop._clear_exploration_state()
                return

        yield TurnEvent.turn_start(state.total_turns)

        try:
            assistant_text, tool_calls, last_usage, raw_parts = yield from loop._stream_llm_turn(
                exploration_system, tools_schema,
            )
        except CancellationError:
            yield TurnEvent.cancelled_event()
            loop._clear_exploration_state()
            return
        except ProviderError as e:
            yield TurnEvent.error_event(loop._format_provider_error_for_user(e))
            loop._clear_exploration_state()
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
            yield TurnEvent.turn_end(state.total_turns)
            if explore_only:
                break
            if state.knowledge_base.has_minimum_for_planning:
                state.transition_to(ExplorationPhase.PLAN)
                yield TurnEvent.exploration_phase_change(
                    "explore", "plan",
                    "Agent finished exploration. Moving to planning.",
                )
            break

        tool_results: List[ToolResult] = yield from loop._execute_tool_calls(tool_calls)
        tool_msg = Message(role=Role.TOOL, tool_results=tool_results)
        loop.session.add_message(tool_msg)

        loop._maybe_inject_error_hint()

        yield TurnEvent.turn_end(state.total_turns)


def _run_phase2_plan(
    loop: "AgentLoop",
    state: ExplorationState,
    exploration_system: str,
    user_goal: str,
) -> Generator[TurnEvent, None, Optional[List[str]]]:
    """Phase 2: synthesize a modification plan from gathered findings.

    Returns the parsed step list, or None if planning failed/was rejected.
    """
    knowledge_summary = state.knowledge_base.to_summary()
    plan_prompt = PLAN_SYNTHESIS_PROMPT.format(knowledge_summary=knowledge_summary)
    loop.session.add_message(Message(role=Role.USER, content=plan_prompt))

    state.total_turns += 1
    yield TurnEvent.turn_start(state.total_turns)
    try:
        plan_text, _, usage, _ = yield from loop._stream_llm_turn(
            exploration_system, None,  # text-only, no tools
        )
    except CancellationError:
        yield TurnEvent.cancelled_event()
        return None
    except ProviderError as e:
        yield TurnEvent.error_event(loop._format_provider_error_for_user(e))
        return None

    if plan_text:
        yield TurnEvent.text_done(plan_text)
    loop.session.add_message(Message(role=Role.ASSISTANT, content=plan_text, token_usage=usage))
    yield TurnEvent.turn_end(state.total_turns)

    steps = _parse_plan(plan_text)
    if not steps:
        yield TurnEvent.error_event(
            "Failed to generate a valid modification plan from exploration findings."
        )
        return None

    yield TurnEvent.plan_generated(steps)

    # Build ModificationPlan from parsed steps
    changes: List[PlannedChange] = []
    for i, step in enumerate(steps):
        addr_match = re.search(r"0x([0-9a-fA-F]+)", step)
        addr = int(addr_match.group(1), 16) if addr_match else 0
        changes.append(PlannedChange(
            index=i, target_address=addr,
            current_behavior="", proposed_behavior=step, patch_strategy=step,
        ))
    state.modification_plan = ModificationPlan(changes=changes, rationale=plan_text)

    # User approval gate
    answer = loop._wait_for_queue(loop._user_answer_queue).strip().lower()
    while answer not in ("approve", "1", "yes", "y"):
        loop._check_cancelled()
        yield TurnEvent.user_question(
            "Modification plan rejected. Would you like to regenerate it, or type feedback for a revised plan?",
            ["Regenerate", "Cancel"],
            tool_call_id="plan_reject",
            allow_text=True,
        )
        followup = loop._wait_for_queue(loop._user_answer_queue).strip()
        if followup.lower() in ("cancel", "no", "n"):
            yield TurnEvent.error_event("Modification plan cancelled by user.")
            return None
        # Treat anything else as guidance for a new plan attempt.
        feedback = followup if followup.lower() != "regenerate" else ""
        regen_prompt = "The user rejected the previous modification plan."
        if feedback:
            regen_prompt += f" Their feedback: {feedback}"
        regen_prompt += "\n\nPlease generate a revised modification plan."
        plan_prompt = PLAN_SYNTHESIS_PROMPT.format(knowledge_summary=knowledge_summary)
        loop.session.add_message(Message(role=Role.USER, content=regen_prompt + "\n\n" + plan_prompt))

        state.total_turns += 1
        yield TurnEvent.turn_start(state.total_turns)
        try:
            plan_text, _, usage, _ = yield from loop._stream_llm_turn(
                exploration_system, None,
            )
        except CancellationError:
            yield TurnEvent.cancelled_event()
            return None
        except ProviderError as e:
            yield TurnEvent.error_event(loop._format_provider_error_for_user(e))
            return None

        if plan_text:
            yield TurnEvent.text_done(plan_text)
        loop.session.add_message(Message(role=Role.ASSISTANT, content=plan_text, token_usage=usage))
        yield TurnEvent.turn_end(state.total_turns)

        steps = _parse_plan(plan_text)
        if not steps:
            yield TurnEvent.error_event("Failed to generate a valid modification plan.")
            return None

        # Rebuild ModificationPlan
        changes = []
        for i, step in enumerate(steps):
            addr_match = re.search(r"0x([0-9a-fA-F]+)", step)
            addr = int(addr_match.group(1), 16) if addr_match else 0
            changes.append(PlannedChange(
                index=i, target_address=addr,
                current_behavior="", proposed_behavior=step, patch_strategy=step,
            ))
        state.modification_plan = ModificationPlan(changes=changes, rationale=plan_text)

        yield TurnEvent.plan_generated(steps)
        answer = loop._wait_for_queue(loop._user_answer_queue).strip().lower()

    state.transition_to(ExplorationPhase.EXECUTE)
    yield TurnEvent.exploration_phase_change("plan", "execute", "Plan approved. Executing patches.")
    from .plan import _persist_plan
    _persist_plan(loop, user_goal, steps)
    return steps


def _run_phase3_execute(
    loop: "AgentLoop",
    state: ExplorationState,
    steps: List[str],
    exploration_system: str,
    tools_schema: List,
) -> Generator[TurnEvent, None, bool]:
    """Phase 3: execute each planned patch step. Returns True if completed."""
    for i, step_desc in enumerate(steps):
        loop._check_cancelled()
        state.execute_turns += 1
        if state.execute_turns > state.max_execute_turns:
            yield TurnEvent.error_event(
                f"Execute turn limit reached ({state.max_execute_turns}). "
                "Some patches may not have been applied."
            )
            return False

        yield TurnEvent.plan_step_start(i, step_desc)
        loop.session.add_message(Message(
            role=Role.USER,
            content=EXECUTE_STEP_PROMPT.format(
                index=i + 1, total=len(steps), description=step_desc,
            ),
        ))

        # Mini agent loop for this step
        for _st in range(10):
            loop._check_cancelled()
            state.total_turns += 1
            yield TurnEvent.turn_start(state.total_turns)

            try:
                a_text, t_calls, t_usage, r_parts = yield from loop._stream_llm_turn(
                    exploration_system, tools_schema,
                )
            except CancellationError:
                yield TurnEvent.cancelled_event()
                return False
            except ProviderError as e:
                yield TurnEvent.error_event(loop._format_provider_error_for_user(e))
                return False

            if a_text:
                yield TurnEvent.text_done(a_text)
            a_msg = Message(
                role=Role.ASSISTANT, content=a_text,
                tool_calls=t_calls, token_usage=t_usage,
            )
            if r_parts is not None:
                a_msg._raw_parts = r_parts
            loop.session.add_message(a_msg)

            if not t_calls:
                yield TurnEvent.turn_end(state.total_turns)
                break

            t_results: List[ToolResult] = yield from loop._execute_tool_calls(t_calls)
            loop.session.add_message(Message(role=Role.TOOL, tool_results=t_results))
            yield TurnEvent.turn_end(state.total_turns)

        yield TurnEvent.plan_step_done(i, "completed")
    return True


def _run_phase4_save(
    loop: "AgentLoop",
    state: ExplorationState,
) -> Generator[TurnEvent, None, None]:
    """Phase 4: prompt the user to save or discard applied patches."""
    state.transition_to(ExplorationPhase.SAVE)
    yield TurnEvent.exploration_phase_change("execute", "save", "All patches applied. Awaiting save decision.")

    summary = PatchSummary(patches=list(state.patches_applied))
    summary.compute()
    patches_detail = [
        {
            "address": f"0x{p.address:x}",
            "description": p.description,
            "original": p.original_bytes.hex() if p.original_bytes else "",
            "new": p.new_bytes.hex() if p.new_bytes else "",
            "verified": p.verified,
        }
        for p in state.patches_applied
    ]
    yield TurnEvent.save_approval_request(
        patch_count=len(state.patches_applied),
        total_bytes=summary.total_bytes_modified,
        all_verified=summary.all_verified,
        patches_detail=patches_detail,
    )

    save_answer = loop._wait_for_queue(loop._user_answer_queue).strip().lower()
    if save_answer in ("save all", "save", "1", "yes", "y"):
        loop.session.add_message(Message(role=Role.USER, content=(
            "[SYSTEM] Patches are saved in the analysis database. "
            "To create a patched binary:\n"
            "- **IDA Pro**: File → Produce file → Create patched file\n"
            "- **Binary Ninja**: File → Save / Save As"
        )))
        yield TurnEvent.save_completed(len(state.patches_applied), summary.total_bytes_modified)
        log_info("Exploration mode: patches saved")
    else:
        rolled_back = False
        if state.patches_applied:
            rollback_parts = [
                (
                    f"import ida_bytes; ida_bytes.patch_bytes(0x{p.address:x}, {repr(bytes(p.original_bytes))})"
                    if loop.host_name == "IDA Pro"
                    else f"bv.write(0x{p.address:x}, {repr(bytes(p.original_bytes))})"
                )
                for p in reversed(state.patches_applied) if p.original_bytes
            ]
            if rollback_parts:
                try:
                    loop.tools.execute("execute_python", {"code": "; ".join(rollback_parts)})
                    rolled_back = True
                    log_info("Exploration mode: patches rolled back via execute_python")
                except ToolError as e:
                    log_error(f"Exploration mode: rollback failed: {e}")

        discard_msg = (
            "[SYSTEM] Patches discarded. Original bytes have been restored."
            if rolled_back
            else "[SYSTEM] Patches discarded. The in-memory changes persist "
                 "until the analysis database is reloaded without saving."
        )
        loop.session.add_message(Message(role=Role.USER, content=discard_msg))
        yield TurnEvent.save_discarded(len(state.patches_applied), rolled_back)
        log_info(f"Exploration mode: patches discarded by user (rolled_back={rolled_back})")


def run_exploration_mode(
    loop: "AgentLoop",
    user_message: str,
    system_prompt: str,
    tools_schema: List,
    explore_only: bool = False,
) -> Generator[TurnEvent, None, None]:
    """Run the agent in exploration mode: explore -> plan -> patch -> save."""
    state = ExplorationState(explore_only=explore_only)
    state.max_explore_turns = loop.config.exploration_turn_limit
    state.knowledge_base.user_goal = user_message
    loop._exploration_state = state

    exploration_system = system_prompt + EXPLORATION_SYSTEM_ADDENDUM
    log_info(f"Exploration mode started: goal={user_message[:80]!r}, explore_only={explore_only}")
    yield TurnEvent.exploration_phase_change("", "explore", f"Starting exploration: {user_message[:60]}")

    # Phase 1: EXPLORE — subagent for /modify, inline for /explore
    if not explore_only:
        yield from _run_phase1_subagent(loop, state, user_message, exploration_system)
    else:
        yield from _run_phase1_inline(loop, state, exploration_system, tools_schema, explore_only)

    if explore_only:
        summary = state.knowledge_base.to_summary()
        if summary:
            loop.session.add_message(Message(role=Role.USER, content=(
                "[SYSTEM] Exploration complete. Here is a summary of findings:\n\n" + summary
            )))
        log_info("Exploration mode finished (explore-only)")
        loop._clear_exploration_state()
        return

    # Phase 2: PLAN
    if state.phase == ExplorationPhase.PLAN:
        steps = yield from _run_phase2_plan(loop, state, exploration_system, user_message)
        if steps is None:
            loop._clear_exploration_state()
            return
    else:
        steps = []

    # Phase 3: EXECUTE
    if state.phase == ExplorationPhase.EXECUTE:
        ok = yield from _run_phase3_execute(
            loop, state, steps, exploration_system, tools_schema,
        )
        if not ok:
            loop._clear_exploration_state()
            return
        # Phase 4: SAVE
        yield from _run_phase4_save(loop, state)

    log_info("Exploration mode finished")
    loop._clear_exploration_state()
