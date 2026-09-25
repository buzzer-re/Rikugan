"""Host-agnostic session controller orchestration."""

from __future__ import annotations

import copy
import os
import threading
import time
import uuid
from collections.abc import Callable
from typing import TYPE_CHECKING, Any

from ..agent.loop import AgentLoop, BackgroundAgentRunner
from ..agent.turn import TurnEvent
from ..core.config import RikuganConfig
from ..core.host import get_database_instance_id, set_database_instance_id
from ..core.logging import log_debug, log_error, log_info
from ..mcp.manager import MCPManager
from ..providers.registry import ProviderRegistry
from ..skills.registry import SkillRegistry
from ..state.history import SessionHistory
from ..state.session import SessionState

if TYPE_CHECKING:
    from ..tools.registry import ToolRegistry
else:
    ToolRegistry = Any


def _normalize_db_path(path: str) -> str:
    if not path:
        return ""
    try:
        return os.path.normcase(os.path.realpath(os.path.abspath(path)))
    except OSError:
        return path


class SessionControllerBase:
    """Non-Qt orchestrator for Rikugan sessions."""

    def __init__(
        self,
        config: RikuganConfig,
        tool_registry_factory: Callable[..., ToolRegistry],
        database_path_getter: Callable[[], str],
        host_name: str,
    ):
        self.config = config
        self.host_name = host_name
        self._provider_registry = ProviderRegistry()
        self._provider_registry.register_custom_providers(list(config.custom_providers.keys()))
        self._tool_registry = tool_registry_factory(config)
        # Serializes background autosaves so two turns cannot write the same
        # session file at once.
        self._autosave_lock = threading.Lock()
        self._autosave_thread: threading.Thread | None = None
        self._skill_registry = SkillRegistry()
        self._mcp_manager = MCPManager()
        self._idb_path = _normalize_db_path(database_path_getter())
        self._db_instance_id = self._ensure_db_instance_id()
        self._runtime_init_done = threading.Event()
        self._runtime_shutdown = threading.Event()
        self._runtime_init_thread = threading.Thread(
            target=self._initialize_runtime,
            daemon=True,
            name="rikugan-runtime-init",
        )
        self._runtime_init_thread.start()

        # Multi-tab session management
        self._sessions: dict[str, SessionState] = {}
        self._active_tab_id: str = ""
        tab_id = self._create_session()
        self._active_tab_id = tab_id

        self._runners: dict[str, BackgroundAgentRunner] = {}
        self._pending_messages: dict[str, list[str]] = {}

    def _initialize_runtime(self) -> None:
        """Load heavy runtime components off the UI path."""
        started = time.perf_counter()
        try:
            if self._runtime_shutdown.is_set():
                return
            self._skill_registry.discover()

            # Apply disabled skills + load enabled external skills
            self._skill_registry.load_external_skills(
                self.config.enabled_external_skills,
                self.config.disabled_skills,
            )

            if self._runtime_shutdown.is_set():
                return
            self._mcp_manager.load_config()

            enabled_set = set(self.config.enabled_external_mcp)
            if enabled_set:
                # Load enabled external MCP servers only when explicitly configured.
                from ..core.external_sources import discover_all_external_mcp

                external_mcp = discover_all_external_mcp()
                for source_key, servers in external_mcp.items():
                    enabled = [s for s in servers if f"{source_key}:{s.name}" in enabled_set]
                    if enabled:
                        self._mcp_manager.add_external_configs(enabled)

            if self._runtime_shutdown.is_set():
                return
            self._mcp_manager.start_servers(self._tool_registry)
        except Exception as e:
            log_error(f"Background runtime initialization failed: {e}")
        finally:
            self._runtime_init_done.set()
            elapsed_ms = int((time.perf_counter() - started) * 1000)
            log_debug(f"Runtime initialization completed in {elapsed_ms} ms")

    # --- Instance ID ---

    @staticmethod
    def _ensure_db_instance_id() -> str:
        """Read or generate a database-instance UUID for the current IDB/BNDB."""
        existing = get_database_instance_id()
        if existing:
            log_debug(f"Database instance ID: {existing}")
            return existing
        new_id = uuid.uuid4().hex
        if set_database_instance_id(new_id):
            log_info(f"Generated new database instance ID: {new_id}")
            return new_id
        # Standalone or write failure — use an ephemeral ID (won't persist)
        log_debug("Could not persist database instance ID, using ephemeral")
        return new_id

    # --- Tab / multi-session management ---

    def _create_session(self) -> str:
        """Create a new SessionState and return its tab_id."""
        tab_id = uuid.uuid4().hex[:8]
        session = SessionState(
            provider_name=self.config.provider.name,
            model_name=self.config.provider.model,
            idb_path=self._idb_path,
            db_instance_id=self._db_instance_id,
        )
        self._sessions[tab_id] = session
        return tab_id

    def create_tab(self) -> str:
        """Create a new tab with a fresh session. Returns tab_id."""
        tab_id = self._create_session()
        log_info(f"Created new tab {tab_id}")
        return tab_id

    def fork_session(self, source_tab_id: str) -> str | None:
        """Duplicate a session into a new tab. Returns new tab_id or None."""
        source = self._sessions.get(source_tab_id)
        if source is None:
            return None
        new_tab_id = uuid.uuid4().hex[:8]
        forked = SessionState(
            provider_name=source.provider_name,
            model_name=source.model_name,
            idb_path=source.idb_path,
            db_instance_id=source.db_instance_id,
        )
        forked.messages = copy.deepcopy(source.messages)
        forked.total_usage = copy.copy(source.total_usage)
        forked.last_prompt_tokens = source.last_prompt_tokens
        forked.current_turn = source.current_turn
        forked.metadata = dict(source.metadata)
        forked.metadata["forked_from"] = source.id
        self._sessions[new_tab_id] = forked
        log_info(f"Forked session {source.id} → new tab {new_tab_id}")
        return new_tab_id

    def close_tab(self, tab_id: str) -> None:
        """Save and remove a tab's session."""
        session = self._sessions.get(tab_id)
        if session is None:
            return
        self.cancel(tab_id)
        self._runners.pop(tab_id, None)
        self._pending_messages.pop(tab_id, None)
        if self.config.checkpoint_auto_save and session.messages:
            try:
                history = SessionHistory(self.config)
                history.save_session(session)
            except (OSError, ValueError) as e:
                log_error(f"Failed to save session on tab close: {e}")
        del self._sessions[tab_id]
        log_debug(f"Closed tab {tab_id}")

    def switch_tab(self, tab_id: str) -> None:
        """Switch active tab without interrupting other running chats."""
        if tab_id == self._active_tab_id:
            return
        if tab_id not in self._sessions:
            return
        self._active_tab_id = tab_id
        # Reopening this binary should land here, not on whichever chat was
        # created last.
        self._sessions[tab_id].touch()
        log_debug(f"Switched to tab {tab_id}")

    def tab_label(self, tab_id: str) -> str:
        """Return a display label for a tab."""
        session = self._sessions.get(tab_id)
        if session is None:
            return "New Chat"
        for msg in session.messages:
            if msg.role.value == "user" and msg.content:
                text = msg.content.strip()
                return text[:20] + ("..." if len(text) > 20 else "")
        return "New Chat"

    @property
    def active_tab_id(self) -> str:
        return self._active_tab_id

    @property
    def tab_ids(self) -> list[str]:
        return list(self._sessions.keys())

    @property
    def session(self) -> SessionState:
        return self._sessions[self._active_tab_id]

    def get_session(self, tab_id: str) -> SessionState | None:
        return self._sessions.get(tab_id)

    @property
    def provider_registry(self) -> ProviderRegistry:
        return self._provider_registry

    @property
    def tool_registry(self) -> ToolRegistry:
        return self._tool_registry

    @property
    def skill_slugs(self) -> list[str]:
        if not self._runtime_init_done.is_set():
            return []
        return self._skill_registry.list_slugs()

    @property
    def runtime_ready(self) -> bool:
        return self._runtime_init_done.is_set()

    @property
    def is_agent_running(self) -> bool:
        return self.is_tab_running(self._active_tab_id)

    @property
    def any_agent_running(self) -> bool:
        return any(runner.agent_loop.is_running for runner in self._runners.values())

    @property
    def has_runners(self) -> bool:
        """True while any runner is still registered, running or draining."""
        return bool(self._runners)

    def is_tab_running(self, tab_id: str) -> bool:
        runner = self._runners.get(tab_id)
        return runner is not None and runner.agent_loop.is_running

    def tab_pending_count(self, tab_id: str) -> int:
        return len(self._pending_messages.get(tab_id, []))

    def get_runner(self) -> BackgroundAgentRunner | None:
        return self._runners.get(self._active_tab_id)

    def get_runner_for_tab(self, tab_id: str) -> BackgroundAgentRunner | None:
        return self._runners.get(tab_id)

    def running_tab_ids(self) -> list[str]:
        """Return tab ids that still have an active background runner."""
        return [tab_id for tab_id, runner in self._runners.items() if runner.agent_loop.is_running]

    def get_provider(self) -> Any:
        """Create and return an LLMProvider instance for the current config."""
        if not self._runtime_init_done.is_set():
            self._runtime_init_done.wait(timeout=10.0)
        try:
            provider = self._provider_registry.get_or_create(
                self.config.provider.name,
                api_key=self.config.provider.api_key,
                api_base=self.config.provider.api_base,
                model=self.config.provider.model,
                **(self.config.provider.extra or {}),
            )
            provider.ensure_ready()
            return provider
        except Exception as e:
            log_error(f"Provider creation failed: {e}")
            return None

    def get_context_window(self) -> int:
        """Return the active provider/model context window without UI-owned overrides."""
        try:
            provider = self._provider_registry.get_or_create(
                self.config.provider.name,
                api_key=self.config.provider.api_key,
                api_base=self.config.provider.api_base,
                model=self.config.provider.model,
                **(self.config.provider.extra or {}),
            )
            return provider.context_window()
        except Exception as e:
            log_debug(f"Context window lookup failed: {e}")
            return self.config.provider.context_window or 128000

    def get_tool_registry(self) -> ToolRegistry:
        """Return the tool registry."""
        return self._tool_registry

    def start_agent(self, user_message: str, tab_id: str | None = None) -> str | None:
        """Create provider + agent loop and start the background runner."""
        tab_id = tab_id or self._active_tab_id
        if self.is_tab_running(tab_id):
            self.queue_message(user_message, tab_id=tab_id)
            return None

        if not self._runtime_init_done.is_set():
            # Delay only the first agent start if background init is still running.
            self._runtime_init_done.wait(timeout=10.0)

        try:
            provider = self._provider_registry.get_or_create(
                self.config.provider.name,
                api_key=self.config.provider.api_key,
                api_base=self.config.provider.api_base,
                model=self.config.provider.model,
                **(self.config.provider.extra or {}),
            )
            provider.ensure_ready()
        except Exception as e:
            log_error(f"Provider creation failed: {e}")
            return f"Provider error: {e}"

        session = self._sessions[tab_id]
        loop = AgentLoop(
            provider,
            self._tool_registry,
            self.config,
            session,
            skill_registry=self._skill_registry,
            host_name=self.host_name,
        )
        runner = BackgroundAgentRunner(loop)
        self._runners[tab_id] = runner
        runner.start(user_message)
        return None

    def get_event(self, timeout: float = 0, tab_id: str | None = None) -> TurnEvent | None:
        runner = self._runners.get(tab_id or self._active_tab_id)
        if runner is None:
            return None
        return runner.get_event(timeout=timeout)

    def poll_events(self, max_per_tab: int = 30) -> list[tuple[str, TurnEvent]]:
        """Drain pending events from all running chats."""
        events: list[tuple[str, TurnEvent]] = []
        for tab_id, runner in list(self._runners.items()):
            for _ in range(max_per_tab):
                event = runner.get_event(timeout=0)
                if event is None:
                    break
                events.append((tab_id, event))
        return events

    def cancel(self, tab_id: str | None = None) -> None:
        target = tab_id or self._active_tab_id
        self._pending_messages.pop(target, None)
        runner = self._runners.get(target)
        if runner:
            runner.cancel()

    def queue_message(self, text: str, tab_id: str | None = None) -> None:
        target = tab_id or self._active_tab_id
        queue_for_tab = self._pending_messages.setdefault(target, [])
        queue_for_tab.append(text)
        log_debug(f"Message queued for {target}, {len(queue_for_tab)} pending")

    def on_agent_finished(self, tab_id: str | None = None) -> str | None:
        target = tab_id or self._active_tab_id
        self._runners.pop(target, None)
        pending = self._pending_messages.get(target, [])
        next_message = pending.pop(0) if pending else None
        if not pending:
            self._pending_messages.pop(target, None)

        # Re-persist the instance ID in the database.  For BN the BNDB may
        # not have existed at init time; writing again ensures the ID is
        # present when the BNDB is eventually saved.
        set_database_instance_id(self._db_instance_id)

        session = self._sessions.get(target)
        if session and self.config.checkpoint_auto_save and session.messages:
            self._autosave_session(session)
        return next_message

    def _autosave_session(self, session: SessionState) -> None:
        """Persist the session without blocking the caller's thread.

        This runs at the end of every turn, from the UI thread's event pump.
        Serializing every message — decompiler dumps and all — and writing two
        indented JSON files took long enough on a real session to stall the
        panel each time the agent finished.
        """
        snapshot = copy.copy(session)
        # Shallow-copy the containers so the worker can't trip over a list the
        # next turn is appending to. Messages are append-only once created.
        snapshot.messages = list(session.messages)
        snapshot.subagent_logs = {key: list(msgs) for key, msgs in (session.subagent_logs or {}).items()}

        def _write() -> None:
            with self._autosave_lock:
                try:
                    path = SessionHistory(self.config).save_session(snapshot)
                    log_debug(f"Session auto-saved: {path}")
                except (OSError, ValueError) as e:
                    log_error(f"Failed to auto-save session: {e}")

        thread = threading.Thread(target=_write, daemon=True, name="rikugan-session-save")
        self._autosave_thread = thread
        thread.start()

    def flush_autosaves(self, timeout: float = 5.0) -> None:
        """Wait for the last background save. Daemon threads die on exit."""
        thread = self._autosave_thread
        if thread is not None and thread.is_alive():
            thread.join(timeout)

    def new_chat(self) -> None:
        """Reset the active tab to a fresh session."""
        self.cancel(self._active_tab_id)
        self._pending_messages.pop(self._active_tab_id, None)
        session = self._sessions.get(self._active_tab_id)
        if session and self.config.checkpoint_auto_save and session.messages:
            try:
                history = SessionHistory(self.config)
                history.save_session(session)
            except OSError as e:
                log_debug(f"Failed to save session on new chat: {e}")
        self._sessions[self._active_tab_id] = SessionState(
            provider_name=self.config.provider.name,
            model_name=self.config.provider.model,
            idb_path=self._idb_path,
            db_instance_id=self._db_instance_id,
        )
        log_info("Started new chat session (active tab)")

    def delete_tab(self, tab_id: str) -> None:
        """Delete a chat session from memory and disk without saving it first."""
        session = self._sessions.get(tab_id)
        self.cancel(tab_id)
        self._runners.pop(tab_id, None)
        self._pending_messages.pop(tab_id, None)
        if session is not None:
            try:
                SessionHistory(self.config).delete_session(session.id)
            except OSError as e:
                log_error(f"Failed to delete session {session.id}: {e}")
            del self._sessions[tab_id]
        if not self._sessions:
            self._active_tab_id = self._create_session()
        elif self._active_tab_id == tab_id:
            self._active_tab_id = next(iter(self._sessions))

    def load_restorable_sessions(self) -> list[SessionState]:
        """Read and parse all saved sessions for the current database from disk.

        Pure data path — performs only disk I/O and JSON/Message parsing and
        does NOT touch ``self._sessions`` or create any Qt objects, so it is
        safe to run on a background thread.  The expensive part of restore
        (parsing every message of every chat) lives here; tab registration and
        widget creation happen later on the UI thread via
        ``register_restored_sessions``.  Sessions are returned oldest-first.
        """
        loaded: list[SessionState] = []
        if not self.config.restore_sessions_on_start:
            log_debug("Skipping session restore: disabled in config")
            return loaded
        if not self._idb_path:
            log_debug("Skipping session restore: no database path available")
            return loaded
        try:
            history = SessionHistory(self.config)
            summaries = history.list_sessions(
                idb_path=self._idb_path,
                db_instance_id=self._db_instance_id,
            )
            summaries.sort(key=lambda s: s.get("created_at", 0))
            for summary in summaries:
                session = history.load_session(summary["id"])
                if session and session.messages:
                    loaded.append(session)
        except (OSError, ValueError, KeyError) as e:
            log_error(f"Failed to load sessions for restore: {e}")
        return loaded

    def belongs_to_current_db(self, session: SessionState) -> bool:
        """Whether this chat was opened against the binary now in view.

        A chat carries its binary's addresses, names and analysis in its
        history, and the agent answers against whatever database is loaded
        now. Listing one from another binary invites the user to type into it,
        which silently mixes two binaries' context — so those stay hidden
        rather than merely sorted to the bottom.
        """
        if session.db_instance_id and self._db_instance_id:
            return session.db_instance_id == self._db_instance_id
        if session.idb_path and self._idb_path:
            # Normalize both: a session saved before the path was canonicalized
            # would otherwise look like a different binary.
            return _normalize_db_path(session.idb_path) == _normalize_db_path(self._idb_path)
        # Nothing on either side identifies a binary, so there is nothing to
        # contradict — an unsaved or pre-upgrade chat stays visible.
        return True

    def register_restored_sessions(self, sessions: list[SessionState]) -> list[tuple[str, SessionState]]:
        """Register pre-loaded sessions as tabs. Must run on the UI thread.

        Assigns tab ids, inserts them into the live session map, drops the
        default empty tab, and activates the most recent restored session.
        """
        results: list[tuple[str, SessionState]] = []
        for session in sessions:
            if not session.messages:
                continue
            if not self.belongs_to_current_db(session):
                log_debug(f"Skipping session {session.id}: belongs to another binary")
                continue
            tab_id = uuid.uuid4().hex[:8]
            self._sessions[tab_id] = session
            results.append((tab_id, session))
            log_debug(f"Restored session {session.id} as tab {tab_id}")
        if results:
            # Drop the default empty session created in __init__ and activate
            # the most recent restored tab. If the user already started working
            # in the default tab during the (async) load, keep them there.
            default_dropped = False
            if self._active_tab_id in self._sessions:
                default_session = self._sessions[self._active_tab_id]
                if not default_session.messages:
                    del self._sessions[self._active_tab_id]
                    default_dropped = True
            if default_dropped:
                # The chat the user was last working in, which is rarely the
                # one created most recently. Typing into the wrong chat means
                # typing into another binary's context.
                self._active_tab_id = max(results, key=lambda r: r[1].last_active_at)[0]
        return results

    def restore_sessions(self) -> list[tuple[str, SessionState]]:
        """Load ALL saved sessions and register them as tabs (synchronous).

        Retained for callers/tests that want the blocking path; the panel uses
        the split ``load_restorable_sessions`` + ``register_restored_sessions``
        so the disk/parse cost stays off the UI thread.
        """
        return self.register_restored_sessions(self.load_restorable_sessions())

    def restore_session(self) -> SessionState | None:
        """Legacy: restore only the latest session into the active tab."""
        if not self.config.restore_sessions_on_start:
            return None
        if not self._idb_path:
            log_debug("Skipping session restore: no database path available")
            return None
        try:
            history = SessionHistory(self.config)
            session = history.get_latest_session(
                idb_path=self._idb_path,
                db_instance_id=self._db_instance_id,
            )
            if session and session.messages:
                log_debug(f"Restoring session {session.id} with {len(session.messages)} messages")
                self._sessions[self._active_tab_id] = session
                log_info(f"Restored session {session.id} ({len(session.messages)} messages)")
                return session
        except (OSError, ValueError, KeyError) as e:
            log_error(f"Failed to restore session: {e}")
        return None

    def reset_for_new_file(self, new_idb_path: str) -> None:
        """Save all sessions and reset for a new database file."""
        for tab_id in list(self._runners):
            self.cancel(tab_id)
        for tab_id, session in self._sessions.items():
            if session.messages:
                try:
                    history = SessionHistory(self.config)
                    history.save_session(session)
                except (OSError, ValueError) as e:
                    log_error(f"Failed to save session {tab_id} on file change: {e}")
        self._sessions.clear()
        self._idb_path = _normalize_db_path(new_idb_path)
        self._db_instance_id = self._ensure_db_instance_id()
        tab_id = self._create_session()
        self._active_tab_id = tab_id

    def update_settings(self) -> None:
        # Re-register custom providers in case user added/removed one
        self._provider_registry.register_custom_providers(list(self.config.custom_providers.keys()))
        for session in self._sessions.values():
            session.provider_name = self.config.provider.name
            session.model_name = self.config.provider.model

    def reload_mcp(self) -> None:
        """Reload MCP config and restart servers in the background.

        Safe to call at any time — stops existing servers first, then
        re-reads the config and starts newly-enabled servers.
        """
        thread = threading.Thread(
            target=self._mcp_manager.reload,
            args=(self._tool_registry,),
            daemon=True,
            name="rikugan-mcp-reload",
        )
        thread.start()

    def shutdown(self) -> None:
        self._runtime_shutdown.set()
        # A background autosave would be killed mid-write when the host exits.
        self.flush_autosaves()
        if self._runtime_init_thread.is_alive():
            self._runtime_init_done.wait(timeout=1.0)
        for tab_id in list(self._runners):
            self.cancel(tab_id)
        self._runners.clear()
        # Final attempt to persist instance ID before the host saves the DB.
        set_database_instance_id(self._db_instance_id)
        for tab_id, session in self._sessions.items():
            if self.config.checkpoint_auto_save and session.messages:
                try:
                    history = SessionHistory(self.config)
                    history.save_session(session)
                except (OSError, ValueError) as e:
                    log_error(f"Failed to save session {tab_id} on shutdown: {e}")
        self._mcp_manager.shutdown()
