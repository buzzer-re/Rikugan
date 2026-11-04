"""Asking before the agent touches the disk, and undoing what it did."""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from unittest.mock import MagicMock, patch

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from tests.mocks.ida_mock import install_ida_mocks

install_ida_mocks()

for _mod_name in ("rikugan.core.types", "rikugan.core.config", "rikugan.state.session"):
    sys.modules.pop(_mod_name, None)

from rikugan.agent.loop import AgentLoop  # noqa: E402
from tests.agent.test_agent_loop import MockProvider, _text_response, _tool_call_response  # noqa: E402
from rikugan.agent.turn import TurnEventType  # noqa: E402
from rikugan.core.config import RikuganConfig  # noqa: E402
from rikugan.core.types import ToolCall  # noqa: E402
from rikugan.core.workspace import Workspace  # noqa: E402
from rikugan.state.session import SessionState, _result_limit  # noqa: E402
from rikugan.tools import filesystem  # noqa: E402
from rikugan.tools.registry import ToolRegistry  # noqa: E402


def _drive(generator, answer=None):
    """Run a tool-call generator, answering any approval. Returns (events, result)."""
    events = []
    while True:
        try:
            event = next(generator)
        except StopIteration as stop:
            return events, stop.value
        events.append(event)
        if event.type == TurnEventType.TOOL_APPROVAL_REQUEST:
            if answer is None:
                raise AssertionError("the agent asked when it should not have")
            answer()


class _Base(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.top = os.path.realpath(tmp.name)
        self.bin_dir = os.path.join(self.top, "case")
        os.makedirs(self.bin_dir)
        self.config = RikuganConfig()
        self.config._config_dir = os.path.join(self.top, "config")
        self.config.auto_context = False
        self.registry = ToolRegistry()
        self.registry.register_module(filesystem)
        self.session = self._session()

    def _session(self):
        session = SessionState()
        session.files.workspace = Workspace.for_database(
            os.path.join(self.bin_dir, "sample.exe"),
            self.config.config_dir,
            "abc",
            home=os.path.join(self.top, "home"),
        )
        return session

    def _loop(self, session=None):
        return AgentLoop(MagicMock(), self.registry, self.config, session or self.session)

    def _call(self, loop, name, answer=None, **arguments):
        tc = ToolCall(id="call", name=name, arguments=arguments)
        events, result = _drive(loop._execute_single_tool(tc), (lambda: loop.submit_tool_approval(answer)) if answer else None)
        return events, result

    def _asked(self, events):
        return [e for e in events if e.type == TurnEventType.TOOL_APPROVAL_REQUEST]

    def _write(self, name, text):
        with open(os.path.join(self.bin_dir, name), "w", encoding="utf-8") as f:
            f.write(text)

    def _read(self, name):
        with open(os.path.join(self.bin_dir, name), encoding="utf-8") as f:
            return f.read()


class TestConsent(_Base):
    def setUp(self):
        super().setUp()
        self._write("a.txt", "hello\n")

    def test_first_use_asks_and_says_what_allowing_means(self):
        events, _ = self._call(self._loop(), "read_file", answer="allow", file_path="a.txt")
        request = self._asked(events)[0]
        self.assertEqual(request.tool_name, "read_file")
        self.assertIn("Read a.txt", request.text)
        self.assertIn("for this chat", request.text)

    def test_allowing_runs_the_tool(self):
        _, result = self._call(self._loop(), "read_file", answer="allow", file_path="a.txt")
        self.assertIn("hello", result.content)
        self.assertFalse(result.is_error)

    def test_a_second_call_does_not_ask(self):
        loop = self._loop()
        self._call(loop, "read_file", answer="allow", file_path="a.txt")
        events, _ = self._call(loop, "read_file", file_path="a.txt")
        self.assertEqual(self._asked(events), [])

    def test_the_answer_outlives_the_loop(self):
        # A loop is built per user message; a grant held on it would ask every time.
        self._call(self._loop(), "read_file", answer="allow", file_path="a.txt")
        events, _ = self._call(self._loop(), "read_file", file_path="a.txt")
        self.assertEqual(self._asked(events), [])

    def test_another_chat_is_asked_for_itself(self):
        self._call(self._loop(), "read_file", answer="allow", file_path="a.txt")
        other = self._session()
        events, _ = self._call(self._loop(other), "read_file", answer="deny", file_path="a.txt")
        self.assertEqual(len(self._asked(events)), 1)

    def test_denying_blocks_the_tool(self):
        _, result = self._call(self._loop(), "read_file", answer="deny", file_path="a.txt")
        self.assertTrue(result.is_error)
        self.assertNotIn("hello", result.content)

    def test_denying_is_not_asked_again(self):
        self._call(self._loop(), "read_file", answer="deny", file_path="a.txt")
        events, result = self._call(self._loop(), "read_file", file_path="a.txt")
        self.assertEqual(self._asked(events), [])
        self.assertTrue(result.is_error)

    def test_denying_stops_declaring_the_tools_in_that_chat_only(self):
        loop = self._loop()
        self._call(loop, "read_file", answer="deny", file_path="a.txt")
        names = [t["function"]["name"] for t in loop._build_tools_schema(None, False)]
        self.assertNotIn("read_file", names)
        self.assertNotIn("read_file", loop._declared_tool_names())

        other = self._loop(self._session())
        names = [t["function"]["name"] for t in other._build_tools_schema(None, False)]
        self.assertIn("read_file", names)

    def test_undecided_tools_are_declared_or_first_use_could_never_happen(self):
        names = [t["function"]["name"] for t in self._loop()._build_tools_schema(None, False)]
        self.assertIn("read_file", names)

    def test_the_setting_means_no_question(self):
        self.config.filesystem_access = True
        events, result = self._call(self._loop(), "read_file", file_path="a.txt")
        self.assertEqual(self._asked(events), [])
        self.assertIn("hello", result.content)

    def test_the_setting_overrides_an_earlier_denial(self):
        self._call(self._loop(), "read_file", answer="deny", file_path="a.txt")
        self.config.filesystem_access = True
        _, result = self._call(self._loop(), "read_file", file_path="a.txt")
        self.assertIn("hello", result.content)
        self.assertIn("read_file", self._loop()._declared_tool_names())

    def test_always_allow_turns_on_the_setting_and_persists(self):
        self._call(self._loop(), "read_file", answer="allow_all", file_path="a.txt")
        self.assertTrue(self.config.filesystem_access)
        reloaded = RikuganConfig()
        reloaded._config_dir = self.config.config_dir
        reloaded.load()
        self.assertTrue(reloaded.filesystem_access)

    def test_always_allow_does_not_rewrite_stored_keys(self):
        # save() without the password would write encryption disabled and lose them.
        os.makedirs(self.config.config_dir)
        blob = {"encryption": {"enabled": True, "ciphertext": "c"}, "provider": {"api_key": ""}}
        with open(self.config.config_path, "w", encoding="utf-8") as f:
            json.dump(blob, f)
        self._call(self._loop(), "read_file", answer="allow_all", file_path="a.txt")
        with open(self.config.config_path, encoding="utf-8") as f:
            stored = json.load(f)
        self.assertEqual(stored["encryption"], blob["encryption"])
        self.assertIs(stored["filesystem_access"], True)

    def test_always_allowing_scripts_does_not_open_the_disk(self):
        # A separate question. The approval queue must be what answers it.
        loop = self._loop()
        loop._always_allow_scripts = True
        events, _ = self._call(loop, "read_file", answer="deny", file_path="a.txt")
        self.assertEqual(len(self._asked(events)), 1)

    def test_a_subagent_shares_the_answer(self):
        from rikugan.agent.subagent import SubagentRunner

        parent = self._loop()
        self._call(parent, "read_file", answer="allow", file_path="a.txt")
        runner = SubagentRunner(MagicMock(), self.registry, self.config, parent_loop=parent)
        captured = {}
        original = AgentLoop.run

        def capture(loop, message):
            captured["session"] = loop.session
            return iter(())

        AgentLoop.run = capture
        self.addCleanup(setattr, AgentLoop, "run", original)
        list(runner.run_task("look"))
        self.assertIs(captured["session"].files, self.session.files)


class TestModelCannotCallInternalTools(_Base):
    def test_restore_is_refused_as_unknown(self):
        self.config.filesystem_access = True
        self._write("a.txt", "precious")
        _, result = self._call(
            self._loop(), "restore_file_backup", file_path="a.txt", backup="", written_mtime_ns=0
        )
        self.assertTrue(result.is_error)
        self.assertIn("Unknown tool", result.content)
        self.assertEqual(self._read("a.txt"), "precious")


class TestUndoRoundTrip(_Base):
    def setUp(self):
        super().setUp()
        self.config.filesystem_access = True
        self._write("a.txt", "alpha\n")

    def _edit(self, loop, old, new):
        return self._call(loop, "edit_file", file_path="a.txt", old_string=old, new_string=new)

    def _undo(self, loop, command="/undo"):
        return list(loop._handle_undo_command(command))

    def test_an_edit_announces_itself(self):
        loop = self._loop()
        self._call(loop, "read_file", file_path="a.txt")
        events, _ = self._edit(loop, "alpha", "beta")
        recorded = [e for e in events if e.type == TurnEventType.MUTATION_RECORDED]
        self.assertEqual(len(recorded), 1)
        self.assertTrue(recorded[0].metadata["reversible"])

    def test_a_read_announces_nothing(self):
        events, _ = self._call(self._loop(), "read_file", file_path="a.txt")
        self.assertEqual([e for e in events if e.type == TurnEventType.MUTATION_RECORDED], [])

    def test_a_refused_edit_announces_nothing(self):
        events, _ = self._edit(self._loop(), "alpha", "beta")  # never read
        self.assertEqual([e for e in events if e.type == TurnEventType.MUTATION_RECORDED], [])

    def test_the_undo_runs_on_a_later_message(self):
        # The regression that started this: /undo is its own user message, so it
        # always runs on a new loop over the same session.
        first = self._loop()
        self._call(first, "read_file", file_path="a.txt")
        self._edit(first, "alpha", "beta")
        self.assertEqual(self._read("a.txt"), "beta\n")

        events = self._undo(self._loop())
        self.assertEqual(self._read("a.txt"), "alpha\n")
        self.assertEqual(self.session.mutation_log, [])
        self.assertIn("Undid 1", events[-1].text)

    def test_a_file_changed_since_does_not_block_older_undos(self):
        # Otherwise one external edit would stop undo working for good.
        loop = self._loop()
        self._call(loop, "write_file", file_path="b.txt", content="mine")
        self._call(loop, "read_file", file_path="a.txt")
        self._edit(loop, "alpha", "beta")
        self._write("a.txt", "the user's newer work\n")
        os.utime(os.path.join(self.bin_dir, "a.txt"), ns=(1, 1))

        events = self._undo(self._loop(), "/undo 2")

        self.assertEqual(self._read("a.txt"), "the user's newer work\n")
        self.assertFalse(os.path.exists(os.path.join(self.bin_dir, "b.txt")))
        self.assertIn("has changed since", events[-1].text)
        self.assertEqual(self.session.mutation_log, [])


class TestWholeTurnsThroughTheLoop(_Base):
    """The real loop, scripted model, real files: nothing here calls a tool directly."""

    def _run(self, session, message, responses, answer=None):
        loop = AgentLoop(MockProvider(responses=responses), self.registry, self.config, session)
        events = []
        for event in loop.run(message):
            events.append(event)
            if event.type == TurnEventType.TOOL_APPROVAL_REQUEST:
                loop.submit_tool_approval(answer)
        return events

    def test_ask_read_edit_then_undo_on_a_later_message(self):
        self._write("notes.txt", "alpha\nbeta\n")
        events = self._run(
            self.session,
            "fix the notes",
            [
                _tool_call_response("read_file", {"file_path": "notes.txt"}, "c1"),
                _tool_call_response(
                    "edit_file", {"file_path": "notes.txt", "old_string": "beta", "new_string": "BETA"}, "c2"
                ),
                _text_response("Changed beta."),
            ],
            answer="allow",
        )
        self.assertEqual(self._read("notes.txt"), "alpha\nBETA\n")
        self.assertEqual(len([e for e in events if e.type == TurnEventType.TOOL_APPROVAL_REQUEST]), 1)
        self.assertEqual(len([e for e in events if e.type == TurnEventType.MUTATION_RECORDED]), 1)
        results = [e for e in events if e.type == TurnEventType.TOOL_RESULT]
        self.assertFalse(any(e.tool_is_error for e in results))
        self.assertIn("1\talpha", results[0].tool_result)

        events = self._run(self.session, "/undo", [])
        self.assertEqual(self._read("notes.txt"), "alpha\nbeta\n")
        self.assertEqual(len([e for e in events if e.type == TurnEventType.MUTATION_UNDONE]), 1)

    def test_what_the_model_is_sent_keeps_a_large_files_handle(self):
        self.config.filesystem_access = True
        self._write("big.txt", "".join(f"line {n}\n" for n in range(20_000)))
        self._run(
            self.session,
            "look",
            [_tool_call_response("read_file", {"file_path": "big.txt"}), _text_response("ok")],
        )
        sent = self.session.get_messages_for_provider()
        results = [tr.content for msg in sent for tr in msg.tool_results]
        self.assertIn("artifact://1", results[0])
        self.assertIn("line 19999", results[0])

    def test_a_denial_reaches_the_model_and_leaves_the_file_alone(self):
        self._write("notes.txt", "alpha\n")
        events = self._run(
            self.session,
            "fix",
            [
                _tool_call_response("write_file", {"file_path": "new.txt", "content": "x"}),
                _text_response("Could not."),
            ],
            answer="deny",
        )
        results = [e for e in events if e.type == TurnEventType.TOOL_RESULT]
        self.assertTrue(results[0].tool_is_error)
        self.assertFalse(os.path.exists(os.path.join(self.bin_dir, "new.txt")))


class TestControllerGivesTheChatAWorkspace(_Base):
    def test_built_from_the_open_database_when_a_turn_starts(self):
        from rikugan.ui import session_controller_base as base

        controller = MagicMock()
        controller.config = self.config
        controller._db_instance_id = "xyz"
        controller._sessions = {"t1": self.session}
        controller._runners = {}
        controller.is_tab_running.return_value = False
        self.config.filesystem_extra_roots = [self.top]
        self.session.files.workspace = None
        binary = os.path.join(self.bin_dir, "sample.exe")
        with (
            patch.object(base, "get_database_path", return_value=binary + ".i64"),
            patch.object(base, "get_input_path", return_value=binary),
            patch.object(base, "AgentLoop"),
            patch.object(base, "BackgroundAgentRunner"),
        ):
            base.SessionControllerBase.start_agent(controller, "hi", tab_id="t1")

        workspace = self.session.files.workspace
        self.assertEqual(workspace.binary_root, self.bin_dir)
        self.assertIn(self.top, workspace.extra_roots)
        self.assertIn("xyz", workspace.scratch_root)
        self.assertIn(binary.casefold(), workspace.protected)


class TestBinaryNinjaServerLeavesFileToolsOn(unittest.TestCase):
    def test_it_replaces_analysis_tools_not_file_tools(self):
        # Its server has no way to touch files, so hiding ours would just lose them.
        from rikugan.binja import native_mcp

        registry = ToolRegistry()
        registry.register_module(filesystem)
        self.assertEqual(native_mcp.superseded_builtins(registry), [])


class TestShellSwitch(_Base):
    """Filesystem access includes the shell unless the config file says otherwise."""

    def setUp(self):
        super().setUp()
        from rikugan.tools import shell

        self.registry.register_module(shell)
        self.config.filesystem_access = True

    def test_offered_by_default(self):
        self.assertIn("run_shell", self._loop()._declared_tool_names())

    def test_switched_off_it_is_not_offered(self):
        self.config.filesystem_shell = False
        loop = self._loop()
        self.assertNotIn("run_shell", loop._declared_tool_names())
        self.assertNotIn("run_shell", [t["function"]["name"] for t in loop._build_tools_schema(None, False)])
        self.assertIn("read_file", loop._declared_tool_names())

    def test_switched_off_it_is_refused_even_if_named(self):
        self.config.filesystem_shell = False
        marker = os.path.join(self.bin_dir, "ran")
        _, result = self._call(self._loop(), "run_shell", command=f"touch {marker}")
        self.assertTrue(result.is_error)
        self.assertFalse(os.path.exists(marker))

    def test_the_first_use_prompt_mentions_the_shell(self):
        self.config.filesystem_access = False
        self._write("a.txt", "x")
        events, _ = self._call(self._loop(), "read_file", answer="allow", file_path="a.txt")
        self.assertIn("shell", self._asked(events)[0].text)

    def test_a_shell_call_shows_its_command_when_asking(self):
        self.config.filesystem_access = False
        events, _ = self._call(self._loop(), "run_shell", answer="deny", command="make all")
        self.assertIn("Run: make all", self._asked(events)[0].text)


class TestStopReachesTheShell(_Base):
    def test_cancelling_the_loop_stops_a_running_command(self):
        import threading
        import time

        from rikugan.tools import shell

        self.registry.register_module(shell)
        self.config.filesystem_access = True
        loop = self._loop()
        threading.Timer(0.3, loop.cancel).start()
        started = time.monotonic()
        _, result = self._call(loop, "run_shell", command="sleep 30", timeout=60)
        self.assertLess(time.monotonic() - started, 6)
        self.assertIn("cancelled by the user", result.content)


class TestLongToolsDoNotStarveOthers(unittest.TestCase):
    def test_other_tools_run_while_long_ones_hold_their_threads(self):
        import threading

        from rikugan.tools.base import tool

        release = threading.Event()
        self.addCleanup(release.set)

        @tool(category="x", long_running=True, timeout=10)
        def slow() -> str:
            """Hold a thread."""
            release.wait(10)
            return "done"

        @tool(category="x", timeout=2)
        def quick() -> str:
            """Return at once."""
            return "quick"

        registry = ToolRegistry()
        registry.register_function(slow)
        registry.register_function(quick)
        holders = [threading.Thread(target=registry.execute, args=("slow", {}), daemon=True) for _ in range(3)]
        for holder in holders:
            holder.start()
        self.assertEqual(registry.execute("quick", {}), "quick")


class TestOldResultsLastLonger(unittest.TestCase):
    def test_a_recent_file_read_keeps_its_budget(self):
        self.assertGreater(_result_limit("read_file", 1), 20_000)

    def test_a_file_read_outlasts_an_analysis_result(self):
        self.assertEqual(_result_limit("decompile_function", 12), 500)
        self.assertGreater(_result_limit("read_file", 12), 500)

    def test_eventually_it_shrinks_too(self):
        self.assertEqual(_result_limit("read_file", 100), 500)

    def test_other_tools_are_unchanged(self):
        self.assertEqual(_result_limit("decompile_function", 1), 8000)


class TestSystemPrompt(unittest.TestCase):
    def _prompt(self, names):
        from rikugan.agent.system_prompt import build_system_prompt

        return build_system_prompt(host_name="IDA Pro", tool_names=names)

    def test_described_when_available(self):
        self.assertIn("## Files on Disk", self._prompt(["read_file", "decompile_function"]))

    def test_not_described_otherwise(self):
        self.assertNotIn("## Files on Disk", self._prompt(["decompile_function"]))

    def test_the_shell_is_only_described_when_it_is_offered(self):
        self.assertNotIn("## Shell Commands", self._prompt(["read_file"]))
        self.assertIn("## Shell Commands", self._prompt(["read_file", "run_shell"]))

    def test_the_rule_against_running_the_binary_names_the_shell(self):
        self.assertIn("run_shell included", self._prompt(["decompile_function"]))

    def test_warns_against_instructions_inside_results(self):
        self.assertIn("<tool_result>", self._prompt(["read_file"]))


class TestSettings(unittest.TestCase):
    def test_the_label_is_exactly_what_was_asked_for(self):
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(__file__))), "rikugan/ui/settings_dialog.py")
        with open(path, encoding="utf-8") as f:
            source = f.read()
        self.assertIn('"Filesystem access: Agent can use disk files to code, edit or read"', source)
        self.assertIn("QCheckBox(FILESYSTEM_ACCESS_LABEL)", source)
        self.assertIn("self._config.filesystem_access = self._filesystem_access_cb.isChecked()", source)


class TestRegistryCarriesTheChat(unittest.TestCase):
    def test_a_handler_on_the_worker_thread_sees_the_active_session(self):
        from rikugan.core.chat_context import activate_session, current_session
        from rikugan.tools.base import tool

        @tool(category="x")
        def which_chat() -> str:
            """Report the active session."""
            session = current_session()
            return session.id if session else "none"

        registry = ToolRegistry()
        registry.register_function(which_chat)
        session = SessionState()
        with activate_session(session):
            self.assertEqual(registry.execute("which_chat", {}), session.id)
        self.assertEqual(registry.execute("which_chat", {}), "none")


if __name__ == "__main__":
    unittest.main()
