"""Tests for iris.ui.session_controller."""

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from tests.mocks.ida_mock import install_ida_mocks

install_ida_mocks()

# Some UI tests stub modules in sys.modules; ensure this test gets real ones.
for _mod_name in [
    "rikugan.core.types",
    "rikugan.state.session",
    "rikugan.state.history",
    "rikugan.core.config",
    "rikugan.core.logging",
    "rikugan.agent.turn",
    "rikugan.agent.mutation",
    "rikugan.providers.auth_cache",
    "rikugan.providers.anthropic_provider",
    "rikugan.providers.ollama_provider",
    "rikugan.providers.registry",
    "rikugan.ui.chat_view",
    "rikugan.ui.context_bar",
    "rikugan.ui.input_area",
    "rikugan.ui.styles",
    "rikugan.ui.tool_widgets",
]:
    sys.modules.pop(_mod_name, None)

from rikugan.core.config import RikuganConfig
from rikugan.core.types import Message, Role, TokenUsage, ToolCall, ToolResult
from rikugan.ida.ui.session_controller import IdaSessionController
from rikugan.state.session import SessionState


class TestIdaSessionController(unittest.TestCase):
    def setUp(self):
        self.cfg = RikuganConfig()
        self.cfg._config_dir = tempfile.mkdtemp()
        self.ctrl = IdaSessionController(self.cfg)

    def tearDown(self):
        self.ctrl.shutdown()

    def test_initial_session_state(self):
        self.assertIsNotNone(self.ctrl.session)
        self.assertEqual(self.ctrl.session.provider_name, self.cfg.provider.name)
        self.assertEqual(self.ctrl.session.model_name, self.cfg.provider.model)

    def test_is_agent_running_initially_false(self):
        self.assertFalse(self.ctrl.is_agent_running)

    def test_get_event_without_runner_returns_none(self):
        self.assertIsNone(self.ctrl.get_event())

    def test_queue_and_drain_messages(self):
        self.ctrl.queue_message("first")
        self.ctrl.queue_message("second")

        # on_agent_finished returns queued messages one at a time
        next_msg = self.ctrl.on_agent_finished()
        self.assertEqual(next_msg, "first")

        next_msg = self.ctrl.on_agent_finished()
        self.assertEqual(next_msg, "second")

        next_msg = self.ctrl.on_agent_finished()
        self.assertIsNone(next_msg)

    def test_cancel_clears_pending_messages(self):
        self.ctrl.queue_message("will be cancelled")
        self.ctrl.cancel()
        next_msg = self.ctrl.on_agent_finished()
        self.assertIsNone(next_msg)

    def test_new_chat_creates_fresh_session(self):
        old_id = self.ctrl.session.id
        self.ctrl.session.add_message(Message(role=Role.USER, content="hello"))
        self.ctrl.new_chat()

        self.assertNotEqual(self.ctrl.session.id, old_id)
        self.assertEqual(len(self.ctrl.session.messages), 0)

    def test_new_chat_clears_pending_messages(self):
        self.ctrl.queue_message("pending")
        self.ctrl.new_chat()
        self.assertIsNone(self.ctrl.on_agent_finished())

    def test_update_settings_syncs_session(self):
        self.cfg.provider.name = "test_provider"
        self.cfg.provider.model = "test_model"
        self.ctrl.update_settings()

        self.assertEqual(self.ctrl.session.provider_name, "test_provider")
        self.assertEqual(self.ctrl.session.model_name, "test_model")

    def test_skill_slugs_returns_list(self):
        slugs = self.ctrl.skill_slugs
        self.assertIsInstance(slugs, list)

    def test_on_agent_finished_auto_saves(self):
        self.cfg.checkpoint_auto_save = True
        self.ctrl.session.add_message(Message(role=Role.USER, content="test"))
        self.ctrl.on_agent_finished()
        # The autosave runs on a background thread so the panel doesn't stall.
        self.ctrl.flush_autosaves()

        # Verify session was saved to disk
        from rikugan.state.history import SessionHistory

        history = SessionHistory(self.cfg)
        sessions = history.list_sessions(db_instance_id=self.ctrl._db_instance_id)
        self.assertTrue(any(s["id"] == self.ctrl.session.id for s in sessions))

    def test_restore_session(self):
        # Save a session first
        self.ctrl.session.add_message(Message(role=Role.USER, content="persisted"))
        self.cfg.checkpoint_auto_save = True
        self.ctrl.on_agent_finished()
        self.ctrl.flush_autosaves()
        saved_id = self.ctrl.session.id

        # New chat, then restore
        self.ctrl.new_chat()
        self.assertNotEqual(self.ctrl.session.id, saved_id)

        restored = self.ctrl.restore_session()
        self.assertIsNotNone(restored)
        self.assertEqual(self.ctrl.session.id, saved_id)
        self.assertEqual(len(self.ctrl.session.messages), 1)
        self.assertEqual(self.ctrl.session.messages[0].content, "persisted")

    def test_restore_sessions_returns_saved_sessions(self):
        self.ctrl.session.add_message(Message(role=Role.USER, content="persisted one"))
        self.cfg.checkpoint_auto_save = True
        self.ctrl.on_agent_finished()
        self.ctrl.flush_autosaves()

        self.ctrl.new_chat()
        self.ctrl.session.add_message(Message(role=Role.USER, content="persisted two"))
        self.ctrl.on_agent_finished()
        self.ctrl.flush_autosaves()

        ctrl2 = IdaSessionController(self.cfg)
        restored = ctrl2.restore_sessions()
        self.assertEqual(len(restored), 2)
        self.assertTrue(all(session.messages for _, session in restored))
        ctrl2.shutdown()

    def test_restore_preserves_token_usage(self):
        """Full round-trip: save with token usage -> restore -> verify preserved."""
        usage = TokenUsage(prompt_tokens=100, completion_tokens=50, total_tokens=150)
        self.ctrl.session.add_message(Message(role=Role.USER, content="question"))
        self.ctrl.session.add_message(
            Message(role=Role.ASSISTANT, content="answer", token_usage=usage),
        )
        self.cfg.checkpoint_auto_save = True
        self.ctrl.on_agent_finished()
        self.ctrl.flush_autosaves()
        saved_id = self.ctrl.session.id

        # Create fresh controller to avoid in-memory state
        ctrl2 = IdaSessionController(self.cfg)
        restored = ctrl2.restore_session()
        self.assertIsNotNone(restored)
        self.assertEqual(restored.id, saved_id)
        self.assertEqual(len(restored.messages), 2)
        self.assertEqual(restored.messages[1].content, "answer")
        ctrl2.shutdown()

    def test_restore_preserves_tool_calls(self):
        """Full round-trip: save with tool calls -> restore -> verify preserved."""
        tc = ToolCall(id="tc_1", name="get_info", arguments={"addr": "0x1000"})
        tr = ToolResult(tool_call_id="tc_1", name="get_info", content="data here")
        self.ctrl.session.add_message(Message(role=Role.USER, content="analyze"))
        self.ctrl.session.add_message(
            Message(role=Role.ASSISTANT, content="", tool_calls=[tc]),
        )
        self.ctrl.session.add_message(Message(role=Role.TOOL, tool_results=[tr]))
        self.cfg.checkpoint_auto_save = True
        self.ctrl.on_agent_finished()
        self.ctrl.flush_autosaves()

        ctrl2 = IdaSessionController(self.cfg)
        restored = ctrl2.restore_session()
        self.assertIsNotNone(restored)
        self.assertEqual(len(restored.messages), 3)
        self.assertEqual(len(restored.messages[1].tool_calls), 1)
        self.assertEqual(restored.messages[1].tool_calls[0].name, "get_info")
        self.assertEqual(restored.messages[2].tool_results[0].content, "data here")
        ctrl2.shutdown()

    def test_runtime_init_skips_external_mcp_discovery_when_none_enabled(self):
        self.ctrl.shutdown()

        with patch.object(self.cfg, "enabled_external_mcp", []):
            with patch("rikugan.core.external_sources.discover_all_external_mcp") as discover_mcp:
                ctrl = IdaSessionController(self.cfg)
                ctrl._runtime_init_done.wait(timeout=5.0)
                ctrl.shutdown()

        self.assertFalse(discover_mcp.called)

    def test_runtime_init_discovers_external_mcp_when_enabled(self):
        self.ctrl.shutdown()

        with patch.object(self.cfg, "enabled_external_mcp", ["claude:test"]):
            with patch(
                "rikugan.core.external_sources.discover_all_external_mcp", return_value={"claude": [], "codex": []}
            ) as discover_mcp:
                ctrl = IdaSessionController(self.cfg)
                ctrl._runtime_init_done.wait(timeout=5.0)
                ctrl.shutdown()

        self.assertTrue(discover_mcp.called)

    def test_shutdown_is_idempotent(self):
        self.ctrl.shutdown()
        self.ctrl.shutdown()  # Should not raise

    def test_fork_session_copies_messages(self):
        """Forking should create a new tab with a deep copy of messages."""
        self.ctrl.session.add_message(Message(role=Role.USER, content="hello"))
        self.ctrl.session.add_message(Message(role=Role.ASSISTANT, content="hi"))
        source_tab = self.ctrl.active_tab_id

        new_tab_id = self.ctrl.fork_session(source_tab)
        self.assertIsNotNone(new_tab_id)
        self.assertNotEqual(new_tab_id, source_tab)

        forked = self.ctrl._sessions[new_tab_id]
        self.assertEqual(len(forked.messages), 2)
        self.assertEqual(forked.messages[0].content, "hello")
        self.assertEqual(forked.messages[1].content, "hi")
        self.assertNotEqual(forked.id, self.ctrl.session.id)

    def test_fork_session_deep_copies(self):
        """Modifications to forked session should not affect the original."""
        self.ctrl.session.add_message(Message(role=Role.USER, content="original"))
        source_tab = self.ctrl.active_tab_id

        new_tab_id = self.ctrl.fork_session(source_tab)
        forked = self.ctrl._sessions[new_tab_id]
        forked.add_message(Message(role=Role.USER, content="forked-only"))

        self.assertEqual(len(self.ctrl.session.messages), 1)
        self.assertEqual(len(forked.messages), 2)

    def test_fork_nonexistent_tab_returns_none(self):
        result = self.ctrl.fork_session("nonexistent")
        self.assertIsNone(result)

    def test_fork_records_metadata(self):
        """Forked session should have forked_from metadata."""
        source_tab = self.ctrl.active_tab_id
        source_id = self.ctrl.session.id

        new_tab_id = self.ctrl.fork_session(source_tab)
        forked = self.ctrl._sessions[new_tab_id]
        self.assertEqual(forked.metadata.get("forked_from"), source_id)


if __name__ == "__main__":
    unittest.main()


class TestChatBinaryScoping(unittest.TestCase):
    """A chat belongs to the binary it was opened against.

    Its history carries that binary's addresses and names, and the agent
    answers against whatever database is loaded now — so typing into a chat
    from another binary silently mixes two contexts.
    """

    def setUp(self):
        self.cfg = RikuganConfig()
        self.cfg._config_dir = tempfile.mkdtemp()
        self.ctrl = IdaSessionController(self.cfg)
        self.ctrl._idb_path = "/bin/current.i64"
        self.ctrl._db_instance_id = "db-current"

    def tearDown(self):
        self.ctrl.shutdown()

    def _session(self, *, instance="", path="", last_active=0.0, text="hi"):
        s = SessionState(idb_path=path, db_instance_id=instance)
        s.messages.append(Message(role=Role.USER, content=text))
        if last_active:
            s.last_active_at = last_active
        return s

    def test_a_chat_from_another_binary_is_not_listed(self):
        keep = self._session(instance="db-current")
        drop = self._session(instance="db-other")
        registered = self.ctrl.register_restored_sessions([keep, drop])
        self.assertEqual([s for _, s in registered], [keep])

    def test_matching_falls_back_to_the_path(self):
        keep = self._session(path="/bin/current.i64")
        drop = self._session(path="/bin/elsewhere.i64")
        self.ctrl._db_instance_id = ""
        registered = self.ctrl.register_restored_sessions([keep, drop])
        self.assertEqual([s for _, s in registered], [keep])

    def test_a_chat_with_no_binary_recorded_is_kept(self):
        # Pre-upgrade or unsaved chats identify no binary; there is nothing to
        # contradict, so hiding them would lose the user's work.
        orphan = self._session()
        self.assertTrue(self.ctrl.belongs_to_current_db(orphan))

    def test_the_last_used_chat_is_focused_not_the_newest(self):
        older_but_recently_used = self._session(instance="db-current", last_active=5000.0, text="a")
        newest_but_untouched = self._session(instance="db-current", last_active=100.0, text="b")
        registered = self.ctrl.register_restored_sessions(
            [older_but_recently_used, newest_but_untouched]
        )
        expected = next(tid for tid, s in registered if s is older_but_recently_used)
        self.assertEqual(self.ctrl.active_tab_id, expected)

    def test_switching_to_a_chat_marks_it_as_used(self):
        registered = self.ctrl.register_restored_sessions(
            [self._session(instance="db-current", last_active=1.0, text=f"m{i}") for i in range(2)]
        )
        # Pick a chat that is not already active — switching to the active one
        # is a no-op by design.
        target, session = next(t for t in registered if t[0] != self.ctrl.active_tab_id)
        before = session.last_active_at
        self.ctrl.switch_tab(target)
        self.assertGreater(session.last_active_at, before)
