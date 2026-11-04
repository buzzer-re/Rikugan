"""Subagent runner: spawn isolated AgentLoop instances with their own context."""

from __future__ import annotations

from collections.abc import Generator
from typing import Any

from ..core.config import RikuganConfig
from ..core.logging import log_debug, log_info
from ..core.types import Message, Role
from ..providers.base import LLMProvider
from ..skills.registry import SkillRegistry
from ..state.session import SessionState
from ..tools.registry import ToolRegistry
from .exploration_mode import KnowledgeBase
from .turn import TurnEvent, TurnEventType


class SubagentRunner:
    """Runs an isolated AgentLoop with its own SessionState.

    The subagent shares the same provider, tool registry, and config as
    the parent, but has a fresh session — its own message history and
    context window.  This keeps the parent's context clean from verbose
    tool output (decompilations, disassembly dumps, etc.).

    After the subagent finishes, the parent receives only a compact
    summary rather than the full exploration trace.
    """

    def __init__(
        self,
        provider: LLMProvider,
        tool_registry: ToolRegistry,
        config: RikuganConfig,
        host_name: str = "IDA Pro",
        skill_registry: SkillRegistry | None = None,
        parent_loop: Any | None = None,
        cancel_event: Any | None = None,
    ):
        self.provider = provider
        self.tools = tool_registry
        self.config = config
        self.host_name = host_name
        self.skills = skill_registry
        self._parent_loop = parent_loop
        self._cancel_event = cancel_event
        self._last_session: SessionState | None = None

    @property
    def last_session(self) -> SessionState | None:
        """The session from the most recent subagent run."""
        return self._last_session

    # Event types that must always be forwarded even in silent mode
    # (approval gates and user questions require UI interaction).
    _INTERACTIVE_EVENTS = frozenset(
        {
            TurnEventType.TOOL_APPROVAL_REQUEST,
            TurnEventType.USER_QUESTION,
        }
    )

    def run_task(
        self,
        task: str,
        max_turns: int = 20,
        system_addendum: str = "",
        silent: bool = False,
    ) -> Generator[TurnEvent, None, str]:
        """Run a general-purpose subagent task.

        Yields TurnEvents from the subagent so the UI can show progress.
        Returns the subagent's final assistant text as a string summary.

        When *silent* is True, only interactive events (tool approval,
        user questions) are forwarded — text, tool calls, and results
        are suppressed from the parent UI.

        The subagent gets a clean session and runs the task as a normal
        agent loop (not exploration mode, not plan mode).
        """
        from .loop import AgentLoop  # deferred to avoid circular import

        session = SessionState()
        # Share the parent's mutation log so a subagent's renames are undoable
        # too; its own session is discarded when it finishes.
        if self._parent_loop is not None:
            session.mutation_log = self._parent_loop.session.mutation_log
        self._last_session = session
        loop = AgentLoop(
            provider=self.provider,
            tool_registry=self.tools,
            config=self.config,
            session=session,
            skill_registry=self.skills,
            host_name=self.host_name,
            parent_loop=self._parent_loop,
        )

        log_info(f"Subagent started: task={task[:80]!r}, max_turns={max_turns}, silent={silent}")

        # Prefix the task with a turn limit instruction
        augmented_task = (
            f"[SUBAGENT TASK — max {max_turns} turns]\n"
            f"{task}\n\n"
            f"When done, provide a concise summary of your findings. "
            f"Focus on actionable results, not the process."
        )
        if system_addendum:
            augmented_task = f"{system_addendum}\n\n{augmented_task}"

        final_text = ""
        for event in loop.run(augmented_task):
            # In silent mode, only forward interactive events
            if silent:
                if event.type in self._INTERACTIVE_EVENTS:
                    yield event
            else:
                yield event

            # Capture the last text_done as the final output
            if event.type.value == "text_done" and event.text:
                final_text = event.text

        # Sync "always allow" flag back to parent
        if self._parent_loop and loop._always_allow_scripts:
            self._parent_loop._always_allow_scripts = True

        log_info(f"Subagent finished: {len(final_text)} chars output")
        return final_text

    def run_exploration(
        self,
        user_goal: str,
        max_turns: int = 30,
        idb_path: str = "",
        prior_context: str = "",
    ) -> Generator[TurnEvent, None, KnowledgeBase]:
        """Run exploration Phase 1 as an isolated subagent.

        The subagent runs in explore-only mode, accumulating findings
        in its own KnowledgeBase.  When exploration is complete, the
        KnowledgeBase is returned to the parent — NOT the raw
        decompilation/disassembly output that filled the subagent's
        context window.

        Yields TurnEvents so the UI can track progress.
        Returns the populated KnowledgeBase.
        """
        from .loop import AgentLoop  # deferred to avoid circular import

        session = SessionState()
        # Share the parent's mutation log so a subagent's renames are undoable
        # too; its own session is discarded when it finishes.
        if self._parent_loop is not None:
            session.mutation_log = self._parent_loop.session.mutation_log
        session.idb_path = idb_path
        self._last_session = session
        if prior_context:
            # An isolated context is the point — raw decompilation must not
            # follow the goal in here. But a goal is often a reply to what came
            # before ("continue", "do that one next"), and with nothing behind
            # it the subagent can only report that it does not know what was
            # meant. A digest of the conversation costs little and makes a
            # referential goal resolvable.
            session.add_message(Message(role=Role.USER, content=prior_context))

        loop = AgentLoop(
            provider=self.provider,
            tool_registry=self.tools,
            config=self.config,
            session=session,
            skill_registry=self.skills,
            host_name=self.host_name,
            parent_loop=self._parent_loop,
        )

        log_info(f"Subagent exploration started: goal={user_goal[:80]!r}, max_turns={max_turns}")

        # Run in explore-only mode via the /explore prefix
        yield from loop.run(f"/explore {user_goal}")

        # Extract the knowledge base.  _run_exploration_mode stores
        # it in _last_knowledge_base before clearing _exploration_state.
        kb = loop.last_knowledge_base
        if kb is None:
            kb = KnowledgeBase(user_goal=user_goal)
            log_debug("Subagent exploration: no knowledge base returned, using empty")

        # Sync "always allow" flag back to parent
        if self._parent_loop and loop._always_allow_scripts:
            self._parent_loop._always_allow_scripts = True

        log_info(
            f"Subagent exploration finished: "
            f"{len(kb.relevant_functions)} functions, "
            f"{len(kb.findings)} findings, "
            f"{len(kb.hypotheses)} hypotheses"
        )
        return kb
