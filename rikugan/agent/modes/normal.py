"""Normal mode runner: standard tool-use loop."""

from __future__ import annotations

from typing import TYPE_CHECKING, Generator, List

from ...core.errors import CancellationError, ProviderError
from ...core.logging import log_debug, log_error
from ...core.types import Message, Role, ToolResult
from ..turn import TurnEvent

if TYPE_CHECKING:
    from ..loop import AgentLoop


def run_normal_loop(
    loop: "AgentLoop",
    system_prompt: str,
    tools_schema: list,
) -> Generator[TurnEvent, None, None]:
    """Run the standard agentic while loop (non-plan, non-exploration)."""
    max_turns = 100
    turn = 0
    while True:
        loop._check_cancelled()
        turn += 1
        if turn > max_turns:
            yield TurnEvent.error_event(f"Reached max turns limit ({max_turns}).")
            break
        loop.session.current_turn = turn
        log_debug(f"Turn {turn} start")
        yield TurnEvent.turn_start(turn)

        # If tools were disabled due to consecutive errors, force text-only
        turn_tools = None if loop._tools_disabled_for_turn else tools_schema
        loop._tools_disabled_for_turn = False

        try:
            assistant_text, tool_calls, last_usage, raw_parts = yield from loop._stream_llm_turn(
                system_prompt, turn_tools,
            )
        except CancellationError:
            yield TurnEvent.cancelled_event()
            return
        except ProviderError as e:
            log_error(f"Provider error: {e}")
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
            loop._consecutive_errors = 0
            log_debug(f"Turn {turn} end (final)")
            yield TurnEvent.turn_end(turn)
            break

        tool_results: List[ToolResult] = yield from loop._execute_tool_calls(tool_calls)
        loop.session.add_message(Message(role=Role.TOOL, tool_results=tool_results))

        # Consecutive error recovery: hint at 3, disable tools at 5
        loop._maybe_inject_error_hint()

        log_debug(f"Turn {turn} end ({len(tool_calls)} tool calls)")
        yield TurnEvent.turn_end(turn)
