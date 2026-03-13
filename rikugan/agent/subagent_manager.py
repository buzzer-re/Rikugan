"""Subagent manager: orchestrate multiple concurrent subagent instances."""

from __future__ import annotations

import queue
import threading
import time
import uuid
from dataclasses import dataclass, field
from enum import Enum

from ..core.config import RikuganConfig
from ..core.logging import log_error, log_info
from ..core.types import TokenUsage
from ..providers.base import LLMProvider
from ..skills.registry import SkillRegistry
from ..tools.registry import ToolRegistry
from .turn import TurnEvent


class SubagentStatus(str, Enum):
    PENDING = "pending"
    RUNNING = "running"
    COMPLETED = "completed"
    FAILED = "failed"
    CANCELLED = "cancelled"


@dataclass
class SubagentInfo:
    """Metadata and state for a single subagent instance."""

    id: str
    name: str
    task: str
    agent_type: str  # "custom" | "network_recon" | "report_writer"
    status: SubagentStatus
    created_at: float
    completed_at: float | None = None
    parent_id: str | None = None
    children: list[str] = field(default_factory=list)
    summary: str = ""
    turn_count: int = 0
    token_usage: TokenUsage | None = None
    perks: list[str] = field(default_factory=list)
    category: str = ""  # "bulk_rename", "" (general), etc.


class SubagentManager:
    """Registry and executor of all subagents in the current session."""

    def __init__(
        self,
        provider: LLMProvider,
        tool_registry: ToolRegistry,
        config: RikuganConfig,
        host_name: str,
        skill_registry: SkillRegistry | None = None,
    ) -> None:
        self._provider = provider
        self._tools = tool_registry
        self._config = config
        self._host_name = host_name
        self._skills = skill_registry
        self._agents: dict[str, SubagentInfo] = {}
        self._event_queue: queue.Queue[TurnEvent] = queue.Queue()
        self._cancel_events: dict[str, threading.Event] = {}

    def spawn(
        self,
        name: str,
        task: str,
        agent_type: str = "custom",
        parent_id: str | None = None,
        perks: list[str] | None = None,
        max_turns: int = 20,
        category: str = "",
    ) -> str:
        """Spawn a new subagent in a background thread. Returns agent ID."""
        agent_id = uuid.uuid4().hex[:12]
        cancel = threading.Event()
        self._cancel_events[agent_id] = cancel

        info = SubagentInfo(
            id=agent_id,
            name=name,
            task=task,
            agent_type=agent_type,
            status=SubagentStatus.PENDING,
            created_at=time.time(),
            parent_id=parent_id,
            perks=perks or [],
            category=category,
        )
        self._agents[agent_id] = info

        if parent_id and parent_id in self._agents:
            self._agents[parent_id].children.append(agent_id)

        # Determine system addendum based on agent type
        system_addendum = self._build_system_addendum(agent_type, perks or [])

        # Override max_turns for known agent types
        if agent_type == "network_recon":
            from .agents.network_recon import NETWORK_RECON_MAX_TURNS

            max_turns = max_turns or NETWORK_RECON_MAX_TURNS
        elif agent_type == "report_writer":
            from .agents.report_writer import REPORT_WRITER_MAX_TURNS

            max_turns = max_turns or REPORT_WRITER_MAX_TURNS

        # Emit spawned event
        self._event_queue.put(
            TurnEvent.subagent_spawned(
                agent_id=agent_id,
                name=name,
                agent_type=agent_type,
                task=task,
            )
        )

        thread = threading.Thread(
            target=self._run_agent,
            args=(agent_id, task, max_turns, system_addendum, cancel),
            daemon=True,
            name=f"rikugan-subagent-{agent_id[:6]}",
        )
        thread.start()
        log_info(f"Subagent spawned: id={agent_id}, name={name!r}, type={agent_type}")
        return agent_id

    def register(
        self,
        name: str,
        task: str,
        agent_type: str = "custom",
        parent_id: str | None = None,
        perks: list[str] | None = None,
        category: str = "",
    ) -> str:
        """Register an external agent for display without spawning a thread.

        Use this for agents managed outside SubagentManager (e.g. bulk rename
        deep-mode agents that run their own SubagentRunner).  Returns agent ID.
        """
        agent_id = uuid.uuid4().hex[:12]

        info = SubagentInfo(
            id=agent_id,
            name=name,
            task=task,
            agent_type=agent_type,
            status=SubagentStatus.PENDING,
            created_at=time.time(),
            parent_id=parent_id,
            perks=perks or [],
            category=category,
        )
        self._agents[agent_id] = info

        if parent_id and parent_id in self._agents:
            self._agents[parent_id].children.append(agent_id)

        self._event_queue.put(
            TurnEvent.subagent_spawned(
                agent_id=agent_id,
                name=name,
                agent_type=agent_type,
                task=task,
            )
        )
        log_info(f"External agent registered: id={agent_id}, name={name!r}")
        return agent_id

    def update_external(
        self,
        agent_id: str,
        status: SubagentStatus,
        summary: str = "",
        turn_count: int = 0,
    ) -> None:
        """Update state of an externally managed agent."""
        info = self._agents.get(agent_id)
        if info is None:
            return

        info.status = status
        info.summary = summary
        info.turn_count = turn_count

        if status in (SubagentStatus.COMPLETED, SubagentStatus.FAILED, SubagentStatus.CANCELLED):
            info.completed_at = time.time()
            elapsed = info.completed_at - info.created_at
            if status == SubagentStatus.COMPLETED:
                self._event_queue.put(
                    TurnEvent.subagent_completed(
                        agent_id=agent_id,
                        name=info.name,
                        summary=summary,
                        turn_count=turn_count,
                        elapsed=elapsed,
                    )
                )
            else:
                self._event_queue.put(
                    TurnEvent.subagent_failed(
                        agent_id=agent_id,
                        name=info.name,
                        error=summary,
                    )
                )
        elif status == SubagentStatus.RUNNING:
            self._event_queue.put(
                TurnEvent.subagent_progress(
                    agent_id=agent_id,
                    turn_count=turn_count,
                )
            )

    def cancel(self, agent_id: str) -> None:
        """Cancel a running or pending subagent."""
        cancel = self._cancel_events.get(agent_id)
        if cancel:
            cancel.set()
        info = self._agents.get(agent_id)
        if info and info.status in (SubagentStatus.PENDING, SubagentStatus.RUNNING):
            info.status = SubagentStatus.CANCELLED
            info.completed_at = time.time()
            self._event_queue.put(
                TurnEvent.subagent_failed(
                    agent_id=agent_id,
                    name=info.name,
                    error="Cancelled by user",
                )
            )

    def get(self, agent_id: str) -> SubagentInfo | None:
        """Look up a subagent by ID."""
        return self._agents.get(agent_id)

    def list_all(self) -> list[SubagentInfo]:
        """Return all subagent info records."""
        return list(self._agents.values())

    def tree(self) -> list[SubagentInfo]:
        """Return root agents (those with no parent).

        Children are accessible via the .children field on each SubagentInfo.
        """
        return [a for a in self._agents.values() if a.parent_id is None]

    def poll_event(self) -> TurnEvent | None:
        """Non-blocking poll for the next subagent event."""
        try:
            return self._event_queue.get_nowait()
        except queue.Empty:
            return None

    def running_count(self) -> int:
        """Number of subagents currently running."""
        return sum(1 for a in self._agents.values() if a.status == SubagentStatus.RUNNING)

    def completed_count(self) -> int:
        """Number of subagents that have completed."""
        return sum(1 for a in self._agents.values() if a.status == SubagentStatus.COMPLETED)

    def _build_system_addendum(self, agent_type: str, perks: list[str]) -> str:
        """Build the system prompt addendum for the given agent type and perks."""
        if agent_type == "network_recon":
            from .agents.network_recon import build_network_recon_addendum

            return build_network_recon_addendum()
        elif agent_type == "report_writer":
            from .agents.report_writer import build_report_writer_addendum

            return build_report_writer_addendum()
        else:
            from .agents.perks import build_perks_addendum

            return build_perks_addendum(perks)

    def _run_agent(
        self,
        agent_id: str,
        task: str,
        max_turns: int,
        system_addendum: str,
        cancel: threading.Event,
    ) -> None:
        """Background thread target: run a subagent to completion."""
        from .subagent import SubagentRunner  # deferred to avoid circular import

        info = self._agents[agent_id]
        info.status = SubagentStatus.RUNNING

        runner = SubagentRunner(
            provider=self._provider,
            tool_registry=self._tools,
            config=self._config,
            host_name=self._host_name,
            skill_registry=self._skills,
            cancel_event=cancel,
        )

        try:
            turn_count = 0
            final_text = ""
            gen = runner.run_task(task, max_turns=max_turns, system_addendum=system_addendum)
            for event in gen:
                if cancel.is_set():
                    info.status = SubagentStatus.CANCELLED
                    info.completed_at = time.time()
                    self._event_queue.put(
                        TurnEvent.subagent_failed(
                            agent_id=agent_id,
                            name=info.name,
                            error="Cancelled by user",
                        )
                    )
                    return

                if event.type.value == "turn_end":
                    turn_count += 1
                    info.turn_count = turn_count
                    self._event_queue.put(
                        TurnEvent.subagent_progress(
                            agent_id=agent_id,
                            turn_count=turn_count,
                        )
                    )

                if event.type.value == "text_done" and event.text:
                    final_text = event.text

                if event.usage:
                    info.token_usage = event.usage

            info.summary = final_text
            info.status = SubagentStatus.COMPLETED
            info.completed_at = time.time()
            elapsed = info.completed_at - info.created_at

            self._event_queue.put(
                TurnEvent.subagent_completed(
                    agent_id=agent_id,
                    name=info.name,
                    summary=final_text,
                    turn_count=turn_count,
                    elapsed=elapsed,
                )
            )
            log_info(
                f"Subagent completed: id={agent_id}, turns={turn_count}, "
                f"elapsed={elapsed:.1f}s, summary_len={len(final_text)}"
            )

        except Exception as e:
            info.status = SubagentStatus.FAILED
            info.completed_at = time.time()
            info.summary = f"Error: {e}"
            self._event_queue.put(
                TurnEvent.subagent_failed(
                    agent_id=agent_id,
                    name=info.name,
                    error=str(e),
                )
            )
            log_error(f"Subagent failed: id={agent_id}, error={e}")
